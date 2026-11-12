// SPDX-License-Identifier: GPL-2.0-only
/*
 * I2C controller on an OBMF-ICP channel
 *
 * Each I2C message, or a write followed by a read of the same target,
 * is one channel request.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/i2c.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/usb/obmf.h>

/* OBMF-ICP 1.0.0, 5.6.1, Table 26: payload byte 0 */
#define OBMF_I2C_READ 0
#define OBMF_I2C_WRITE 1
#define OBMF_I2C_WRITE_READ 3
#define OBMF_I2C_CMD_NO_STOP BIT(7)

/* OBMF-ICP 1.0.0, 5.6.1, Table 26: the bytes after byte 0 */
struct obmf_i2c_req {
	u8 address;
	__le16 read_length;
	u8 data[];
} __packed;

/* OBMF-ICP 1.0.0, 5.6.2, Table 28: the bytes after the echo */
struct obmf_i2c_resp {
	__le16 read_length;
	u8 reserved;
	u8 data[];
} __packed;

/* OBMF-ICP 1.0.0, 5.15.2, Table 79 */
struct obmf_i2c_cfg {
	u8 speed;
	u8 options;
} __packed;

#define OBMF_I2C_RETRIES 3

static const u32 obmf_i2c_speeds[] = { 100000, 400000, 1000000, 3400000 };

struct obmf_i2c {
	struct i2c_adapter adap;
	struct obmf_fdev *fdev;
	struct i2c_adapter_quirks quirks;
	struct obmf_i2c_req *req;
	struct obmf_i2c_resp *resp;
};

/* Documentation/i2c/fault-codes.rst has no -ECHRNG, -EPIPE or -EMSGSIZE */
static int obmf_i2c_errno(int err)
{
	switch (err) {
	case -ECHRNG:
	case -EPIPE:
		return -EIO;
	case -EMSGSIZE:
		return -EOPNOTSUPP;
	}
	return err;
}

static int obmf_i2c_request(struct obmf_i2c *i2c, u8 cmd, u16 addr,
			    const u8 *wbuf, u16 wlen, u8 *rbuf, u16 rlen)
{
	struct obmf_i2c_resp *resp = i2c->resp;
	struct obmf_i2c_req *req = i2c->req;
	size_t len;
	int ret;

	req->address = addr;
	req->read_length = cpu_to_le16(rlen);
	if (wlen)
		memcpy(req->data, wbuf, wlen);

	ret = obmf_request(i2c->fdev, cmd, req, struct_size(req, data, wlen),
			   resp, struct_size(resp, data, rlen), &len, 0);
	if (ret)
		return obmf_i2c_errno(ret);

	/* A write-only response may also end after the echo */
	if (!rlen && !len)
		return 0;
	if (len != struct_size(resp, data, rlen) ||
	    le16_to_cpu(resp->read_length) != rlen) {
		obmf_fdev_protocol_error(i2c->fdev);
		return -EPROTO;
	}
	if (rlen)
		memcpy(rbuf, resp->data, rlen);
	return 0;
}

/* Every message is checked before the first is sent */
static int obmf_i2c_check(struct i2c_msg *msgs, int num)
{
	int i;

	for (i = 0; i < num; i++) {
		if (msgs[i].flags & I2C_M_TEN)
			return -EAFNOSUPPORT;
		/* The SMBus emulation copies the client's PEC flag here */
		if (msgs[i].flags &
		    ~(I2C_M_RD | I2C_M_DMA_SAFE | I2C_CLIENT_PEC))
			return -EOPNOTSUPP;
		if (msgs[i].addr > 0x7f)
			return -EINVAL;
	}
	return 0;
}

static int obmf_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			 int num)
{
	struct obmf_i2c *i2c = i2c_get_adapdata(adap);
	struct i2c_msg *m, *r;
	int i, n, ret;
	u8 cmd;

	ret = obmf_i2c_check(msgs, num);
	if (ret)
		return ret;

	for (i = 0; i < num; i += n) {
		m = &msgs[i];
		r = i + 1 < num ? &msgs[i + 1] : NULL;
		if (!(m->flags & I2C_M_RD) && r && (r->flags & I2C_M_RD) &&
		    r->addr == m->addr) {
			n = 2;
			cmd = OBMF_I2C_WRITE_READ;
		} else {
			n = 1;
			r = (m->flags & I2C_M_RD) ? m : NULL;
			cmd = r ? OBMF_I2C_READ : OBMF_I2C_WRITE;
		}
		if (i + n < num)
			cmd |= OBMF_I2C_CMD_NO_STOP;

		if (r == m)
			ret = obmf_i2c_request(i2c, cmd, m->addr, NULL, 0,
					       m->buf, m->len);
		else
			ret = obmf_i2c_request(i2c, cmd, m->addr, m->buf,
					       m->len, r ? r->buf : NULL,
					       r ? r->len : 0);
		if (ret)
			return ret;
	}
	return num;
}

static u32 obmf_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | (I2C_FUNC_SMBUS_EMUL & ~I2C_FUNC_SMBUS_QUICK);
}

