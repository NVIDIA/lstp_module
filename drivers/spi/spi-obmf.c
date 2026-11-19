// SPDX-License-Identifier: GPL-2.0-only
/*
 * SPI controller on an OBMF-ICP channel
 *
 * Each SPI transfer is one channel request. The chip select moves ride
 * in the request's command byte, so a message is one chip select window:
 * its first transfer asserts, its last one releases. What the channel
 * cannot express is refused before anything is sent.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/usb/obmf.h>

/* OBMF-ICP 1.0.0, 5.10.1, Table 50: payload byte 0 */
#define OBMF_SPI_READ 1
#define OBMF_SPI_WRITE 2
#define OBMF_SPI_WRITE_READ 3
#define OBMF_SPI_DEASSERT BIT(4)
#define OBMF_SPI_ASSERT BIT(5)
#define OBMF_SPI_CS GENMASK(7, 6)

/* OBMF-ICP 1.0.0, 5.10.1, Table 50: the bytes after byte 0 */
struct obmf_spi_req {
	__le16 write_size;
	__le16 read_size;
	u8 data[];
} __packed;

/* OBMF-ICP 1.0.0, 5.10.2, Table 51: the bytes after the echo */
struct obmf_spi_resp {
	__le16 read_size;
	u8 data[];
} __packed;

/* OBMF-ICP 1.0.0, 5.15.2, Table 84 */
#define OBMF_SPI_MAX_DEVICES 4
#define OBMF_SPI_READ_BPW GENMASK(23, 18)
#define OBMF_SPI_WRITE_BPW GENMASK(29, 24)

/*
 * Table 84 bits 0-17 are the SPI_* mode bits with the same numbers. The
 * request has no line count, no flow control and no transfer without a
 * chip select, so a channel set up for any of those is not served; the
 * SPI core splits a CS_WORD client's transfers itself.
 */
#define OBMF_SPI_WIRE_MODE                                               \
	(SPI_CPHA | SPI_CPOL | SPI_CS_HIGH | SPI_LSB_FIRST | SPI_3WIRE | \
	 SPI_LOOP | SPI_3WIRE_HIZ | SPI_RX_CPHA_FLIP | SPI_MOSI_IDLE_LOW)
#define OBMF_SPI_NO_MODE                                                   \
	(SPI_NO_CS | SPI_READY | SPI_TX_DUAL | SPI_TX_QUAD | SPI_RX_DUAL | \
	 SPI_RX_QUAD | SPI_CS_WORD | SPI_TX_OCTAL | SPI_RX_OCTAL)

/*
 * A transfer may take at most 400 ms on the wire (8 bits a byte), well
 * inside the default request timeout, so the core splits longer ones
 */
#define OBMF_SPI_BYTES_PER_HZ 20

struct obmf_spi {
	struct obmf_fdev *fdev;
	struct obmf_spi_req *req;
	struct obmf_spi_resp *resp;
	size_t max_len;
	u32 mode;
	bool cs_held;
	/* chip selects whose client is gone, a bit each */
	unsigned long gone;
};

/* A timeout the device reports is a timeout too; channel faults are -EIO */
static int obmf_spi_errno(int err)
{
	switch (err) {
	case -ETIME:
		return -ETIMEDOUT;
	case -ECHRNG:
	case -EPIPE:
		return -EIO;
	}
	return err;
}

static int obmf_spi_request(struct obmf_spi *s, u8 cmd, const void *tx,
			    void *rx, u16 len)
{
	struct obmf_spi_resp *resp = s->resp;
	struct obmf_spi_req *req = s->req;
	u16 wlen = tx ? len : 0;
	u16 rlen = rx ? len : 0;
	size_t got;
	int ret;

	req->write_size = cpu_to_le16(wlen);
	req->read_size = cpu_to_le16(rlen);
	if (wlen)
		memcpy(req->data, tx, wlen);

	ret = obmf_request(s->fdev, cmd, req, struct_size(req, data, wlen),
			   resp, struct_size(resp, data, rlen), &got, 0);
	if (ret)
		return obmf_spi_errno(ret);

	/* A write-only response may also end after the echo */
	if (!rlen && !got)
		return 0;
	if (got != struct_size(resp, data, rlen) ||
	    le16_to_cpu(resp->read_size) != rlen) {
		obmf_fdev_protocol_error(s->fdev);
		return -EPROTO;
	}
	if (rlen)
		memcpy(rx, resp->data, rlen);
	return 0;
}

/*
 * After a failed request the device may still hold the chip select, which
 * the SPI core's own release cannot reach. One write without data that
 * only de-asserts frees it; the failed command is never sent again, as it
 * may have reached the device. If this fails too, the next message's
 * first request asserts the chip select afresh. -ENODEV: nothing was
 * sent, as the device is gone, which the core has reported.
 */
static void obmf_spi_release(struct obmf_spi *s, struct spi_device *spi)
{
	u8 cmd = FIELD_PREP(OBMF_SPI_CS, spi_get_chipselect(spi, 0)) |
		 OBMF_SPI_DEASSERT | OBMF_SPI_WRITE;
	int ret;

	ret = obmf_spi_request(s, cmd, NULL, NULL, 0);
	if (ret && ret != -ENODEV)
		dev_warn_ratelimited(&spi->dev,
				     "chip select %u not released: %pe\n",
				     spi_get_chipselect(spi, 0), ERR_PTR(ret));
}

static int obmf_spi_transfer_one(struct spi_controller *ctlr,
				 struct spi_device *spi,
				 struct spi_transfer *xfer)
{
	struct obmf_spi *s = spi_controller_get_devdata(ctlr);
	u8 cmd = FIELD_PREP(OBMF_SPI_CS, spi_get_chipselect(spi, 0));
	int ret;

	/* A removed client's leftover: nothing is sent but a release */
	if (test_bit(spi_get_chipselect(spi, 0), &s->gone)) {
		if (s->cs_held)
			obmf_spi_release(s, spi);
		s->cs_held = false;
		return -ESHUTDOWN;
	}

	if (xfer->tx_buf && xfer->rx_buf)
		cmd |= OBMF_SPI_WRITE_READ;
	else if (xfer->tx_buf)
		cmd |= OBMF_SPI_WRITE;
	else
		cmd |= OBMF_SPI_READ;
	if (!s->cs_held)
		cmd |= OBMF_SPI_ASSERT;
	/* The last transfer always releases: no chip select outlives a message */
	if (xfer->cs_change || spi_transfer_is_last(ctlr, xfer))
		cmd |= OBMF_SPI_DEASSERT;

	ret = obmf_spi_request(s, cmd, xfer->tx_buf, xfer->rx_buf, xfer->len);
	s->cs_held = !ret && !(cmd & OBMF_SPI_DEASSERT);
	/* -ENODEV: the device is gone or stopping, nothing can be sent */
	if (ret && ret != -ENODEV)
		obmf_spi_release(s, spi);
	return ret;
}

static int obmf_spi_prepare_message(struct spi_controller *ctlr,
				    struct spi_message *msg)
{
	struct obmf_spi *s = spi_controller_get_devdata(ctlr);

	s->cs_held = false;
	return 0;
}

/*
 * The chip select moves only with a request, so the SPI core's own moves
 * around a transfer without data, and its delays after a release, would
 * not reach the wire: such messages are refused before anything is sent.
 * A transfer without data in the middle of a window, with only a delay,
 * is the one kind kept. User space can send such messages at will, so
 * the refusal logs at debug level only, as the core's own checks are
 * silent.
 */