static const struct i2c_algorithm obmf_i2c_algo = {
	.xfer = obmf_i2c_xfer,
	.functionality = obmf_i2c_func,
};

static int obmf_i2c_cfg(struct obmf_i2c *i2c, struct device *dev)
{
	const struct obmf_channel_info *info = i2c->fdev->info;
	const struct obmf_i2c_cfg *cfg;
	u32 speed, freq;

	if (info->config_len < sizeof(*cfg))
		return dev_err_probe(dev, -EINVAL,
				     "configuration data too short\n");
	cfg = (const struct obmf_i2c_cfg *)info->config;
	if (cfg->speed >= ARRAY_SIZE(obmf_i2c_speeds))
		return dev_err_probe(dev, -EINVAL, "unknown bus speed %u\n",
				     cfg->speed);
	speed = obmf_i2c_speeds[cfg->speed];
	if (!device_property_read_u32(dev, "clock-frequency", &freq) &&
	    freq != speed)
		return dev_err_probe(dev, -EINVAL,
				     "clock-frequency %u, channel runs at %u\n",
				     freq, speed);
	/*
	 * The channel's bus recovery (options bit 0) is not offered to the
	 * I2C core: no status says the bus is held, and a recovery sent
	 * after a timeout can take a reply later than the core's hold as
	 * its own
	 */
	return 0;
}

static int obmf_i2c_probe(struct auxiliary_device *adev,
			  const struct auxiliary_device_id *id)
{
	struct obmf_fdev *fdev = to_obmf_fdev(adev);
	const struct obmf_channel_info *info = fdev->info;
	size_t head = 1 + sizeof(struct obmf_i2c_req);
	struct device *dev = &adev->dev;
	struct i2c_adapter *adap;
	struct obmf_i2c *i2c;
	size_t wmax, rmax;
	int ret;

	BUILD_BUG_ON(sizeof(struct obmf_i2c_req) != 3);
	BUILD_BUG_ON(sizeof(struct obmf_i2c_resp) != 3);

	ret = devm_obmf_enable(fdev);
	if (ret)
		return ret;

	if (info->max_req <= head || info->max_resp <= head)
		return dev_err_probe(dev, -EINVAL,
				     "channel limits %zu/%zu too small\n",
				     info->max_req, info->max_resp);
	wmax = min_t(size_t, info->max_req - head, U16_MAX);
	rmax = min_t(size_t, info->max_resp - head, U16_MAX);

	i2c = devm_kzalloc(dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;
	i2c->fdev = fdev;
	i2c->req = devm_kzalloc(dev, struct_size(i2c->req, data, wmax),
				GFP_KERNEL);
	i2c->resp = devm_kzalloc(dev, struct_size(i2c->resp, data, rmax),
				 GFP_KERNEL);
	if (!i2c->req || !i2c->resp)
		return -ENOMEM;

	ret = obmf_i2c_cfg(i2c, dev);
	if (ret)
		return ret;

	i2c->quirks.flags = I2C_AQ_NO_ZERO_LEN;
	i2c->quirks.max_write_len = wmax;
	i2c->quirks.max_read_len = rmax;

	adap = &i2c->adap;
	adap->owner = THIS_MODULE;
	adap->algo = &obmf_i2c_algo;
	adap->quirks = &i2c->quirks;
	/* A device that retries lost arbitration itself gets no more tries */
	adap->retries = info->quirks.i2c_dev_retries ? 0 : OBMF_I2C_RETRIES;
	adap->timeout = (adap->retries + 1) * HZ;
	adap->dev.parent = dev;
	device_set_node(&adap->dev, dev_fwnode(dev));
	obmf_fdev_label(fdev, adap->name, sizeof(adap->name));
	i2c_set_adapdata(adap, i2c);
	auxiliary_set_drvdata(adev, i2c);

	ret = devm_i2c_add_adapter(dev, adap);
	if (ret)
		return ret;
	return devm_obmf_publish(fdev, NULL);
}

static int obmf_i2c_suspend(struct device *dev)
{
	struct obmf_i2c *i2c = dev_get_drvdata(dev);

	i2c_mark_adapter_suspended(&i2c->adap);
	return 0;
}

static int obmf_i2c_resume(struct device *dev)
{
	struct obmf_i2c *i2c = dev_get_drvdata(dev);

	i2c_mark_adapter_resumed(&i2c->adap);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(obmf_i2c_pm, obmf_i2c_suspend, obmf_i2c_resume);

static const struct auxiliary_device_id obmf_i2c_ids[] = {
	{ .name = "obmf.i2c" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, obmf_i2c_ids);

static struct auxiliary_driver obmf_i2c_driver = {
	.probe = obmf_i2c_probe,
	.id_table = obmf_i2c_ids,
	.driver.pm = pm_sleep_ptr(&obmf_i2c_pm),
};
module_auxiliary_driver(obmf_i2c_driver);

MODULE_DESCRIPTION("I2C controller on an OBMF-ICP channel");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("OBMF");