static int obmf_spi_optimize_message(struct spi_message *msg)
{
	struct spi_transfer *xfer, *prev = NULL;
	const char *why = NULL;
	bool data, last;

	list_for_each_entry(xfer, &msg->transfers, transfer_list) {
		data = (xfer->tx_buf || xfer->rx_buf) && xfer->len;
		last = list_is_last(&xfer->transfer_list, &msg->transfers);
		if (xfer->cs_off)
			why = "transfer with the chip select off";
		else if (xfer->word_delay.value)
			why = "delay between words";
		else if (!data && (!prev || last || xfer->cs_change))
			why = "chip select move without data";
		else if (!data && prev->cs_change && xfer->delay.value)
			why = "delay before the chip select is asserted";
		else if (xfer->delay.value && xfer->cs_change)
			why = "delay after the chip select is released";
		else if (xfer->delay.value && last)
			why = "delay after the chip select is released";
		if (why) {
			dev_dbg(&msg->spi->dev, "%s: refused\n", why);
			return -EINVAL;
		}
		prev = xfer;
	}
	return 0;
}

/*
 * The channel's configuration is fixed: a device must use its mode on
 * the wire and run at its clock, and the chip select timing is the
 * device's own. The SPI core logs each refusal, and user space can ask
 * for another mode or clock at will through spidev: those reasons are
 * debug lines
 */
static int obmf_spi_setup(struct spi_device *spi)
{
	struct spi_controller *ctlr = spi->controller;
	struct obmf_spi *s = spi_controller_get_devdata(ctlr);
	u32 mode = spi->mode & OBMF_SPI_WIRE_MODE;

	if (mode != s->mode) {
		dev_dbg(&spi->dev, "mode %#x, channel runs %#x\n", mode,
			s->mode);
		return -EINVAL;
	}
	if (spi->max_speed_hz < ctlr->max_speed_hz) {
		dev_dbg(&spi->dev, "%u Hz, channel runs at %u Hz\n",
			spi->max_speed_hz, ctlr->max_speed_hz);
		return -EINVAL;
	}
	if (spi->cs_setup.value || spi->cs_hold.value) {
		dev_err(&spi->dev,
			"chip select setup and hold times not supported\n");
		return -EINVAL;
	}
	clear_bit(spi_get_chipselect(spi, 0), &s->gone);
	return 0;
}

/*
 * The SPI core calls this after a client's driver has gone, and at
 * unbind removes every client before it waits a few seconds for its
 * queue to drain. Messages the client left queued then fail at once
 * instead of going out one request at a time to a device that may answer
 * slowly. The bit is set and tested without a lock: a transfer that read
 * it before it was set is the request in flight, and the next one sees
 * it; setup() clears it for the chip select's next client.
 */
static void obmf_spi_cleanup(struct spi_device *spi)
{
	struct obmf_spi *s = spi_controller_get_devdata(spi->controller);

	set_bit(spi_get_chipselect(spi, 0), &s->gone);
}

#if IS_ENABLED(CONFIG_SPI_OBMF_SPIDEV)
/*
 * For development and debugging only, not for production: a board
 * describes its SPI clients in its device tree or ACPI tables
 */
static bool auto_bind_spidev;
module_param(auto_bind_spidev, bool, 0444);
MODULE_PARM_DESC(auto_bind_spidev,
		 "For development and debugging only, not for production: a spidev device on each chip select of a channel whose firmware description has no SPI clients; describe clients in the device tree or ACPI tables instead (default: off)");

static int obmf_spi_count(struct device *dev, void *data)
{
	(*(unsigned int *)data)++;
	return 0;
}

/*
 * One spidev client per chip select, at the channel's mode and clock,
 * when the firmware describes no client: no child node, and no client
 * found by the SPI core (ACPI clients may be elsewhere in the
 * namespace). spi_new_device() marks the client's other chip select
 * slots unused, which some kernels need to add a second client. spidev
 * binds only through its ID table, so the client names it by override
 * and is attached again. The clients go with the controller.
 */
static void obmf_spi_add_spidev(struct spi_controller *ctlr)
{
	struct obmf_spi *s = spi_controller_get_devdata(ctlr);
	struct spi_board_info info = {
		.modalias = "spidev",
		.max_speed_hz = ctlr->max_speed_hz,
		.mode = s->mode,
	};
	struct spi_device *spi;
	unsigned int n = 0;
	int ret;
	u8 cs;

	if (!auto_bind_spidev)
		return;
	device_for_each_child(&ctlr->dev, &n, obmf_spi_count);
	if (n || device_get_child_node_count(&ctlr->dev))
		return;

	for (cs = 0; cs < ctlr->num_chipselect; cs++) {
		info.chip_select = cs;
		spi = spi_new_device(ctlr, &info);
		if (!spi) {
			dev_warn(&ctlr->dev, "no spidev on chip select %u\n",
				 cs);
			continue;
		}
#ifndef OBMF_COMPAT_SPI_DRIVER_OVERRIDE
		ret = device_set_driver_override(&spi->dev, "spidev");
#else
		ret = driver_set_override(&spi->dev, &spi->driver_override,
					  "spidev", strlen("spidev"));
#endif
		if (!ret)
			ret = device_attach(&spi->dev);
		if (ret < 0) {
			dev_warn(&ctlr->dev,
				 "no spidev on chip select %u: %pe\n", cs,
				 ERR_PTR(ret));
			spi_unregister_device(spi);
		}
	}
}
#else
static void obmf_spi_add_spidev(struct spi_controller *ctlr)
{
}
#endif

static size_t obmf_spi_max_transfer_size(struct spi_device *spi)
{
	struct obmf_spi *s = spi_controller_get_devdata(spi->controller);

	return s->max_len;
}

/* Table 84: the channel's settings, and what this driver can serve */
static int obmf_spi_cfg(struct spi_controller *ctlr, struct device *dev)
{
	struct obmf_spi *s = spi_controller_get_devdata(ctlr);
	const struct obmf_channel_info *info = s->fdev->info;
	const struct obmf_spi_cfg *cfg;
	u32 flags, rd, wr, lo;

	if (info->config_len < sizeof(*cfg))
		return dev_err_probe(dev, -EINVAL,
				     "configuration data too short\n");
	cfg = (const struct obmf_spi_cfg *)info->config;
	flags = le32_to_cpu(cfg->flags);
	rd = le32_to_cpu(cfg->rd_max_speed_hz);
	wr = le32_to_cpu(cfg->wr_max_speed_hz);
	lo = min(rd, wr);

	if (!cfg->num_devices || cfg->num_devices > OBMF_SPI_MAX_DEVICES)
		return dev_err_probe(dev, -EINVAL, "%u devices\n",
				     cfg->num_devices);
	if (lo < OBMF_SPI_BYTES_PER_HZ)
		return dev_err_probe(dev, -EINVAL, "clock %u/%u Hz\n", rd, wr);
	if (flags & OBMF_SPI_NO_MODE)
		return dev_err_probe(dev, -EINVAL, "mode %#x not supported\n",
				     (u32)(flags & OBMF_SPI_NO_MODE));
	/* 0 is 8 bits; how wider words sit in the data is not specified */
	if ((FIELD_GET(OBMF_SPI_READ_BPW, flags) | 8) != 8 ||
	    (FIELD_GET(OBMF_SPI_WRITE_BPW, flags) | 8) != 8)
		return dev_err_probe(dev, -EINVAL,
				     "%lu/%lu bits per word not supported\n",
				     FIELD_GET(OBMF_SPI_READ_BPW, flags),
				     FIELD_GET(OBMF_SPI_WRITE_BPW, flags));

	s->mode = flags & OBMF_SPI_WIRE_MODE;
	s->max_len = min3(info->max_req - 1 - sizeof(*s->req),
			  info->max_resp - 1 - sizeof(*s->resp),
			  (size_t)U16_MAX);
	s->max_len = min_t(size_t, s->max_len, lo / OBMF_SPI_BYTES_PER_HZ);
	ctlr->num_chipselect = cfg->num_devices;
	ctlr->mode_bits = s->mode;
	ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
	/* One clock: a device rated below either direction's is refused */
	ctlr->max_speed_hz = max(rd, wr);
	ctlr->min_speed_hz = ctlr->max_speed_hz;
	return 0;
}

static int obmf_spi_probe(struct auxiliary_device *adev,
			  const struct auxiliary_device_id *id)
{
	struct obmf_fdev *fdev = to_obmf_fdev(adev);
	const struct obmf_channel_info *info = fdev->info;
	struct device *dev = &adev->dev;
	struct spi_controller *ctlr;
	struct obmf_spi *s;
	int ret;

	BUILD_BUG_ON(sizeof(struct obmf_spi_req) != 4);
	BUILD_BUG_ON(sizeof(struct obmf_spi_resp) != 2);
	BUILD_BUG_ON(sizeof(struct obmf_spi_cfg) != 20);
	BUILD_BUG_ON(OBMF_SPI_MAX_DEVICES > BITS_PER_LONG);

	ret = devm_obmf_enable(fdev);
	if (ret)
		return ret;

	if (info->max_req <= 1 + sizeof(struct obmf_spi_req) ||
	    info->max_resp <= 1 + sizeof(struct obmf_spi_resp))
		return dev_err_probe(dev, -EINVAL,
				     "channel limits %zu/%zu too small\n",
				     info->max_req, info->max_resp);

	ctlr = devm_spi_alloc_host(dev, sizeof(*s));
	if (!ctlr)
		return -ENOMEM;
	s = spi_controller_get_devdata(ctlr);
	s->fdev = fdev;
	ret = obmf_spi_cfg(ctlr, dev);
	if (ret)
		return ret;
	s->req = devm_kzalloc(dev, struct_size(s->req, data, s->max_len),
			      GFP_KERNEL);
	s->resp = devm_kzalloc(dev, struct_size(s->resp, data, s->max_len),
			       GFP_KERNEL);
	if (!s->req || !s->resp)
		return -ENOMEM;

	ctlr->bus_num = -1;
	ctlr->setup = obmf_spi_setup;
	ctlr->cleanup = obmf_spi_cleanup;
	ctlr->optimize_message = obmf_spi_optimize_message;
	ctlr->prepare_message = obmf_spi_prepare_message;
	ctlr->transfer_one = obmf_spi_transfer_one;
	ctlr->max_transfer_size = obmf_spi_max_transfer_size;
	device_set_node(&ctlr->dev, dev_fwnode(dev));
	auxiliary_set_drvdata(adev, ctlr);

	/*
	 * At unbind the clients go first, each followed by
	 * obmf_spi_cleanup(), so when the SPI core waits for its queue only
	 * the request in flight and at most one release are left to send
	 */
	ret = devm_spi_register_controller(dev, ctlr);
	if (ret)
		return ret;
	obmf_spi_add_spidev(ctlr);
	dev_dbg(dev, "%u devices, %u Hz, mode %#x, %zu bytes a transfer\n",
		ctlr->num_chipselect, ctlr->max_speed_hz, s->mode, s->max_len);
	return devm_obmf_publish(fdev, NULL);
}

static int obmf_spi_suspend(struct device *dev)
{
	return spi_controller_suspend(dev_get_drvdata(dev));
}

static int obmf_spi_resume(struct device *dev)
{
	return spi_controller_resume(dev_get_drvdata(dev));
}

static DEFINE_SIMPLE_DEV_PM_OPS(obmf_spi_pm, obmf_spi_suspend, obmf_spi_resume);

static const struct auxiliary_device_id obmf_spi_ids[] = {
	{ .name = "obmf.spi" },
	{}
};
MODULE_DEVICE_TABLE(auxiliary, obmf_spi_ids);

static struct auxiliary_driver obmf_spi_driver = {
	.probe = obmf_spi_probe,
	.id_table = obmf_spi_ids,
	.driver.pm = pm_sleep_ptr(&obmf_spi_pm),
};
module_auxiliary_driver(obmf_spi_driver);

MODULE_DESCRIPTION("SPI controller on an OBMF-ICP channel");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("OBMF");
