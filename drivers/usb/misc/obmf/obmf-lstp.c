// SPDX-License-Identifier: GPL-2.0-only
/*
 * OBMF-ICP over USB: the LSTP dialect
 *
 * A device that speaks LSTP 1.0 is served by the OBMF-ICP core: each
 * message is translated at the edge, channel 0 is read with READ_CONFIG,
 * and the channel header bytes are answered here. Channel 0, the I2C
 * controller, GPIO, UART and SPI channels are translated, GPIO without
 * Get Interrupt Config; channels of the other types get a record and no
 * function device.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/minmax.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include "obmf-core.h"
#include "obmf-dialect.h"
#include "obmf_usb_lstp.h"

/* LSTP 1.0, 3.1.1 */
#define OBMF_LSTP_SUCCESS 0x00
#define OBMF_LSTP_BUSY 0x03
#define OBMF_LSTP_NACK 0x04
#define OBMF_LSTP_ARB_LOST 0x05
#define OBMF_LSTP_NOT_SUPP 0x06
#define OBMF_LSTP_TOO_LARGE 0x07

/* LSTP 1.0, 3.2 */
#define OBMF_LSTP_TYPE_MGMT 0x00
#define OBMF_LSTP_TYPE_SPI 0x01
#define OBMF_LSTP_TYPE_GPIO 0x02
#define OBMF_LSTP_TYPE_I2C 0x03
#define OBMF_LSTP_TYPE_UART 0x04
#define OBMF_LSTP_TYPE_IPMI 0x05

/* LSTP 1.0, 4.1.1 and 4.1.2 */
#define OBMF_LSTP_VERSION 1
#define OBMF_LSTP_READ_CONFIG 0x08
#define OBMF_LSTP_CFG_END 0x7fff

/* The configuration bytes one discovery pass reads, as the native pass */
#define OBMF_LSTP_CFG_TOTAL SZ_256K

/*
 * Short slices, a channel's and a pass's: one 48-byte GPIO entry each
 * (LSTP 1.0, 4.3.1.4)
 */
#define OBMF_LSTP_CFG_SHORT_MAX DIV_ROUND_UP(OBMF_LSTP_CFG_END, 48)
#define OBMF_LSTP_PASS_SHORT_MAX (OBMF_LSTP_CFG_TOTAL / 48)

/* LSTP 1.0, 4.4.2: bit 6 of the command asks for no STOP */
#define OBMF_LSTP_I2C_BUS_RECOVERY 0x00
#define OBMF_LSTP_I2C_READ 0x01
#define OBMF_LSTP_I2C_WRITE 0x02
#define OBMF_LSTP_I2C_WRITE_READ 0x04
#define OBMF_LSTP_I2C_BLOCK_READ 0x05
#define OBMF_LSTP_I2C_NO_STOP BIT(6)

/* LSTP 1.0, 4.5.2: the command byte, flags [6:3] and command [1:0] */
#define OBMF_LSTP_UART_FLAGS GENMASK(6, 3)
#define OBMF_LSTP_UART_CMD GENMASK(1, 0)
#define OBMF_LSTP_UART_WRITE 0

/* OBMF-ICP 1.0.0, 5.6.1, Table 26: payload byte 0, then address and count */
#define OBMF_I2C_CMD GENMASK(6, 0)
#define OBMF_I2C_NO_STOP BIT(7)
#define OBMF_I2C_READ 0
#define OBMF_I2C_WRITE 1
#define OBMF_I2C_BLOCK_READ 2
#define OBMF_I2C_WRITE_READ 3
#define OBMF_I2C_HEAD 4

/*
 * OBMF-ICP 1.0.0, 5.10.1, Table 50: payload byte 0, then the write and
 * read sizes; 5.10.2, Table 51: the echo, then the read size. LSTP 1.0,
 * 4.2.2 has the same command and chip select bits for CS0 and CS1.
 */
#define OBMF_SPI_CMD GENMASK(3, 0)
#define OBMF_SPI_CS GENMASK(7, 6)
#define OBMF_SPI_READ 1
#define OBMF_SPI_WRITE 2
#define OBMF_SPI_WRITE_READ 3
#define OBMF_SPI_HEAD 5
#define OBMF_SPI_RESP_HEAD 3

/* OBMF-ICP 1.0.0, 5.5.1, Table 22 and 5.5.2, Table 23 */
#define OBMF_SERIAL_EVENTS GENMASK(3, 0)
#define OBMF_SERIAL_NACK BIT(0)
#define OBMF_SERIAL_HEAD 3

/* OBMF-ICP 1.0.0, 5.15, Table 65; 5.15.1, Table 69; 5.15.2, Table 79 */
#define OBMF_CH_HEALTH_OK 1
#define OBMF_I2C_RECOVERY_FAILED BIT(2)
#define OBMF_I2C_RECOVERY_SUCCESS BIT(3)
#define OBMF_I2C_TRIGGER_RECOVERY BIT(2)
#define OBMF_I2C_CLEAR_STATUS BIT(3)
#define OBMF_I2C_OPT_BUS_RECOVERY BIT(0)

/* LSTP 1.0, 4.1.2: READ_CONFIG request */
struct obmf_lstp_read_config_req {
	u8 channel;
	__le16 offset;
	__le16 length;
} __packed;

/* LSTP 1.0, 4.1.2: the base fields of every READ_CONFIG response */
struct obmf_lstp_cfg_base {
	u8 type;
	u8 flags;
	u8 name[16];
} __packed;

/* LSTP 1.0, 4.1.1 */
struct obmf_lstp_mgmt_cfg {
	u8 version;
	u8 num_channels;
} __packed;

/* LSTP 1.0, 4.4.1 */
struct obmf_lstp_i2c_cfg {
	u8 speed;
	__le16 drive_strength;
	__le16 slew_rate;
	__le16 debounce;
	__le16 glitch_filter;
	u8 bus_low_timeout;
	u8 arbitration_retries;
} __packed;

/* LSTP 1.0, 4.2.1: the electrical fields after the speed are not used */
struct obmf_lstp_spi_cfg {
	u8 num_devices;
	__le32 speed;
} __packed;

/* LSTP 1.0, 4.2.2: the command's chip select bit names CS0 or CS1 */
#define OBMF_LSTP_SPI_CS_MAX 2

/* LSTP 1.0, 4.5.1; the bytes before drive_strength are OBMF-ICP Table 78 */
struct obmf_lstp_uart_cfg {
	__le32 speed;
	u8 data_bits;
	u8 stop_bits;
	u8 line_control;
	__le16 drive_strength;
	__le16 slew_rate;
	__le16 debounce;
	__le16 glitch_filter;
} __packed;

/*
 * LSTP 1.0, 4.3.1.1 and 4.3.1.2: one GPIO of a channel config block; its
 * line fields are those of OBMF-ICP 1.0.0, 5.15.2, Table 77
 */
struct obmf_lstp_gpio_cfg {
	struct obmf_gpio_line_cfg line;
	u8 led_support;
	u8 reserved[2];
} __packed;

/* LSTP 1.0, 4.3.1.2 */
#define OBMF_LSTP_GPIO_OUT 0
#define OBMF_LSTP_GPIO_IN 1
#define OBMF_LSTP_GPIO_LOW 0
#define OBMF_LSTP_GPIO_HIGH 1
#define OBMF_LSTP_GPIO_Z 2
#define OBMF_LSTP_GPIO_OPEN_DRAIN 1
#define OBMF_LSTP_GPIO_OPEN_SOURCE 2
#define OBMF_LSTP_GPIO_BIAS_MAX 2
#define OBMF_LSTP_GPIO_LED_HOLD 1
#define OBMF_LSTP_GPIO_LED_INTERPOLATE 2

/* LSTP 1.0, 4.3.1.4: at most 255 blocks of at most 255 GPIOs */
#define OBMF_LSTP_GPIO_BLOCKS 255

/* LSTP 1.0, 4.3.2 */
#define OBMF_LSTP_GPIO_GET_VALUE 0x00
#define OBMF_LSTP_GPIO_SET_VALUE 0x01
#define OBMF_LSTP_GPIO_SET_IRQ_CONFIG 0x03
#define OBMF_LSTP_GPIO_IRQ_EVENT 0x04

/* LSTP 1.0, 4.3.2: the SET_IRQ_CONFIG type of each Table 18 trigger */
static const u8 obmf_lstp_irq_types[] = {
	[OBMF_GPIO_IRQ_DISABLE] = 0x00,	   [OBMF_GPIO_IRQ_RISING] = 0x01,
	[OBMF_GPIO_IRQ_FALLING] = 0x02,	   [OBMF_GPIO_IRQ_BOTH] = 0x03,
	[OBMF_GPIO_IRQ_LEVEL_HIGH] = 0x04, [OBMF_GPIO_IRQ_LEVEL_LOW] = 0x05,
};

/* LSTP 1.0, 2: a message is one packet of at most 512 bytes */
#define OBMF_LSTP_MAX_MSG 512
#define OBMF_LSTP_GPIO_GET_MAX ((OBMF_LSTP_MAX_MSG - OBMF_HDR_LEN) / 2)

/**
 * struct obmf_lstp_ch - LSTP state of one channel
 * @type: the LSTP channel type
 * @flags: the channel flags read at discovery
 * @recovery: I2C: the sticky recovery result bits, under ch0_lock
 * @tx_count: UART: characters in the write in flight, set by encode
 *	before the slot is armed
 * @gpio_count: GPIO: lines of the Get in flight, set by encode before the
 *	slot is armed
 * @gpio_index: GPIO: their indices, in request order
 * @cfg_len: length of the channel configuration as read
 */
struct obmf_lstp_ch {
	u8 type;
	u8 flags;
	u8 recovery;
	u16 tx_count;
	u16 gpio_count;
	u16 gpio_index[OBMF_LSTP_GPIO_GET_MAX];
	size_t cfg_len;
};

static void obmf_lstp_hdr(u8 *buf, u8 channel, u8 status, size_t size)
{
	struct obmf_hdr *hdr = (struct obmf_hdr *)buf;

	hdr->channel = channel;
	hdr->status = status;
	hdr->size = cpu_to_le16(size);
}

/*
 * The OBMF-ICP status for an LSTP status (OBMF-ICP Tables 4 and 25): an
 * unsupported command is an invalid one, a lost arbitration off I2C a
 * permanent error. The errno of the ones with no exact OBMF-ICP code
 * comes from obmf_lstp_status_to_errno().
 */
static u8 obmf_lstp_icp_status(const struct obmf_channel *ch, u8 st)
{
	if (ch->info.type == OBMF_TYPE_I2C_CONTROLLER) {
		if (st == OBMF_LSTP_NACK)
			return OBMF_STATUS_I2C_NACK;
		if (st == OBMF_LSTP_ARB_LOST)
			return OBMF_STATUS_I2C_ARB_LOST;
	}
	switch (st) {
	case OBMF_LSTP_ARB_LOST:
		return OBMF_STATUS_PERMANENT_ERROR;
	case OBMF_LSTP_NOT_SUPP:
		return OBMF_STATUS_INVALID;
	case OBMF_LSTP_TOO_LARGE:
		return OBMF_STATUS_SIZE_UNSUPPORTED;
	}
	return st;
}

/* Two LSTP statuses can give one OBMF-ICP status: the log has both */
static u8 obmf_lstp_status(const struct obmf_channel *ch, u8 st)
{
	u8 icp = obmf_lstp_icp_status(ch, st);

	if (st)
		dev_dbg(ch->core->dev,
			"channel %u: LSTP status %#x, OBMF-ICP %#x\n",
			ch->info.number, st, icp);
	return icp;
}

/* The LSTP command for each OBMF-ICP I2C command */
static const u8 obmf_lstp_i2c_cmds[] = {
	[OBMF_I2C_READ] = OBMF_LSTP_I2C_READ,
	[OBMF_I2C_WRITE] = OBMF_LSTP_I2C_WRITE,
	[OBMF_I2C_BLOCK_READ] = OBMF_LSTP_I2C_BLOCK_READ,
	[OBMF_I2C_WRITE_READ] = OBMF_LSTP_I2C_WRITE_READ,
};

/*
 * OBMF-ICP {command, address, read count, data} becomes LSTP {address,
 * data} for a write and {address, read count, data} for the others; the
 * command moves to the header (LSTP 1.0, 4.4.2)
 */
static ssize_t obmf_lstp_i2c_encode(u8 *buf, size_t len)
{
	size_t plen = len - OBMF_HDR_LEN;
	u8 *p = buf + OBMF_HDR_LEN;
	size_t dlen;
	u16 rlen;
	u8 op, cmd;

	if (plen < OBMF_I2C_HEAD)
		return -EINVAL;
	op = FIELD_GET(OBMF_I2C_CMD, p[0]);
	if (op >= ARRAY_SIZE(obmf_lstp_i2c_cmds))
		return -EINVAL;
	rlen = get_unaligned_le16(p + 2);
	dlen = plen - OBMF_I2C_HEAD;
	if ((op == OBMF_I2C_READ && dlen) || (op == OBMF_I2C_WRITE && rlen) ||
	    (op == OBMF_I2C_BLOCK_READ && rlen > 1))
		return -EINVAL;

	cmd = obmf_lstp_i2c_cmds[op];
	if (p[0] & OBMF_I2C_NO_STOP)
		cmd |= OBMF_LSTP_I2C_NO_STOP;
	if (op == OBMF_I2C_WRITE) {
		p[0] = p[1];
		memmove(p + 1, p + OBMF_I2C_HEAD, dlen);
		plen = 1 + dlen;
	} else {
		memmove(p, p + 1, --plen);
	}
	obmf_lstp_hdr(buf, buf[0], cmd, plen);
	return OBMF_HDR_LEN + plen;
}

/*
 * An LSTP I2C response carries the data only; OBMF-ICP Table 28 wants
 * {echo, read count, reserved, data}, or the echo alone on an error. The
 * echo is the command without its no-STOP bit.
 */
static ssize_t obmf_lstp_i2c_decode(struct obmf_channel *ch, const u8 *in,
				    size_t len, u8 *out, size_t cap,
				    u8 pending_cmd)
{
	size_t dlen = len - OBMF_HDR_LEN;
	u8 st = in[1] & OBMF_HDR_STATUS;
	u8 *p = out + OBMF_HDR_LEN;
	size_t plen = 1;

	if (!st) {
		if (FIELD_GET(OBMF_I2C_CMD, pending_cmd) == OBMF_I2C_WRITE &&
		    dlen)
			return -EPROTO;
		plen = OBMF_I2C_HEAD + dlen;
	}
	if (OBMF_HDR_LEN + plen > cap)
		return -EPROTO;
	p[0] = FIELD_GET(OBMF_I2C_CMD, pending_cmd);
	if (!st) {
		put_unaligned_le16(dlen, p + 1);
		p[3] = 0;
		memcpy(p + OBMF_I2C_HEAD, in + OBMF_HDR_LEN, dlen);
	}
	obmf_lstp_hdr(out, in[0], OBMF_HDR_RQRESP | obmf_lstp_status(ch, st),
		      plen);
	return OBMF_HDR_LEN + plen;
}

/*
 * OBMF-ICP {command, write size, read size, data} (Table 50) becomes the
 * LSTP command in the header and the data, or the 4-byte read length for
 * a READ (LSTP 1.0, 4.2.2). LSTP has no posted write, no CS2 or CS3 (the
 * chip select field would reach the request/response bit) and a
 * WRITE_READ that reads as many bytes as it writes; a size that does not
 * match the data is refused too.
 */
static ssize_t obmf_lstp_spi_encode(u8 *buf, size_t len)
{
	size_t plen = len - OBMF_HDR_LEN;
	u8 *p = buf + OBMF_HDR_LEN;
	u16 wlen, rlen;
	u8 cmd;

	if (plen < OBMF_SPI_HEAD)
		return -EINVAL;
	cmd = p[0];
	wlen = get_unaligned_le16(p + 1);
	rlen = get_unaligned_le16(p + 3);
	if (FIELD_GET(OBMF_SPI_CS, cmd) >= OBMF_LSTP_SPI_CS_MAX ||
	    plen - OBMF_SPI_HEAD != wlen)
		return -EINVAL;

	switch (FIELD_GET(OBMF_SPI_CMD, cmd)) {
	case OBMF_SPI_READ:
		/* No read length 0: LSTP gives it no meaning */
		if (wlen || !rlen)
			return -EINVAL;
		put_unaligned_le32(rlen, p);
		plen = sizeof(__le32);
		break;
	case OBMF_SPI_WRITE:
		if (rlen)
			return -EINVAL;
		memmove(p, p + OBMF_SPI_HEAD, wlen);
		plen = wlen;
		break;
	case OBMF_SPI_WRITE_READ:
		if (rlen != wlen)
			return -EINVAL;
		memmove(p, p + OBMF_SPI_HEAD, wlen);
		plen = wlen;
		break;
	default:
		return -EINVAL;
	}
	obmf_lstp_hdr(buf, buf[0], cmd, plen);
	return OBMF_HDR_LEN + plen;
}

/*
 * An LSTP SPI response carries the read data only; OBMF-ICP Table 51
 * wants {echo, read size, data}, or the echo alone on an error. A WRITE
 * reads nothing: LSTP 1.0, 4.2.2 gives its response as the header alone,
 * but LSTP firmware has been seen to add a byte. The status decides and
 * the data is dropped: a response longer than the host asked for is
 * accepted, for forward compatibility within a protocol version.
 */
static ssize_t obmf_lstp_spi_decode(struct obmf_channel *ch, const u8 *in,
				    size_t len, u8 *out, size_t cap,
				    u8 pending_cmd)
{
	u8 op = FIELD_GET(OBMF_SPI_CMD, pending_cmd);
	size_t dlen = len - OBMF_HDR_LEN;
	u8 st = in[1] & OBMF_HDR_STATUS;
	u8 *p = out + OBMF_HDR_LEN;
	size_t plen = 1;

	if (!st && op == OBMF_SPI_WRITE && dlen) {
		dev_dbg_ratelimited(ch->core->dev,
				    "channel %u: write, %zu bytes ignored\n",
				    ch->info.number, dlen);
		dlen = 0;
	}
	if (!st)
		plen = OBMF_SPI_RESP_HEAD + dlen;
	if (OBMF_HDR_LEN + plen > cap)
		return -EPROTO;
	p[0] = op;
	if (!st) {
		put_unaligned_le16(dlen, p + 1);
		memcpy(p + OBMF_SPI_RESP_HEAD, in + OBMF_HDR_LEN, dlen);
	}
	obmf_lstp_hdr(out, in[0], OBMF_HDR_RQRESP | obmf_lstp_status(ch, st),
		      plen);
	return OBMF_HDR_LEN + plen;
}

/*
 * OBMF-ICP {events, count, data} (Table 22) becomes the LSTP UART WRITE
 * command with the events in its flags, and the data (LSTP 1.0, 4.5.2)
 */
static ssize_t obmf_lstp_uart_encode(struct obmf_channel *ch, u8 *buf,
				     size_t len)
{
	struct obmf_lstp_ch *lc = ch->dialect_data;
	size_t plen = len - OBMF_HDR_LEN;
	u8 *p = buf + OBMF_HDR_LEN;
	u8 ev = p[0];

	if (plen < OBMF_SERIAL_HEAD || ev & ~OBMF_SERIAL_EVENTS ||
	    get_unaligned_le16(p + 1) != plen - OBMF_SERIAL_HEAD)
		return -EINVAL;
	plen -= OBMF_SERIAL_HEAD;
	memmove(p, p + OBMF_SERIAL_HEAD, plen);
	obmf_lstp_hdr(buf, buf[0],
		      FIELD_PREP(OBMF_LSTP_UART_FLAGS, ev) |
			      OBMF_LSTP_UART_WRITE,
		      plen);
	lc->tx_count = plen;
	return OBMF_HDR_LEN + plen;
}

/*
 * The host's answer {NACK, accepted} (Table 23) becomes the LSTP status:
 * ACK, or NACK for a refusal or a partial accept, which the device may
 * send again. An error status refuses the request itself, with or without
 * a body: NOT_SUPP (LSTP 1.0, 3.1.1).
 */
static ssize_t obmf_lstp_uart_answer(u8 *buf, size_t len)
{
	u8 st = buf[1] & OBMF_HDR_STATUS;
	u8 *p = buf + OBMF_HDR_LEN;

	if (st)
		st = OBMF_LSTP_NOT_SUPP;
	else if (len != OBMF_HDR_LEN + OBMF_SERIAL_HEAD)
		return -EINVAL;
	else if (p[0] & OBMF_SERIAL_NACK)
		st = OBMF_LSTP_NACK;
	obmf_lstp_hdr(buf, buf[0], OBMF_HDR_RQRESP | st, 0);
	return OBMF_HDR_LEN;
}

/*
 * The device's ACK or NACK of a host write carries no data; OBMF-ICP
 * Table 23 wants {NACK, accepted}, all or nothing
 */
static ssize_t obmf_lstp_uart_decode(struct obmf_channel *ch, const u8 *in,
				     size_t len, u8 *out, size_t cap)
{
	struct obmf_lstp_ch *lc = ch->dialect_data;
	u8 st = in[1] & OBMF_HDR_STATUS;
	u8 *p = out + OBMF_HDR_LEN;
	size_t plen = 0;

	if (len != OBMF_HDR_LEN)
		return -EPROTO;
	if (OBMF_HDR_LEN + OBMF_SERIAL_HEAD > cap)
		return -EPROTO;
	if (st == OBMF_LSTP_SUCCESS) {
		p[0] = 0;
		put_unaligned_le16(lc->tx_count, p + 1);
		plen = OBMF_SERIAL_HEAD;
	} else if (st == OBMF_LSTP_NACK) {
		p[0] = OBMF_SERIAL_NACK;
		put_unaligned_le16(0, p + 1);
		plen = OBMF_SERIAL_HEAD;
		st = OBMF_STATUS_SUCCESS;
	}
	obmf_lstp_hdr(out, in[0], OBMF_HDR_RQRESP | obmf_lstp_status(ch, st),
		      plen);
	return OBMF_HDR_LEN + plen;
}

/* A device's UART WRITE becomes OBMF-ICP {events, count, data} */
static ssize_t obmf_lstp_uart_dreq(const u8 *in, size_t len, u8 *out,
				   size_t cap)
{
	size_t dlen = len - OBMF_HDR_LEN;
	u8 cmd = in[1] & OBMF_HDR_STATUS;
	u8 *p = out + OBMF_HDR_LEN;

	if (FIELD_GET(OBMF_LSTP_UART_CMD, cmd) != OBMF_LSTP_UART_WRITE)
		return -EOPNOTSUPP;
	if (OBMF_HDR_LEN + OBMF_SERIAL_HEAD + dlen > cap)
		return -EMSGSIZE;
	p[0] = FIELD_GET(OBMF_LSTP_UART_FLAGS, cmd);
	put_unaligned_le16(dlen, p + 1);
	memcpy(p + OBMF_SERIAL_HEAD, in + OBMF_HDR_LEN, dlen);
	obmf_lstp_hdr(out, in[0], 0, OBMF_SERIAL_HEAD + dlen);
	return OBMF_HDR_LEN + OBMF_SERIAL_HEAD + dlen;
}

/*
 * Set Interrupt Config {3, entry} becomes SET_IRQ_CONFIG {offset, type},
 * which sets one line (LSTP 1.0, 4.3.2): a request for more does not fit,
 * and a trigger Table 18 reserves is refused. In place, same length.
 */
static ssize_t obmf_lstp_gpio_irq_encode(u8 *buf, size_t n)
{
	u8 *p = buf + OBMF_HDR_LEN;
	u16 e = get_unaligned_le16(p + 1);
	u8 type = FIELD_GET(OBMF_GPIO_DATA, e);

	if (n > 1)
		return -EMSGSIZE;
	if (type >= ARRAY_SIZE(obmf_lstp_irq_types))
		return -EINVAL;
	put_unaligned_le16(FIELD_GET(OBMF_GPIO_INDEX, e), p);
	p[2] = obmf_lstp_irq_types[type];
	obmf_lstp_hdr(buf, buf[0], OBMF_LSTP_GPIO_SET_IRQ_CONFIG, 3);
	return OBMF_HDR_LEN + 3;
}

/*
 * OBMF-ICP Get Values {0, entries} becomes GET_VALUE {offsets}, each
 * entry's index the offset (LSTP 1.0, 4.3.2); the indices are kept, as
 * the response carries the states only. Set Values {1, entries} becomes
 * SET_VALUE {offset, state} for each line. Data bits Table 18 reserves
 * refuse the request. Get Interrupt Config and the commands a device
 * sends are not translated.
 */
static ssize_t obmf_lstp_gpio_encode(struct obmf_channel *ch, u8 *buf,
				     size_t len, size_t cap)
{
	struct obmf_lstp_ch *lc = ch->dialect_data;
	size_t plen = len - OBMF_HDR_LEN;
	u8 *p = buf + OBMF_HDR_LEN;
	size_t n, k;
	u16 e;

	if (!plen || p[0] > OBMF_GPIO_INTERRUPT_NOTIFY_DATAGRAM)
		return -EINVAL;
	if (p[0] > OBMF_GPIO_SET_VALUES &&
	    p[0] != OBMF_GPIO_SET_INTERRUPT_CONFIG)
		return -EOPNOTSUPP;
	n = (plen - 1) / 2;
	if (!n || plen != 1 + 2 * n)
		return -EINVAL;
	if (p[0] == OBMF_GPIO_SET_INTERRUPT_CONFIG)
		return obmf_lstp_gpio_irq_encode(buf, n);
	if (p[0] == OBMF_GPIO_GET_VALUES) {
		if (n > ARRAY_SIZE(lc->gpio_index))
			return -EMSGSIZE;
		for (k = 0; k < n; k++) {
			e = get_unaligned_le16(p + 1 + 2 * k);
			if (FIELD_GET(OBMF_GPIO_DATA, e))
				return -EINVAL;
			lc->gpio_index[k] = e;
		}
		lc->gpio_count = n;
		memmove(p, p + 1, 2 * n);
		obmf_lstp_hdr(buf, buf[0], OBMF_LSTP_GPIO_GET_VALUE, 2 * n);
		return OBMF_HDR_LEN + 2 * n;
	}

	if (OBMF_HDR_LEN + 3 * n > cap)
		return -EMSGSIZE;
	for (k = 0; k < n; k++)
		if (FIELD_GET(OBMF_GPIO_DATA,
			      get_unaligned_le16(p + 1 + 2 * k)) > 1)
			return -EINVAL;
	/* from the last line, so no entry is overwritten before it is read */
	for (k = n; k--;) {
		e = get_unaligned_le16(p + 1 + 2 * k);
		put_unaligned_le16(FIELD_GET(OBMF_GPIO_INDEX, e), p + 3 * k);
		p[3 * k + 2] = FIELD_GET(OBMF_GPIO_DATA, e);
	}
	obmf_lstp_hdr(buf, buf[0], OBMF_LSTP_GPIO_SET_VALUE, 3 * n);
	return OBMF_HDR_LEN + 3 * n;
}

/*
 * GET_VALUE answers one state byte for each line, in request order;
 * OBMF-ICP Table 19 wants {echo, entries}, each the line's index and its
 * value. SET_VALUE and SET_IRQ_CONFIG answer with no data, Table 19 with
 * the echo alone. A count other than the request's or a state above 1 is
 * malformed. An error status gives the echo alone.
 */
static ssize_t obmf_lstp_gpio_decode(struct obmf_channel *ch, const u8 *in,
				     size_t len, u8 *out, size_t cap,
				     u8 pending_cmd)
{
	struct obmf_lstp_ch *lc = ch->dialect_data;
	size_t dlen = len - OBMF_HDR_LEN, plen = 1, k;
	u8 st = in[1] & OBMF_HDR_STATUS;
	const u8 *d = in + OBMF_HDR_LEN;
	u8 *p = out + OBMF_HDR_LEN;

	if (pending_cmd > OBMF_GPIO_SET_VALUES &&
	    pending_cmd != OBMF_GPIO_SET_INTERRUPT_CONFIG)
		return -EPROTO;
	if (!st && pending_cmd == OBMF_GPIO_GET_VALUES) {
		if (dlen != lc->gpio_count)
			return -EPROTO;
		plen += 2 * dlen;
	} else if (!st && dlen) {
		return -EPROTO;
	}
	if (OBMF_HDR_LEN + plen > cap)
		return -EPROTO;
	p[0] = pending_cmd;
	for (k = 0; k < plen / 2; k++) {
		if (d[k] > 1)
			return -EPROTO;
		put_unaligned_le16(lc->gpio_index[k] |
					   FIELD_PREP(OBMF_GPIO_DATA, d[k]),
				   p + 1 + 2 * k);
	}
	obmf_lstp_hdr(out, in[0], OBMF_HDR_RQRESP | obmf_lstp_status(ch, st),
		      plen);
	return OBMF_HDR_LEN + plen;
}

/*
 * IRQ_EVENT {offset, state} for each line (LSTP 1.0, 4.3.2), which has no
 * response, becomes an Interrupt Notify Datagram {5, entries}, each the
 * line's index with data 0, as Table 18 reserves the data. A length that
 * is not whole entries, an offset past Table 18's index or a state above
 * 1 is malformed. The device sends no other request.
 */
static ssize_t obmf_lstp_gpio_dreq(const u8 *in, size_t len, u8 *out,
				   size_t cap)
{
	size_t dlen = len - OBMF_HDR_LEN, n = dlen / 3, k;
	const u8 *d = in + OBMF_HDR_LEN;
	u8 *p = out + OBMF_HDR_LEN;
	u16 off;

	if ((in[1] & OBMF_HDR_STATUS) != OBMF_LSTP_GPIO_IRQ_EVENT)
		return -EOPNOTSUPP;
	if (!n || dlen != 3 * n || OBMF_HDR_LEN + 1 + 2 * n > cap)
		return -EPROTO;
	p[0] = OBMF_GPIO_INTERRUPT_NOTIFY_DATAGRAM;
	for (k = 0; k < n; k++) {
		off = get_unaligned_le16(d + 3 * k);
		if (off > FIELD_MAX(OBMF_GPIO_INDEX) || d[3 * k + 2] > 1)
			return -EPROTO;
		put_unaligned_le16(off, p + 1 + 2 * k);
	}
	obmf_lstp_hdr(out, in[0], 0, 1 + 2 * n);
	return OBMF_HDR_LEN + 1 + 2 * n;
}

/*
 * Host requests on the I2C, GPIO, UART and SPI channels, and answers to
 * the UART device's writes; LSTP has no answer for the other device
 * requests
 */
static ssize_t obmf_lstp_encode(struct obmf_channel *ch, u8 *buf, size_t len,
				size_t cap)
{
	bool answer = buf[1] & OBMF_HDR_RQRESP;
	ssize_t n;

	/*
	 * The I2C, serial and SPI rewrites only shorten the message in place, so
	 * @cap is checked after them; the GPIO one checks @cap itself.
	 */
	switch (ch->info.type) {
	case OBMF_TYPE_I2C_CONTROLLER:
		if (answer)
			return 0;
		n = obmf_lstp_i2c_encode(buf, len);
		break;
	case OBMF_TYPE_SERIAL:
		if (answer)
			n = obmf_lstp_uart_answer(buf, len);
		else
			n = obmf_lstp_uart_encode(ch, buf, len);
		break;
	case OBMF_TYPE_GPIO:
		if (answer)
			return 0;
		n = obmf_lstp_gpio_encode(ch, buf, len, cap);
		break;
	case OBMF_TYPE_SPI:
		if (answer)
			return 0;
		n = obmf_lstp_spi_encode(buf, len);
		break;
	default:
		return answer ? 0 : -EOPNOTSUPP;
	}
	if (n > (ssize_t)cap)
		return -EMSGSIZE;
	return n;
}

static ssize_t obmf_lstp_decode(struct obmf_channel *ch, const u8 *in,
				size_t len, u8 *out, size_t cap, u8 pending_cmd,
				unsigned long *levels)
{
	bool resp = in[1] & OBMF_HDR_RQRESP;

	switch (ch->info.type) {
	case OBMF_TYPE_I2C_CONTROLLER:
		if (!resp)
			break;
		return obmf_lstp_i2c_decode(ch, in, len, out, cap, pending_cmd);
	case OBMF_TYPE_SPI:
		if (!resp)
			break;
		return obmf_lstp_spi_decode(ch, in, len, out, cap, pending_cmd);
	case OBMF_TYPE_GPIO:
		if (!resp)
			return obmf_lstp_gpio_dreq(in, len, out, cap);
		return obmf_lstp_gpio_decode(ch, in, len, out, cap,
					     pending_cmd);
	case OBMF_TYPE_SERIAL:
		if (resp)
			return obmf_lstp_uart_decode(ch, in, len, out, cap);
		return obmf_lstp_uart_dreq(in, len, out, cap);
	}
	return -EOPNOTSUPP;
}

/* LSTP 1.0, 3.1.1: the statuses with no OBMF-ICP code on the channel */
static int obmf_lstp_status_to_errno(const struct obmf_channel *ch, u8 raw)
{
	u8 type = ch->info.type;

	switch (raw) {
	case OBMF_LSTP_NACK:
		if (type == OBMF_TYPE_I2C_CONTROLLER ||
		    type == OBMF_TYPE_SERIAL)
			return 0;
		return -EIO;
	case OBMF_LSTP_ARB_LOST:
		return type == OBMF_TYPE_I2C_CONTROLLER ? 0 : -EIO;
	case OBMF_LSTP_NOT_SUPP:
		return -EOPNOTSUPP;
	}
	return raw > OBMF_LSTP_TOO_LARGE ? -EIO : 0;
}

/* LSTP 1.0, 4.4.2: BUS_RECOVERY, its result kept as Table 69's bits */
static int obmf_lstp_i2c_recover(struct obmf_channel *ch,
				 struct obmf_lstp_ch *lc)
{
	u8 reply[OBMF_HDR_LEN];
	u8 msg[OBMF_HDR_LEN];
	size_t len;
	int ret;

	obmf_lstp_hdr(msg, ch->info.number, OBMF_LSTP_I2C_BUS_RECOVERY, 0);
	ret = obmf_ch_xfer_wire(ch, msg, sizeof(msg), reply, sizeof(reply),
				&len, 0);
	if (ret)
		return ret;
	if (reply[1] & OBMF_HDR_STATUS)
		lc->recovery |= OBMF_I2C_RECOVERY_FAILED;
	else
		lc->recovery |= OBMF_I2C_RECOVERY_SUCCESS;
	return 0;
}

/*
 * LSTP has no channel header: the channels are always enabled and their
 * configuration valid, and only the I2C recovery bits do anything
 */
static int obmf_lstp_ch_reg(struct obmf_channel *ch, unsigned int reg, u8 mask,
			    u8 val, u8 *old)
{
	bool i2c = ch->info.type == OBMF_TYPE_I2C_CONTROLLER;
	struct obmf_lstp_ch *lc = ch->dialect_data;

	if (!lc)
		return -ENODEV;
	if (!mask) {
		switch (reg) {
		case OBMF_CH_REG_STATUS:
			*old = FIELD_PREP(OBMF_CH_STATUS_HEALTH,
					  OBMF_CH_HEALTH_OK) |
			       OBMF_CH_STATUS_CONFIG_VALID;
			return 0;
		case OBMF_CH_REG_CONTROL:
			*old = OBMF_CH_CONTROL_ENABLE;
			return 0;
		case OBMF_CH_REG_SPECIFIC_STATUS:
			*old = i2c ? lc->recovery : 0;
			return 0;
		case OBMF_CH_REG_SPECIFIC_CONTROL:
			*old = 0;
			return 0;
		}
		return -EINVAL;
	}

	switch (reg) {
	case OBMF_CH_REG_SPECIFIC_CONTROL:
		if (!i2c ||
		    mask & ~(OBMF_I2C_TRIGGER_RECOVERY | OBMF_I2C_CLEAR_STATUS))
			break;
		if (mask & val & OBMF_I2C_CLEAR_STATUS)
			lc->recovery = 0;
		if (mask & val & OBMF_I2C_TRIGGER_RECOVERY)
			return obmf_lstp_i2c_recover(ch, lc);
		return 0;
	}
	return -EOPNOTSUPP;
}

/*
 * LSTP 1.0, 4.4.1 to OBMF-ICP Table 79: the speed, and recovery offered.
 * Devices may send only the speed; the fields not sent read as 0.
 */
static int obmf_lstp_i2c_config(struct obmf_channel *ch, const u8 *raw,
				size_t len)
{
	struct obmf_lstp_i2c_cfg c;

	if (!len)
		return -EINVAL;
	memcpy_and_pad(&c, sizeof(c), raw, len, 0);
	ch->config = kmalloc(2, GFP_KERNEL);
	if (!ch->config)
		return -ENOMEM;
	ch->config[0] = c.speed;
	ch->config[1] = OBMF_I2C_OPT_BUS_RECOVERY;
	ch->info.config = ch->config;
	ch->info.config_len = 2;
	ch->info.quirks.i2c_dev_retries = c.arbitration_retries;
	return 0;
}

/*
 * LSTP 1.0, 4.2.1 to OBMF-ICP Table 84: no mode or word size can be set
 * (mode 0, 8 bits), one clock for both directions, and at most the two
 * chip selects a command can name. The device count and the speed are
 * passed on as they are otherwise; the SPI driver checks them.
 */
static int obmf_lstp_spi_config(struct obmf_channel *ch, const u8 *raw,
				size_t len)
{
	const struct obmf_lstp_spi_cfg *c = (const void *)raw;
	struct obmf_spi_cfg *cfg;

	if (len < sizeof(*c))
		return -EINVAL;
	cfg = kzalloc_obj(*cfg, GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;
	cfg->num_devices = min_t(u8, c->num_devices, OBMF_LSTP_SPI_CS_MAX);
	cfg->rd_max_speed_hz = c->speed;
	cfg->wr_max_speed_hz = c->speed;
	if (c->num_devices > OBMF_LSTP_SPI_CS_MAX)
		obmf_bind_warn(ch->core,
			       "channel %u: %u SPI devices, %u used\n",
			       ch->info.number, c->num_devices,
			       OBMF_LSTP_SPI_CS_MAX);
	ch->config = (u8 *)cfg;
	ch->info.config = ch->config;
	ch->info.config_len = sizeof(*cfg);
	return 0;
}

/*
 * LSTP 1.0, 4.5.1: its first bytes are OBMF-ICP Table 78, the only ones
 * used; the electrical fields after them may be left out
 */
static int obmf_lstp_uart_config(struct obmf_channel *ch, const u8 *raw,
				 size_t len)
{
	size_t n = offsetof(struct obmf_lstp_uart_cfg, drive_strength);

	if (len < n)
		return -EINVAL;
	ch->config = kmemdup(raw, n, GFP_KERNEL);
	if (!ch->config)
		return -ENOMEM;
	ch->info.config = ch->config;
	ch->info.config_len = n;
	return 0;
}

/* The GPIO lines a table translation left out, for one message each */
struct obmf_lstp_gpio_tally {
	unsigned int bad;
	unsigned int leds;
	unsigned int odd_leds;
	unsigned int above;
};

/*
 * One LSTP GPIO as an OBMF-ICP Table 77 entry; false for a line left out.
 * A value out of range refuses the line. Z (LSTP 1.0, 4.3.1.2), which
 * OBMF-ICP does not have, becomes the released level: high on open drain,
 * low on open source. A push-pull line with Z stays an output and the entry
 * reports it low; the LSTP configuration kept as read still says Z. An
 * output with LED support is an LED, not a GPIO line; the field on any
 * other line is ignored.
 */
static bool obmf_lstp_gpio_entry(const struct obmf_lstp_gpio_cfg *g, u16 pos,
				 struct obmf_gpio_cfg_entry *e,
				 struct obmf_lstp_gpio_tally *t)
{
	const struct obmf_gpio_line_cfg *l = &g->line;
	bool led = g->led_support == OBMF_LSTP_GPIO_LED_HOLD ||
		   g->led_support == OBMF_LSTP_GPIO_LED_INTERPOLATE;

	if (l->direction > OBMF_LSTP_GPIO_IN ||
	    l->default_output > OBMF_LSTP_GPIO_Z ||
	    l->drive > OBMF_LSTP_GPIO_OPEN_SOURCE ||
	    l->bias > OBMF_LSTP_GPIO_BIAS_MAX) {
		t->bad++;
		return false;
	}
	if (led && l->direction == OBMF_LSTP_GPIO_OUT) {
		t->leds++;
		return false;
	}
	if (g->led_support)
		t->odd_leds++;
	e->index = cpu_to_le16(pos);
	e->line = *l;
	if (l->default_output == OBMF_LSTP_GPIO_Z)
		e->line.default_output = l->drive == OBMF_LSTP_GPIO_OPEN_DRAIN ?
						 OBMF_LSTP_GPIO_HIGH :
						 OBMF_LSTP_GPIO_LOW;
	return true;
}

/*
 * LSTP 1.0, 4.3.1 to OBMF-ICP Tables 76 and 77: up to 255 blocks of a
 * count and that many GPIOs, a line's index its position across the
 * blocks (LSTP 1.0, 1.1.1); lines left out leave holes. A table cut
 * inside a GPIO keeps the whole ones; positions past the 12-bit OBMF-ICP
 * index (Table 18) are dropped.
 */
static int obmf_lstp_gpio_config(struct obmf_channel *ch, const u8 *raw,
				 size_t len)
{
	const size_t esz = sizeof(struct obmf_lstp_gpio_cfg);
	struct obmf_lstp_gpio_tally t = {};
	struct obmf_core *core = ch->core;
	struct device *dev = core->dev;
	unsigned int nr = ch->info.number;
	struct obmf_gpio_cfg_entry *e;
	unsigned int blocks = 0;
	size_t off = 0, k, n;
	bool cut = false;
	u16 count = 0;
	u32 pos = 0;

	BUILD_BUG_ON(sizeof(struct obmf_lstp_gpio_cfg) != 48);
	BUILD_BUG_ON(sizeof(struct obmf_gpio_cfg_entry) != 50);

	if (!len)
		return -EINVAL;
	n = min(len / esz, OBMF_GPIO_LINES);
	ch->config = kzalloc(OBMF_GPIO_COUNT_LEN + n * sizeof(*e), GFP_KERNEL);
	if (!ch->config)
		return -ENOMEM;
	e = (struct obmf_gpio_cfg_entry *)(ch->config + OBMF_GPIO_COUNT_LEN);
	while (!cut && off < len && blocks < OBMF_LSTP_GPIO_BLOCKS) {
		n = raw[off++];
		blocks++;
		for (k = 0; k < n; k++, pos++, off += esz) {
			if (len - off < esz) {
				cut = true;
				break;
			}
			if (pos >= OBMF_GPIO_LINES)
				t.above++;
			else if (obmf_lstp_gpio_entry((const void *)(raw + off),
						      pos, &e[count], &t))
				count++;
		}
	}
	if (cut)
		obmf_bind_warn(core, "channel %u: GPIO table cut in line %u\n",
			       nr, pos);
	else if (off < len)
		obmf_bind_warn(core,
			       "channel %u: %zu bytes after %u GPIO blocks ignored\n",
			       nr, len - off, blocks);
	if (t.bad)
		obmf_bind_warn(core,
			       "channel %u: %u GPIO lines out of range, left out\n",
			       nr, t.bad);
	if (t.above)
		obmf_bind_warn(core,
			       "channel %u: %u GPIO lines above %u, left out\n",
			       nr, t.above, OBMF_GPIO_LINES - 1);
	if (t.odd_leds)
		obmf_bind_warn(core,
			       "channel %u: LED support on %u GPIO lines ignored\n",
			       nr, t.odd_leds);
	if (t.leds)
		dev_dbg(dev, "channel %u: %u LED lines left out\n", nr, t.leds);

	put_unaligned_le16(count, ch->config);
	ch->info.config = ch->config;
	ch->info.config_len = OBMF_GPIO_COUNT_LEN + count * sizeof(*e);
	/*
	 * LSTP 1.0, 4.3.2: GET_VALUE sends 2 bytes a line and gets 1 back,
	 * rebuilt as OBMF-ICP's 1 + 2N (Table 19); SET_VALUE sends 3 a line.
	 */
	ch->info.quirks.gpio_per_get =
		(min(ch->info.max_req, ch->info.max_resp) - 1) / 2;
	ch->info.quirks.gpio_per_set = (ch->info.max_req - 1) / 3;
	return 0;
}

/*
 * The LSTP types with an OBMF-ICP type (LSTP 1.0, 3.2), and what each
 * adds to the wire payload in OBMF-ICP form. One limit covers every
 * request with data: an I2C write adds 3 but a write-read 1, so I2C
 * counts 1. @table: the configuration is a table of blocks that may span
 * slices (LSTP 1.0, 4.3.1.4)
 */
static const struct obmf_lstp_type {
	u8 lstp;
	u8 type;
	u8 role;
	u8 req_extra;
	u8 resp_extra;
	int (*config)(struct obmf_channel *ch, const u8 *raw, size_t len);
	bool table;
} obmf_lstp_types[] = {
	{ OBMF_LSTP_TYPE_SPI, OBMF_TYPE_SPI, OBMF_ROLE_PRODUCER, 5, 3,
	  obmf_lstp_spi_config },
	{ OBMF_LSTP_TYPE_GPIO, OBMF_TYPE_GPIO, OBMF_ROLE_PRODUCER, 1, 1,
	  obmf_lstp_gpio_config, true },
	{ OBMF_LSTP_TYPE_I2C, OBMF_TYPE_I2C_CONTROLLER, OBMF_ROLE_PRODUCER, 1,
	  4, obmf_lstp_i2c_config },
	{ OBMF_LSTP_TYPE_UART, OBMF_TYPE_SERIAL, OBMF_ROLE_PRODUCER, 3, 3,
	  obmf_lstp_uart_config },
	{ OBMF_LSTP_TYPE_IPMI, OBMF_TYPE_IPMI, OBMF_ROLE_CONSUMER, 1, 1 },
};

static const struct obmf_lstp_type *obmf_lstp_type(u8 lstp)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(obmf_lstp_types); i++)
		if (obmf_lstp_types[i].lstp == lstp)
			return &obmf_lstp_types[i];
	return NULL;
}

/*
 * One READ_CONFIG (LSTP 1.0, 4.1.2) of up to @want bytes at @off; the
 * reply in @reply, the configuration bytes it carries in @got. A status
 * other than success is -EREMOTEIO.
 */
static int obmf_lstp_read_config(struct obmf_core *core, u8 nr, u16 off,
				 u16 want, u8 *reply, size_t *got)
{
	struct {
		struct obmf_hdr hdr;
		struct obmf_lstp_read_config_req req;
	} __packed msg = {
		.hdr.status = OBMF_LSTP_READ_CONFIG,
		.hdr.size = cpu_to_le16(sizeof(msg.req)),
		.req.channel = nr,
		.req.offset = cpu_to_le16(off),
		.req.length = cpu_to_le16(want),
	};
	size_t base = OBMF_HDR_LEN + sizeof(struct obmf_lstp_cfg_base);
	size_t len;
	int ret;

	ret = obmf_ch_xfer_wire(core->ch0, &msg, sizeof(msg), reply,
				core->tp->max_rx_msg, &len, 0);
	if (ret)
		return ret;
	if (reply[1] & OBMF_HDR_STATUS)
		return -EREMOTEIO;
	if (len < base || len - base > want)
		return -EPROTO;
	*got = len - base;
	return 0;
}

static size_t obmf_lstp_window(struct obmf_core *core)
{
	return core->tp->max_rx_msg - OBMF_HDR_LEN -
	       sizeof(struct obmf_lstp_cfg_base);
}

/*
 * The device header; busy asks to retry later (LSTP 1.0, 3.1.1), which
 * the last pass cannot
 */
static int obmf_lstp_read_header(struct obmf_core *core, u8 *reply, size_t *got,
				 bool last)
{
	int ret = obmf_lstp_read_config(core, 0, 0, obmf_lstp_window(core),
					reply, got);

	if (ret == -EREMOTEIO &&
	    (reply[1] & OBMF_HDR_STATUS) == OBMF_LSTP_BUSY) {
		if (!last)
			return -EAGAIN;
		dev_err(core->dev, "device busy\n");
		return -EBUSY;
	}
	return ret;
}

static int obmf_lstp_add_channel(struct obmf_core *core,
				 struct obmf_channel **found, int nr,
				 const struct obmf_lstp_type *t,
				 const struct obmf_lstp_cfg_base *base,
				 const u8 *raw, size_t len)
{
	size_t max_req = core->tp->max_tx_msg - OBMF_HDR_LEN + t->req_extra;
	size_t max_resp = core->tp->max_rx_msg - OBMF_HDR_LEN + t->resp_extra;
	size_t rx_cap = max_resp + OBMF_HDR_LEN;
	struct obmf_lstp_ch *lc;
	int ret;

	struct obmf_channel *ch __free(obmf_ch) =
		obmf_ch_alloc(core, nr, max_req, max_resp, rx_cap);
	if (!ch)
		return -ENOMEM;
	lc = kzalloc_obj(*lc, GFP_KERNEL);
	if (!lc)
		return -ENOMEM;
	lc->cfg_len = len;
	lc->type = base->type;
	lc->flags = base->flags;
	ch->dialect_data = lc;

	ch->info.type = t->type;
	ch->info.role = t->role;
	obmf_copy_name(ch->info.name, base->name, sizeof(base->name));
	ch->status = FIELD_PREP(OBMF_CH_STATUS_HEALTH, OBMF_CH_HEALTH_OK) |
		     OBMF_CH_STATUS_CONFIG_VALID;
	ch->control = OBMF_CH_CONTROL_ENABLE;
	if (t->config) {
		ret = t->config(ch, raw, len);
		if (ret == -EINVAL) {
			obmf_bind_warn(core,
				       "channel %d: short config (%zu), ignored\n",
				       nr, len);
			return 0;
		}
		if (ret)
			return ret;
	}
	dev_dbg(core->dev,
		"channel %d: LSTP type %#x name '%s' limits %zu/%zu config %zu\n",
		nr, base->type, ch->info.name, max_req, max_resp, len);
	found[nr] = no_free_ptr(ch);
	return 0;
}

/*
 * A channel's READ_CONFIG slices, until a short one (an empty one for a
 * table, as a device may end a slice short on an entry), an error status
 * other than busy after the first slice, or the vendor's offsets (LSTP
 * 1.0, 4.1.2); every slice repeats the base fields. The first slice gives
 * the type, so a channel of a type not kept takes one read. Each slice
 * counts against the pass's budget in @total, and each short one against
 * OBMF_LSTP_CFG_SHORT_MAX and, in @pass_shorts, OBMF_LSTP_PASS_SHORT_MAX.
 */
static int obmf_lstp_discover_channel(struct obmf_core *core,
				      struct obmf_channel **found, int nr,
				      u8 *reply, u8 *raw, size_t *total,
				      int *pass_shorts)
{
	const struct obmf_lstp_cfg_base *b =
		(const void *)(reply + OBMF_HDR_LEN);
	size_t win = obmf_lstp_window(core);
	const struct obmf_lstp_type *t;
	struct obmf_lstp_cfg_base base;
	struct device *dev = core->dev;
	size_t len = 0, got, want;
	int ret, shorts = 0;

	do {
		want = min3(win, OBMF_LSTP_CFG_END - len,
			    OBMF_LSTP_CFG_TOTAL - *total);
		if (!want || shorts > OBMF_LSTP_CFG_SHORT_MAX ||
		    *pass_shorts > OBMF_LSTP_PASS_SHORT_MAX) {
			obmf_bind_warn(core,
				       "channel %d: over budget, ignored\n", nr);
			return 0;
		}
		ret = obmf_lstp_read_config(core, nr, len, want, reply, &got);
		if (ret == -EREMOTEIO && len &&
		    (reply[1] & OBMF_HDR_STATUS) != OBMF_LSTP_BUSY) {
			dev_dbg(dev,
				"channel %d: status %#lx ends the configuration\n",
				nr, reply[1] & OBMF_HDR_STATUS);
			break;
		}
		if (ret == -EREMOTEIO) {
			obmf_bind_warn(core,
				       "channel %d: status %#lx, ignored\n", nr,
				       reply[1] & OBMF_HDR_STATUS);
			return 0;
		}
		if (ret)
			return ret;
		if (len) {
			if (memcmp(&base, b, sizeof(base)))
				return -EPROTO;
		} else {
			base = *b;
			if (base.type == OBMF_LSTP_TYPE_MGMT)
				return -EPROTO;
			t = obmf_lstp_type(base.type);
			if (!t)
				break;
		}
		memcpy(raw + len, b + 1, got);
		len += got;
		*total += got;
		if (got && got < want) {
			shorts++;
			(*pass_shorts)++;
		}
	} while ((got == want || (t->table && got)) && len < OBMF_LSTP_CFG_END);
	if (!t) {
		obmf_bind_warn(core,
			       "channel %d: LSTP type %#x not supported, ignored\n",
			       nr, base.type);
		return 0;
	}
	if (len == OBMF_LSTP_CFG_END)
		obmf_bind_warn(core, "channel %d: configuration cut at %#x\n",
			       nr, OBMF_LSTP_CFG_END);

	return obmf_lstp_add_channel(core, found, nr, t, &base, raw, len);
}

/*
 * One pass: the device header, channels 1 to N, the header again
 * (LSTP 1.0, 4.1.1 and 4.1.2). Channel 0 of another type may be a
 * stale reply, so only the last pass takes it for the device's answer.
 */
static int obmf_lstp_discover(struct obmf_core *core,
			      struct obmf_channel **found, bool last)
{
	const struct obmf_lstp_cfg_base *b;
	const struct obmf_lstp_mgmt_cfg *m;
	size_t got, first, total = 0;
	int n, nr, ret, shorts = 0;

	u8 *head __free(kfree) = kmalloc(core->tp->max_rx_msg, GFP_KERNEL);
	u8 *reply __free(kfree) = kmalloc(core->tp->max_rx_msg, GFP_KERNEL);
	u8 *raw __free(kvfree) = kvmalloc(OBMF_LSTP_CFG_END, GFP_KERNEL);
	BUILD_BUG_ON(sizeof(struct obmf_lstp_read_config_req) != 5);
	BUILD_BUG_ON(sizeof(struct obmf_lstp_cfg_base) != 18);
	BUILD_BUG_ON(sizeof(struct obmf_lstp_mgmt_cfg) != 2);
	BUILD_BUG_ON(sizeof(struct obmf_lstp_i2c_cfg) != 11);
	BUILD_BUG_ON(sizeof(struct obmf_lstp_uart_cfg) != 15);
	BUILD_BUG_ON(offsetof(struct obmf_lstp_uart_cfg, drive_strength) != 7);

	if (!head || !reply || !raw)
		return -ENOMEM;

	ret = obmf_lstp_read_header(core, head, &first, last);
	if (ret)
		return ret;
	b = (const void *)(head + OBMF_HDR_LEN);
	m = (const void *)(b + 1);
	if (b->type != OBMF_LSTP_TYPE_MGMT) {
		if (!last)
			return -EPROTO;
		dev_err(core->dev, "channel 0 type %#x not supported\n",
			b->type);
		return -ENODEV;
	}
	if (first < sizeof(*m))
		return -EPROTO;
	if (m->version != OBMF_LSTP_VERSION) {
		dev_err(core->dev, "unsupported LSTP version %u\n", m->version);
		return -ENODEV;
	}

	n = m->num_channels;
	for (nr = 1; nr <= n; nr++) {
		if (total == OBMF_LSTP_CFG_TOTAL) {
			dev_warn(core->dev,
				 "channels %d to %d not read, over budget\n",
				 nr, n);
			break;
		}
		ret = obmf_lstp_discover_channel(core, found, nr, reply, raw,
						 &total, &shorts);
		if (ret)
			return ret;
	}

	ret = obmf_lstp_read_header(core, reply, &got, last);
	if (ret)
		return ret;
	if (got != first ||
	    memcmp(head, reply, OBMF_HDR_LEN + sizeof(*b) + first))
		return -EAGAIN;

	core->version_major = m->version;
	obmf_copy_name(core->device_name, b->name, sizeof(b->name));
	if (!core->device_name[0])
		dev_warn(core->dev, "device has no name\n");
	dev_dbg(core->dev, "LSTP %u device '%s', %d channels\n", m->version,
		core->device_name, n);
	return 0;
}

static const struct obmf_dialect obmf_lstp_dialect = {
	.name = "LSTP",
	.encode = obmf_lstp_encode,
	.decode = obmf_lstp_decode,
	.discover = obmf_lstp_discover,
	.status_to_errno = obmf_lstp_status_to_errno,
	.ch_reg = obmf_lstp_ch_reg,
	.ch0_whole_msgs = true,
	.keep_ch_limits = true,
};

/*
 * LSTP 1.0, 2: no functional descriptor and bulk endpoints of at least
 * 64 bytes. It lets a message span packets and a packet carry several;
 * the driver keeps to one message of at most 512 bytes per packet, the
 * form LSTP firmware handles. A response is waited for at least a
 * second, ten times the 100 ms LSTP 1.0, 1 asks of the device, so that
 * a slow device's answer is not taken for none.
 */
static const struct obmf_usb_profile obmf_lstp_usb_profile = {
	.max_packet = OBMF_LSTP_MAX_MSG,
	.resp_timeout_floor_ms = 1000,
};

const struct obmf_usb_proto obmf_lstp_usb_proto = {
	.profile = &obmf_lstp_usb_profile,
	.dialect = &obmf_lstp_dialect,
};

#if IS_ENABLED(CONFIG_OBMF_LSTP_KUNIT_TEST)
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/serial.h>
#include <linux/spi/spi.h>
#include <linux/tracepoint.h>
#include <linux/tty.h>
#include <linux/workqueue.h>
#include <trace/events/printk.h>

#define LT_MSG 600
#define LT_CH 2

static struct obmf_channel *lt_chan(struct kunit *test, u8 type)
{
	struct obmf_channel *ch = kunit_kzalloc(test, sizeof(*ch), GFP_KERNEL);
	static atomic_t ids = ATOMIC_INIT(0);
	struct device *dev;
	char name[32];

	KUNIT_ASSERT_NOT_NULL(test, ch);
	ch->core = kunit_kzalloc(test, sizeof(*ch->core), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ch->core);
	/* Names the debug lines of the code under test */
	snprintf(name, sizeof(name), "obmf-lstp-test.%d",
		 atomic_inc_return(&ids));
	dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	ch->core->dev = dev;
	ch->dialect_data =
		kunit_kzalloc(test, sizeof(struct obmf_lstp_ch), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ch->dialect_data);
	ch->info.type = type;
	ch->info.number = LT_CH;
	return ch;
}

struct lt_row {
	const char *what;
	u8 pending;
	u8 icp[16];
	u8 icp_len;
	u8 wire[16];
	u8 wire_len;
};

/* OBMF-ICP request and LSTP request; LSTP 1.0, 4.4.2 */
static const struct lt_row lt_i2c_requests[] = {
	{ "write-read",
	  0,
	  { 2, 0, 5, 0, 0x03, 0x50, 2, 0, 0x10 },
	  9,
	  { 2, 0x04, 4, 0, 0x50, 2, 0, 0x10 },
	  8 },
	{ "read",
	  0,
	  { 2, 0, 4, 0, 0x00, 0x50, 3, 0 },
	  8,
	  { 2, 0x01, 3, 0, 0x50, 3, 0 },
	  7 },
	{ "write",
	  0,
	  { 2, 0, 6, 0, 0x01, 0x50, 0, 0, 0xaa, 0xbb },
	  10,
	  { 2, 0x02, 3, 0, 0x50, 0xaa, 0xbb },
	  7 },
	{ "block read",
	  0,
	  { 2, 0, 5, 0, 0x02, 0x50, 1, 0, 0x0d },
	  9,
	  { 2, 0x05, 4, 0, 0x50, 1, 0, 0x0d },
	  8 },
	{ "read, no stop",
	  0,
	  { 2, 0, 4, 0, 0x80, 0x50, 1, 0 },
	  8,
	  { 2, 0x41, 3, 0, 0x50, 1, 0 },
	  7 },
	{ "write, no stop",
	  0,
	  { 2, 0, 5, 0, 0x81, 0x50, 0, 0, 0xaa },
	  9,
	  { 2, 0x42, 2, 0, 0x50, 0xaa },
	  6 },
	{ "block read, no stop",
	  0,
	  { 2, 0, 4, 0, 0x82, 0x50, 0, 0 },
	  8,
	  { 2, 0x45, 3, 0, 0x50, 0, 0 },
	  7 },
	{ "write-read, no stop",
	  0,
	  { 2, 0, 5, 0, 0x83, 0x50, 1, 0, 0x10 },
	  9,
	  { 2, 0x44, 4, 0, 0x50, 1, 0, 0x10 },
	  8 },
};

/* LSTP response and OBMF-ICP response (Table 28) for each command */
static const struct lt_row lt_i2c_responses[] = {
	{ "write-read",
	  0x03,
	  { 2, 0x80, 6, 0, 0x03, 2, 0, 0, 0x5a, 0xa5 },
	  10,
	  { 2, 0x80, 2, 0, 0x5a, 0xa5 },
	  6 },
	{ "read",
	  0x00,
	  { 2, 0x80, 7, 0, 0x00, 3, 0, 0, 1, 2, 3 },
	  11,
	  { 2, 0x80, 3, 0, 1, 2, 3 },
	  7 },
	{ "write",
	  0x01,
	  { 2, 0x80, 4, 0, 0x01, 0, 0, 0 },
	  8,
	  { 2, 0x80, 0, 0 },
	  4 },
	{ "block read",
	  0x02,
	  { 2, 0x80, 7, 0, 0x02, 3, 0, 0, 2, 0x11, 0x22 },
	  11,
	  { 2, 0x80, 3, 0, 2, 0x11, 0x22 },
	  7 },
	{ "write, no stop",
	  0x81,
	  { 2, 0x80, 4, 0, 0x01, 0, 0, 0 },
	  8,
	  { 2, 0x80, 0, 0 },
	  4 },
	{ "read, no stop",
	  0x80,
	  { 2, 0x80, 5, 0, 0x00, 1, 0, 0, 9 },
	  9,
	  { 2, 0x80, 1, 0, 9 },
	  5 },
};

static void lt_i2c_encode_table(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_I2C_CONTROLLER);
	const struct lt_row *r;
	u8 buf[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(lt_i2c_requests); i++) {
		r = &lt_i2c_requests[i];
		memcpy(buf, r->icp, r->icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, r->icp_len,
						     sizeof(buf)),
				    (ssize_t)r->wire_len, "%s", r->what);
		KUNIT_EXPECT_MEMEQ_MSG(test, buf, r->wire, r->wire_len, "%s",
				       r->what);
	}
}

static void lt_i2c_decode_table(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_I2C_CONTROLLER);
	const struct lt_row *r;
	u8 out[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(lt_i2c_responses); i++) {
		r = &lt_i2c_responses[i];
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_decode(ch, r->wire, r->wire_len,
						     out, sizeof(out),
						     r->pending, NULL),
				    (ssize_t)r->icp_len, "%s", r->what);
		KUNIT_EXPECT_MEMEQ_MSG(test, out, r->icp, r->icp_len, "%s",
				       r->what);
	}
}

/* Requests the I2C encoder refuses, and the cap */
static void lt_i2c_encode_bad(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_I2C_CONTROLLER);
	static const struct lt_row bad[] = {
		{ "short", 0, { 2, 0, 3, 0, 0x03, 0x50, 2 }, 7 },
		{ "unknown command", 0, { 2, 0, 4, 0, 0x04, 0x50, 0, 0 }, 8 },
		{ "last command", 0, { 2, 0, 4, 0, 0x7f, 0x50, 0, 0 }, 8 },
		{ "read with data", 0, { 2, 0, 5, 0, 0x00, 0x50, 1, 0, 9 }, 9 },
		{ "write with a count",
		  0,
		  { 2, 0, 5, 0, 0x01, 0x50, 1, 0, 9 },
		  9 },
		{ "block read count 2", 0, { 2, 0, 4, 0, 0x02, 0x50, 2, 0 }, 8 },
	};
	const struct lt_row *r = &lt_i2c_requests[0];
	u8 buf[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		memcpy(buf, bad[i].icp, bad[i].icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, bad[i].icp_len,
						     sizeof(buf)),
				    -EINVAL, "%s", bad[i].what);
	}
	memcpy(buf, r->icp, r->icp_len);
	KUNIT_EXPECT_EQ(test, obmf_lstp_encode(ch, buf, r->icp_len, 7),
			-EMSGSIZE);
	memcpy(buf, r->icp, r->icp_len);
	KUNIT_EXPECT_EQ(test, obmf_lstp_encode(ch, buf, r->icp_len, 8), 8);
}

/* Every LSTP status (3.1.1) and two beyond: the echo alone, mapped */
static void lt_i2c_decode_status(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_I2C_CONTROLLER);
	static const u8 map[][2] = {
		{ 0x01, 0x01 },
		{ 0x02, 0x02 },
		{ 0x03, 0x03 },
		{ 0x04, OBMF_STATUS_I2C_NACK },
		{ 0x05, OBMF_STATUS_I2C_ARB_LOST },
		{ 0x06, OBMF_STATUS_INVALID },
		{ 0x07, OBMF_STATUS_SIZE_UNSUPPORTED },
		{ 0x08, 0x08 },
		{ 0x7f, 0x7f },
	};
	u8 in[] = { 2, 0x80, 1, 0, 0xee };
	u8 out[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(map); i++) {
		u8 want[] = { 2, 0x80 | map[i][1], 1, 0, 0x03 };

		in[1] = 0x80 | map[i][0];
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_decode(ch, in, sizeof(in), out,
						     sizeof(out), 0x03, NULL),
				    (ssize_t)sizeof(want), "status %#x",
				    map[i][0]);
		KUNIT_EXPECT_MEMEQ(test, out, want, sizeof(want));
	}
}

/* A write reply with data, and replies longer than the buffer */
static void lt_i2c_decode_bad(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_I2C_CONTROLLER);
	static const u8 write_data[] = { 2, 0x80, 1, 0, 0x5a };
	const struct lt_row *r = &lt_i2c_responses[0];
	u8 out[32];

	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, write_data, sizeof(write_data),
					 out, sizeof(out), 0x01, NULL),
			-EPROTO);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, r->wire, r->wire_len, out,
					 r->icp_len - 1, r->pending, NULL),
			-EPROTO);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, r->wire, r->wire_len, out,
					 r->icp_len, r->pending, NULL),
			(ssize_t)r->icp_len);
	r = &lt_i2c_responses[2];
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, r->wire, r->wire_len, out, 4,
					 r->pending, NULL),
			-EPROTO);
}

/* OBMF-ICP request (Table 50) and LSTP request; LSTP 1.0, 4.2.2 */
static const struct lt_row lt_spi_requests[] = {
	{ "read, CS0, assert and de-assert",
	  0,
	  { 2, 0, 5, 0, 0x31, 0, 0, 0x02, 0x01 },
	  9,
	  { 2, 0x31, 4, 0, 0x02, 0x01, 0, 0 },
	  8 },
	{ "write, CS1, assert",
	  0,
	  { 2, 0, 7, 0, 0x62, 2, 0, 0, 0, 0xaa, 0xbb },
	  11,
	  { 2, 0x62, 2, 0, 0xaa, 0xbb },
	  6 },
	{ "write-read, CS0, keep",
	  0,
	  { 2, 0, 6, 0, 0x03, 1, 0, 1, 0, 0x9f },
	  10,
	  { 2, 0x03, 1, 0, 0x9f },
	  5 },
	{ "write-read, CS1, de-assert",
	  0,
	  { 2, 0, 7, 0, 0x53, 2, 0, 2, 0, 0x05, 0x00 },
	  11,
	  { 2, 0x53, 2, 0, 0x05, 0x00 },
	  6 },
	{ "write without data, CS1, de-assert",
	  0,
	  { 2, 0, 5, 0, 0x52, 0, 0, 0, 0 },
	  9,
	  { 2, 0x52, 0, 0 },
	  4 },
};

/* LSTP response and OBMF-ICP response (Table 51) */
static const struct lt_row lt_spi_responses[] = {
	{ "read",
	  0x31,
	  { 2, 0x80, 6, 0, 0x01, 3, 0, 1, 2, 3 },
	  10,
	  { 2, 0x80, 3, 0, 1, 2, 3 },
	  7 },
	{ "write", 0x62, { 2, 0x80, 3, 0, 0x02, 0, 0 }, 7, { 2, 0x80, 0, 0 }, 4 },
	{ "write-read",
	  0x73,
	  { 2, 0x80, 4, 0, 0x03, 1, 0, 0xef },
	  8,
	  { 2, 0x80, 1, 0, 0xef },
	  5 },
	{ "error with data: the echo only",
	  0x31,
	  { 2, 0x81, 1, 0, 0x01 },
	  5,
	  { 2, 0x81, 2, 0, 9, 9 },
	  6 },
	{ "NOT_SUPP", 0x22, { 2, 0x81, 1, 0, 0x02 }, 5, { 2, 0x86, 0, 0 }, 4 },
	{ "TOO_LARGE", 0x33, { 2, 0x86, 1, 0, 0x03 }, 5, { 2, 0x87, 0, 0 }, 4 },
	{ "ARB_LOST", 0x33, { 2, 0x84, 1, 0, 0x03 }, 5, { 2, 0x85, 0, 0 }, 4 },
};

/*
 * Every command with each chip select and CS flag pair, then the table:
 * the READ request byte for byte, its 4-byte length included
 */
static void lt_spi_encode_table(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SPI);
	const struct lt_row *r;
	u8 buf[32], cmd;
	int i;

	for (i = 0; i < 2 * 4 * 3; i++) {
		cmd = (i / 12) << 6 | (i / 3 % 4) << 4 | (i % 3 + 1);
		memset(buf, 0, sizeof(buf));
		buf[0] = LT_CH;
		buf[2] = 6;
		buf[4] = cmd;
		buf[5] = cmd & 2 ? 1 : 0;
		buf[7] = cmd & 1 ? 1 : 0;
		buf[9] = 0x5a;
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, 9 + !!(cmd & 2),
						     sizeof(buf)),
				    (ssize_t)(cmd & 2 ? 5 : 8), "cmd %#x", cmd);
		KUNIT_EXPECT_EQ_MSG(test, buf[1], cmd, "cmd %#x", cmd);
	}
	for (i = 0; i < ARRAY_SIZE(lt_spi_requests); i++) {
		r = &lt_spi_requests[i];
		memcpy(buf, r->icp, r->icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, r->icp_len,
						     sizeof(buf)),
				    (ssize_t)r->wire_len, "%s", r->what);
		KUNIT_EXPECT_MEMEQ_MSG(test, buf, r->wire, r->wire_len, "%s",
				       r->what);
	}
}

/* What LSTP cannot carry, and sizes that do not match the data */
static void lt_spi_encode_bad(struct kunit *test)
{
	static const struct lt_row bad[] = {
		{ "short", 0, { 2, 0, 4, 0, 0x02, 0, 0, 0 }, 8 },
		{ "CS2", 0, { 2, 0, 6, 0, 0x82, 1, 0, 0, 0, 7 }, 10 },
		{ "CS3", 0, { 2, 0, 6, 0, 0xc2, 1, 0, 0, 0, 7 }, 10 },
		{ "posted write", 0, { 2, 0, 6, 0, 0x04, 1, 0, 0, 0, 7 }, 10 },
		{ "command 0", 0, { 2, 0, 6, 0, 0x00, 1, 0, 0, 0, 7 }, 10 },
		{ "command 5", 0, { 2, 0, 6, 0, 0x05, 1, 0, 0, 0, 7 }, 10 },
		{ "command 15", 0, { 2, 0, 6, 0, 0x0f, 1, 0, 0, 0, 7 }, 10 },
		{ "write-read, sizes differ",
		  0,
		  { 2, 0, 6, 0, 0x03, 1, 0, 2, 0, 7 },
		  10 },
		{ "write with a read size",
		  0,
		  { 2, 0, 6, 0, 0x02, 1, 0, 1, 0, 7 },
		  10 },
		{ "read with data", 0, { 2, 0, 6, 0, 0x01, 1, 0, 1, 0, 7 }, 10 },
		{ "read of 0", 0, { 2, 0, 5, 0, 0x01, 0, 0, 0, 0 }, 9 },
		{ "write size above the data",
		  0,
		  { 2, 0, 6, 0, 0x02, 2, 0, 0, 0, 7 },
		  10 },
		{ "write size below the data",
		  0,
		  { 2, 0, 7, 0, 0x02, 1, 0, 0, 0, 7, 8 },
		  11 },
	};
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SPI);
	u8 buf[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		memcpy(buf, bad[i].icp, bad[i].icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, bad[i].icp_len,
						     sizeof(buf)),
				    -EINVAL, "%s", bad[i].what);
	}
	/* A device request on the channel has no LSTP form */
	memcpy(buf, lt_spi_responses[1].wire, 4);
	buf[1] = 0x02;
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, buf, 4, buf + 8, 16, 0, NULL),
			-EOPNOTSUPP);
}

static void lt_spi_decode_table(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SPI);
	const struct lt_row *r;
	u8 out[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(lt_spi_responses); i++) {
		r = &lt_spi_responses[i];
		memset(out, 0xee, sizeof(out));
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_decode(ch, r->wire, r->wire_len,
						     out, sizeof(out),
						     r->pending, NULL),
				    (ssize_t)r->icp_len, "%s", r->what);
		KUNIT_EXPECT_MEMEQ_MSG(test, out, r->icp, r->icp_len, "%s",
				       r->what);
	}
	/* a response past the cap */
	r = &lt_spi_responses[0];
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, r->wire, r->wire_len, out,
					 r->icp_len - 1, r->pending, NULL),
			-EPROTO);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, r->wire, r->wire_len, out,
					 r->icp_len, r->pending, NULL),
			(ssize_t)r->icp_len);
}

/*
 * A WRITE answered with data, as LSTP firmware has been seen to: success
 * with a read size of 0 and nothing copied. An error status still gives
 * the echo alone, and a response past the cap is still refused.
 */
static void lt_spi_decode_write_data(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SPI);
	static const u8 one[] = { 2, 0x80, 1, 0, 0x00 };
	static const u8 three[] = { 2, 0x80, 3, 0, 1, 2, 3 };
	static const u8 err[] = { 2, 0x81, 1, 0, 0x00 };
	static const u8 want[] = { 2, 0x80, 3, 0, 0x02, 0, 0 };
	static const u8 want_err[] = { 2, 0x81, 1, 0, 0x02 };
	u8 out[32];
	int i;

	memset(out, 0xee, sizeof(out));
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, one, sizeof(one), out, sizeof(out),
					 0x32, NULL),
			(ssize_t)sizeof(want));
	KUNIT_EXPECT_MEMEQ(test, out, want, sizeof(want));
	for (i = sizeof(want); i < sizeof(out); i++)
		KUNIT_EXPECT_EQ_MSG(test, out[i], 0xee, "byte %d", i);
	memset(out, 0xee, sizeof(out));
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, three, sizeof(three), out,
					 sizeof(out), 0x02, NULL),
			(ssize_t)sizeof(want));
	KUNIT_EXPECT_MEMEQ(test, out, want, sizeof(want));
	KUNIT_EXPECT_EQ(test, out[sizeof(want)], 0xee);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, err, sizeof(err), out, sizeof(out),
					 0x12, NULL),
			(ssize_t)sizeof(want_err));
	KUNIT_EXPECT_MEMEQ(test, out, want_err, sizeof(want_err));
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, one, sizeof(one), out,
					 sizeof(want) - 1, 0x32, NULL),
			-EPROTO);
}

/* Host writes (Table 22 to LSTP 1.0, 4.5.2) and the device's writes */
static const struct lt_row lt_uart_writes[] = {
	{ "data",
	  0,
	  { 3, 0, 5, 0, 0x00, 2, 0, 'h', 'i' },
	  9,
	  { 3, 0x00, 2, 0, 'h', 'i' },
	  6 },
	{ "generate break",
	  0,
	  { 3, 0, 3, 0, 0x01, 0, 0 },
	  7,
	  { 3, 0x08, 0, 0 },
	  4 },
	{ "break detected",
	  0,
	  { 3, 0, 3, 0, 0x02, 0, 0 },
	  7,
	  { 3, 0x10, 0, 0 },
	  4 },
	{ "overrun", 0, { 3, 0, 3, 0, 0x04, 0, 0 }, 7, { 3, 0x20, 0, 0 }, 4 },
	{ "carrier down",
	  0,
	  { 3, 0, 3, 0, 0x08, 0, 0 },
	  7,
	  { 3, 0x40, 0, 0 },
	  4 },
	{ "all events",
	  0,
	  { 3, 0, 4, 0, 0x0f, 1, 0, 'A' },
	  8,
	  { 3, 0x78, 1, 0, 'A' },
	  5 },
};

static void lt_uart_encode_table(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SERIAL);
	struct obmf_lstp_ch *lc = ch->dialect_data;
	const struct lt_row *r;
	u8 buf[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(lt_uart_writes); i++) {
		r = &lt_uart_writes[i];
		memcpy(buf, r->icp, r->icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, r->icp_len,
						     sizeof(buf)),
				    (ssize_t)r->wire_len, "%s", r->what);
		KUNIT_EXPECT_MEMEQ_MSG(test, buf, r->wire, r->wire_len, "%s",
				       r->what);
		KUNIT_EXPECT_EQ(test, lc->tx_count, r->wire_len - OBMF_HDR_LEN);
	}
}

static void lt_uart_dreq_table(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SERIAL);
	static const u8 reserved[] = { 3, 0x04, 1, 0, 'A' };
	static const u8 reserved_icp[] = { 3, 0, 4, 0, 0, 1, 0, 'A' };
	const struct lt_row *r;
	u8 out[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(lt_uart_writes); i++) {
		r = &lt_uart_writes[i];
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_decode(ch, r->wire, r->wire_len,
						     out, sizeof(out), 0, NULL),
				    (ssize_t)r->icp_len, "%s", r->what);
		KUNIT_EXPECT_MEMEQ_MSG(test, out, r->icp, r->icp_len, "%s",
				       r->what);
	}
	/* Bit 2 is reserved: ignored */
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, reserved, sizeof(reserved), out,
					 sizeof(out), 0, NULL),
			(ssize_t)sizeof(reserved_icp));
	KUNIT_EXPECT_MEMEQ(test, out, reserved_icp, sizeof(reserved_icp));
}

/* Writes the UART encoder refuses, unknown device commands, the quirks */
static void lt_uart_bad(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SERIAL);
	static const struct lt_row bad[] = {
		{ "short", 0, { 3, 0, 2, 0, 0, 0 }, 6 },
		{ "count above data", 0, { 3, 0, 5, 0, 0, 3, 0, 'h', 'i' }, 9 },
		{ "count below data", 0, { 3, 0, 5, 0, 0, 1, 0, 'h', 'i' }, 9 },
		{ "event bit 4", 0, { 3, 0, 3, 0, 0x10, 0, 0 }, 7 },
		{ "event bit 7", 0, { 3, 0, 3, 0, 0x80, 0, 0 }, 7 },
	};
	const struct lt_row *r = &lt_uart_writes[0];
	u8 cmd1[] = { 3, 0x01, 0, 0 };
	u8 buf[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		memcpy(buf, bad[i].icp, bad[i].icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, bad[i].icp_len,
						     sizeof(buf)),
				    -EINVAL, "%s", bad[i].what);
	}
	memcpy(buf, r->icp, r->icp_len);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_encode(ch, buf, r->icp_len, r->wire_len - 1),
			-EMSGSIZE);

	for (i = 1; i < 4; i++) {
		cmd1[1] = i;
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_decode(ch, cmd1, sizeof(cmd1),
						     buf, sizeof(buf), 0, NULL),
				    -EOPNOTSUPP, "command %d", i);
		cmd1[1] = 0x78 | i;
		KUNIT_EXPECT_EQ(test,
				obmf_lstp_decode(ch, cmd1, sizeof(cmd1), buf,
						 sizeof(buf), 0, NULL),
				-EOPNOTSUPP);
	}
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, r->wire, r->wire_len, buf,
					 r->icp_len - 1, 0, NULL),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, r->wire, r->wire_len, buf,
					 r->icp_len, 0, NULL),
			(ssize_t)r->icp_len);
}

/* ACK and NACK of a host write (Table 23), every other status, a body */
static void lt_uart_decode_status(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SERIAL);
	struct obmf_lstp_ch *lc = ch->dialect_data;
	static const u8 ack_icp[] = { 3, 0x80, 3, 0, 0, 5, 0 };
	static const u8 nack_icp[] = { 3, 0x80, 3, 0, OBMF_SERIAL_NACK, 0, 0 };
	static const u8 map[][2] = {
		{ 0x01, 0x01 },
		{ 0x02, 0x02 },
		{ 0x03, 0x03 },
		{ 0x05, OBMF_STATUS_PERMANENT_ERROR },
		{ 0x06, OBMF_STATUS_INVALID },
		{ 0x07, OBMF_STATUS_SIZE_UNSUPPORTED },
		{ 0x08, 0x08 },
	};
	u8 in[] = { 3, 0x80, 0, 0, 0 };
	u8 out[32];
	int i;

	lc->tx_count = 5;
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, in, 4, out, sizeof(out), 0, NULL),
			(ssize_t)sizeof(ack_icp));
	KUNIT_EXPECT_MEMEQ(test, out, ack_icp, sizeof(ack_icp));
	in[1] = 0x80 | OBMF_LSTP_NACK;
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, in, 4, out, sizeof(out), 0, NULL),
			(ssize_t)sizeof(nack_icp));
	KUNIT_EXPECT_MEMEQ(test, out, nack_icp, sizeof(nack_icp));

	for (i = 0; i < ARRAY_SIZE(map); i++) {
		u8 want[] = { 3, 0x80 | map[i][1], 0, 0 };
		ssize_t n;

		in[1] = 0x80 | map[i][0];
		n = obmf_lstp_decode(ch, in, 4, out, sizeof(out), 0, NULL);
		KUNIT_EXPECT_EQ_MSG(test, n, (ssize_t)sizeof(want),
				    "status %#x", map[i][0]);
		KUNIT_EXPECT_MEMEQ(test, out, want, sizeof(want));
	}

	in[1] = 0x80;
	in[2] = 1;
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, in, sizeof(in), out, sizeof(out),
					 0, NULL),
			-EPROTO);
	KUNIT_EXPECT_EQ(test, obmf_lstp_decode(ch, in, 4, out, 6, 0, NULL),
			-EPROTO);
}

/*
 * The host's answers to a device write become ACK or NACK, and with an
 * error status, with or without a body, NOT_SUPP
 */
static void lt_uart_answer(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_SERIAL);
	static const struct lt_row rows[] = {
		{ "accept",
		  0,
		  { 3, 0x80, 3, 0, 0, 2, 0 },
		  7,
		  { 3, 0x80, 0, 0 },
		  4 },
		{ "refuse",
		  0,
		  { 3, 0x80, 3, 0, 1, 0, 0 },
		  7,
		  { 3, 0x84, 0, 0 },
		  4 },
		{ "partial",
		  0,
		  { 3, 0x80, 3, 0, 1, 1, 0 },
		  7,
		  { 3, 0x84, 0, 0 },
		  4 },
		{ "busy",
		  0,
		  { 3, 0x83, 3, 0, 0, 0, 0 },
		  7,
		  { 3, 0x86, 0, 0 },
		  4 },
		{ "invalid", 0, { 3, 0x81, 0, 0 }, 4, { 3, 0x86, 0, 0 }, 4 },
	};
	static const u8 short_answer[] = { 3, 0x80, 1, 0, 0 };
	u8 buf[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		memcpy(buf, rows[i].icp, rows[i].icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, rows[i].icp_len,
						     rows[i].icp_len),
				    (ssize_t)rows[i].wire_len, "%s",
				    rows[i].what);
		KUNIT_EXPECT_MEMEQ_MSG(test, buf, rows[i].wire,
				       rows[i].wire_len, "%s", rows[i].what);
	}
	memcpy(buf, short_answer, sizeof(short_answer));
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_encode(ch, buf, sizeof(short_answer),
					 sizeof(short_answer)),
			-EINVAL);
}

/* Channels with no translation yet, and device requests LSTP lacks */
static void lt_other_types(struct kunit *test)
{
	static const u8 types[] = { OBMF_TYPE_CONFIG, OBMF_TYPE_MMIO,
				    OBMF_TYPE_IPMI, 0x20 };
	u8 req[] = { 5, 0x00, 1, 0, 0 };
	u8 resp[] = { 5, 0x80, 1, 0, 0 };
	struct obmf_channel *ch;
	u8 out[32];
	ssize_t n;
	int i;

	for (i = 0; i < ARRAY_SIZE(types); i++) {
		ch = lt_chan(test, types[i]);
		memcpy(out, req, sizeof(req));
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, out, sizeof(req),
						     sizeof(out)),
				    -EOPNOTSUPP, "type %#x", types[i]);
		memcpy(out, resp, sizeof(resp));
		KUNIT_EXPECT_EQ(test,
				obmf_lstp_encode(ch, out, sizeof(resp),
						 sizeof(out)),
				0);
		KUNIT_EXPECT_EQ(test,
				obmf_lstp_decode(ch, resp, sizeof(resp), out,
						 sizeof(out), 0, NULL),
				-EOPNOTSUPP);
		KUNIT_EXPECT_EQ(test,
				obmf_lstp_decode(ch, req, sizeof(req), out,
						 sizeof(out), 0, NULL),
				-EOPNOTSUPP);
	}
	ch = lt_chan(test, OBMF_TYPE_I2C_CONTROLLER);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, req, sizeof(req), out, sizeof(out),
					 0, NULL),
			-EOPNOTSUPP);
	memcpy(out, resp, sizeof(resp));
	n = obmf_lstp_encode(ch, out, sizeof(resp), sizeof(out));
	KUNIT_EXPECT_EQ(test, n, 0);
}

/* OBMF-ICP Get and Set Values (Table 18) and LSTP 1.0, 4.3.2 */
static const struct lt_row lt_gpio_requests[] = {
	{ "get",
	  0,
	  { 2, 0, 5, 0, 0x00, 0x01, 0x00, 0xff, 0x0f },
	  9,
	  { 2, 0x00, 4, 0, 0x01, 0x00, 0xff, 0x0f },
	  8 },
	{ "set",
	  0,
	  { 2, 0, 5, 0, 0x01, 0x03, 0x10, 0x05, 0x00 },
	  9,
	  { 2, 0x01, 6, 0, 0x03, 0x00, 0x01, 0x05, 0x00, 0x00 },
	  10 },
	{ "set, one line",
	  0,
	  { 2, 0, 3, 0, 0x01, 0xff, 0x1f },
	  7,
	  { 2, 0x01, 3, 0, 0xff, 0x0f, 0x01 },
	  7 },
	{ "set, three lines",
	  0,
	  { 2, 0, 7, 0, 0x01, 0x05, 0x10, 0x23, 0x01, 0xab, 0x10 },
	  11,
	  { 2, 0x01, 9, 0, 0x05, 0x00, 0x01, 0x23, 0x01, 0x00, 0xab, 0x00,
	    0x01 },
	  13 },
};

/* LSTP responses to the first two requests, and errors on a Get */
static const struct lt_row lt_gpio_responses[] = {
	{ "get",
	  0x00,
	  { 2, 0x80, 5, 0, 0x00, 0x01, 0x10, 0xff, 0x0f },
	  9,
	  { 2, 0x80, 2, 0, 1, 0 },
	  6 },
	{ "set", 0x01, { 2, 0x80, 1, 0, 0x01 }, 5, { 2, 0x80, 0, 0 }, 4 },
	{ "not supported",
	  0x00,
	  { 2, 0x80 | OBMF_STATUS_INVALID, 1, 0, 0x00 },
	  5,
	  { 2, 0x86, 0, 0 },
	  4 },
	{ "too large",
	  0x00,
	  { 2, 0x80 | OBMF_STATUS_SIZE_UNSUPPORTED, 1, 0, 0x00 },
	  5,
	  { 2, 0x87, 0, 0 },
	  4 },
};

/* The Get records its indices for the response */
static void lt_gpio_encode_table(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_GPIO);
	struct obmf_lstp_ch *lc = ch->dialect_data;
	const struct lt_row *r;
	u8 buf[32];
	int i;

	for (i = 0; i < ARRAY_SIZE(lt_gpio_requests); i++) {
		r = &lt_gpio_requests[i];
		memcpy(buf, r->icp, r->icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, r->icp_len,
						     sizeof(buf)),
				    (ssize_t)r->wire_len, "%s", r->what);
		KUNIT_EXPECT_MEMEQ_MSG(test, buf, r->wire, r->wire_len, "%s",
				       r->what);
	}
	KUNIT_EXPECT_EQ(test, lc->gpio_count, 2);
	KUNIT_EXPECT_EQ(test, lc->gpio_index[0], 0x001);
	KUNIT_EXPECT_EQ(test, lc->gpio_index[1], 0xfff);
}

/*
 * Set Interrupt Config of line 0xfff with each Table 18 trigger becomes
 * SET_IRQ_CONFIG with the LSTP type; its answer, the header alone, the
 * echo
 */
static void lt_gpio_irq_table(struct kunit *test)
{
	static const u8 types[] = { 0x00, 0x05, 0x04, 0x01, 0x02, 0x03 };
	static const u8 resp[] = { 2, 0x80, 0, 0 };
	static const u8 echo[] = { 2, 0x80, 1, 0, 0x03 };
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_GPIO);
	u8 out[16];
	int i;

	for (i = 0; i < ARRAY_SIZE(types); i++) {
		u8 buf[] = { 2, 0, 3, 0, 0x03, 0xff, 0x0f | i << 4 };
		const u8 want[] = { 2, 0x03, 3, 0, 0xff, 0x0f, types[i] };

		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, sizeof(buf),
						     sizeof(buf)),
				    (ssize_t)sizeof(want), "trigger %d", i);
		KUNIT_EXPECT_MEMEQ_MSG(test, buf, want, sizeof(want),
				       "trigger %d", i);
	}
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, resp, sizeof(resp), out,
					 sizeof(out), 0x03, NULL),
			(ssize_t)sizeof(echo));
	KUNIT_EXPECT_MEMEQ(test, out, echo, sizeof(echo));
}

/*
 * A device's IRQ_EVENT becomes an Interrupt Notify Datagram of the lines'
 * indices, the states left out; one that is empty, not whole entries,
 * names an offset past 4095 or a state above 1, or does not fit is
 * malformed. Another device request is not translated.
 */
static void lt_gpio_irq_event(struct kunit *test)
{
	static const u8 ev[] = { 2,    0x04, 6,	   0,	 0x01,
				 0x00, 0x01, 0xff, 0x0f, 0x00 };
	static const u8 want[] = { 2, 0x00, 5, 0, 0x05, 0x01, 0x00, 0xff, 0x0f };
	static const struct {
		const char *what;
		u8 m[8];
		size_t len;
	} bad[] = {
		{ "empty", { 2, 0x04, 0, 0 }, 4 },
		{ "two bytes", { 2, 0x04, 2, 0, 0x01, 0x00 }, 6 },
		{ "four bytes", { 2, 0x04, 4, 0, 0x01, 0x00, 0x01, 0x00 }, 8 },
		{ "state 2", { 2, 0x04, 3, 0, 0x01, 0x00, 0x02 }, 7 },
		{ "state 0xff", { 2, 0x04, 3, 0, 0x01, 0x00, 0xff }, 7 },
		{ "offset 4096", { 2, 0x04, 3, 0, 0x00, 0x10, 0x00 }, 7 },
		{ "offset 0xffff", { 2, 0x04, 3, 0, 0xff, 0xff, 0x01 }, 7 },
	};
	static const u8 other[] = { 2, 0x02, 2, 0, 0x01, 0x00 };
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_GPIO);
	u8 out[16];
	int i;

	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, ev, sizeof(ev), out, sizeof(want),
					 0, NULL),
			(ssize_t)sizeof(want));
	KUNIT_EXPECT_MEMEQ(test, out, want, sizeof(want));
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, ev, sizeof(ev), out,
					 sizeof(want) - 1, 0, NULL),
			-EPROTO);
	for (i = 0; i < ARRAY_SIZE(bad); i++)
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_decode(ch, bad[i].m, bad[i].len,
						     out, sizeof(out), 0, NULL),
				    -EPROTO, "%s", bad[i].what);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, other, sizeof(other), out,
					 sizeof(out), 0, NULL),
			-EOPNOTSUPP);
}

/*
 * Requests the GPIO encoder refuses: malformed or reserved ones, the
 * interrupt commands it does not translate, a Set Interrupt Config of
 * more than one line, and those past the cap or the Get record. An
 * answer has no LSTP form.
 */
static void lt_gpio_encode_bad(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_GPIO);
	static const struct lt_row bad[] = {
		{ "no command", 0, { 2, 0, 0, 0 }, 4 },
		{ "get, no line", 0, { 2, 0, 1, 0, 0x00 }, 5 },
		{ "get, odd length", 0, { 2, 0, 4, 0, 0x00, 1, 0, 2 }, 8 },
		{ "get, data", 0, { 2, 0, 3, 0, 0x00, 0x01, 0x10 }, 7 },
		{ "set, data 2", 0, { 2, 0, 3, 0, 0x01, 0x01, 0x20 }, 7 },
		{ "set, data 15", 0, { 2, 0, 3, 0, 0x01, 0x01, 0xf0 }, 7 },
		{ "command 6", 0, { 2, 0, 3, 0, 0x06, 0x01, 0x00 }, 7 },
		{ "command 0xff", 0, { 2, 0, 3, 0, 0xff, 0x01, 0x00 }, 7 },
		{ "irq, no line", 0, { 2, 0, 1, 0, 0x03 }, 5 },
		{ "irq, odd length", 0, { 2, 0, 4, 0, 0x03, 1, 0x30, 2 }, 8 },
		{ "irq, trigger 6", 0, { 2, 0, 3, 0, 0x03, 0x01, 0x60 }, 7 },
		{ "irq, trigger 15", 0, { 2, 0, 3, 0, 0x03, 0x01, 0xf0 }, 7 },
	};
	static const u8 two[] = { 2, 0, 5, 0, 0x03, 0x01, 0x30, 0x02, 0x30 };
	static const u8 answer[] = { 2, 0x80, 1, 0, 0x04 };
	const struct lt_row *r;
	u8 buf[OBMF_HDR_LEN + 1 + 2 * (OBMF_LSTP_GPIO_GET_MAX + 1)];
	int i;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		memcpy(buf, bad[i].icp, bad[i].icp_len);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, bad[i].icp_len,
						     sizeof(buf)),
				    -EINVAL, "%s", bad[i].what);
	}
	for (i = 2; i <= 5; i++) {
		u8 irq[] = { 2, 0, 3, 0, i, 0x01, 0x00 };

		if (i == 3)
			continue;
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, irq, sizeof(irq),
						     sizeof(irq)),
				    -EOPNOTSUPP, "command %d", i);
	}
	memcpy(buf, two, sizeof(two));
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_encode(ch, buf, sizeof(two), sizeof(buf)),
			-EMSGSIZE);
	r = &lt_gpio_requests[1];
	memcpy(buf, r->icp, r->icp_len);
	KUNIT_EXPECT_EQ(test, obmf_lstp_encode(ch, buf, r->icp_len, 9),
			-EMSGSIZE);
	KUNIT_EXPECT_MEMEQ(test, buf, r->icp, r->icp_len);
	KUNIT_EXPECT_EQ(test, obmf_lstp_encode(ch, buf, r->icp_len, 10), 10);
	r = &lt_gpio_requests[0];
	memcpy(buf, r->icp, r->icp_len);
	KUNIT_EXPECT_EQ(test, obmf_lstp_encode(ch, buf, r->icp_len, 7),
			-EMSGSIZE);

	memset(buf, 0, sizeof(buf));
	obmf_lstp_hdr(buf, 2, 0, sizeof(buf) - OBMF_HDR_LEN);
	KUNIT_EXPECT_EQ(test, obmf_lstp_encode(ch, buf, sizeof(buf), SZ_1K),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, obmf_lstp_encode(ch, buf, sizeof(buf) - 2, SZ_1K),
			(ssize_t)(sizeof(buf) - 3));

	memcpy(buf, answer, sizeof(answer));
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_encode(ch, buf, sizeof(answer), sizeof(buf)),
			0);
}

static void lt_gpio_decode_table(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_GPIO);
	const struct lt_row *r = &lt_gpio_requests[0];
	u8 out[32];
	int i;

	memcpy(out, r->icp, r->icp_len);
	KUNIT_ASSERT_EQ(test, obmf_lstp_encode(ch, out, r->icp_len, 32),
			(ssize_t)r->wire_len);
	for (i = 0; i < ARRAY_SIZE(lt_gpio_responses); i++) {
		r = &lt_gpio_responses[i];
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_decode(ch, r->wire, r->wire_len,
						     out, sizeof(out),
						     r->pending, NULL),
				    (ssize_t)r->icp_len, "%s", r->what);
		KUNIT_EXPECT_MEMEQ_MSG(test, out, r->icp, r->icp_len, "%s",
				       r->what);
	}
}

/*
 * Malformed after a Get of two lines: another count, a state above 1, a
 * Set or Set Interrupt Config reply with data, a command that has no
 * response, a buffer too short
 */
static void lt_gpio_decode_bad(struct kunit *test)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_GPIO);
	static const struct lt_row bad[] = {
		{ "one state", 0x00, {}, 0, { 2, 0x80, 1, 0, 1 }, 5 },
		{ "three states", 0x00, {}, 0, { 2, 0x80, 3, 0, 1, 0, 1 }, 7 },
		{ "state 2", 0x00, {}, 0, { 2, 0x80, 2, 0, 1, 2 }, 6 },
		{ "state 0xff", 0x00, {}, 0, { 2, 0x80, 2, 0, 0xff, 0 }, 6 },
		{ "set with data", 0x01, {}, 0, { 2, 0x80, 1, 0, 0 }, 5 },
		{ "irq with data", 0x03, {}, 0, { 2, 0x80, 1, 0, 1 }, 5 },
		{ "command 2", 0x02, {}, 0, { 2, 0x80, 0, 0 }, 4 },
		{ "command 4", 0x04, {}, 0, { 2, 0x80, 0, 0 }, 4 },
		{ "command 5", 0x05, {}, 0, { 2, 0x80, 0, 0 }, 4 },
	};
	const struct lt_row *r = &lt_gpio_requests[0];
	u8 out[32];
	ssize_t n;
	int i;

	memcpy(out, r->icp, r->icp_len);
	KUNIT_ASSERT_EQ(test, obmf_lstp_encode(ch, out, r->icp_len, 32),
			(ssize_t)r->wire_len);
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		n = obmf_lstp_decode(ch, bad[i].wire, bad[i].wire_len, out,
				     sizeof(out), bad[i].pending, NULL);
		KUNIT_EXPECT_EQ_MSG(test, n, -EPROTO, "%s", bad[i].what);
	}
	r = &lt_gpio_responses[0];
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, r->wire, r->wire_len, out,
					 r->icp_len - 1, r->pending, NULL),
			-EPROTO);
}

static void lt_status_errno(struct kunit *test)
{
	static const struct {
		u8 type;
		u8 raw;
		int err;
	} rows[] = {
		{ OBMF_TYPE_I2C_CONTROLLER, 0x01, 0 },
		{ OBMF_TYPE_I2C_CONTROLLER, 0x02, 0 },
		{ OBMF_TYPE_I2C_CONTROLLER, 0x03, 0 },
		{ OBMF_TYPE_I2C_CONTROLLER, 0x04, 0 },
		{ OBMF_TYPE_SERIAL, 0x04, 0 },
		{ OBMF_TYPE_GPIO, 0x04, -EIO },
		{ OBMF_TYPE_I2C_CONTROLLER, 0x05, 0 },
		{ OBMF_TYPE_SERIAL, 0x05, -EIO },
		{ OBMF_TYPE_GPIO, 0x05, -EIO },
		{ OBMF_TYPE_I2C_CONTROLLER, 0x06, -EOPNOTSUPP },
		{ OBMF_TYPE_SERIAL, 0x06, -EOPNOTSUPP },
		{ OBMF_TYPE_SERIAL, 0x07, 0 },
		{ OBMF_TYPE_I2C_CONTROLLER, 0x08, -EIO },
		{ OBMF_TYPE_SERIAL, 0x7f, -EIO },
	};
	struct obmf_channel *ch = lt_chan(test, 0);
	int i;

	for (i = 0; i < ARRAY_SIZE(rows); i++) {
		ch->info.type = rows[i].type;
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_status_to_errno(ch, rows[i].raw),
				    rows[i].err, "type %#x status %#x",
				    rows[i].type, rows[i].raw);
	}
}

#define LT_SPEC_STATUS OBMF_CH_REG_SPECIFIC_STATUS
#define LT_SPEC_CONTROL OBMF_CH_REG_SPECIFIC_CONTROL

static int lt_reg_read(struct obmf_channel *ch, unsigned int reg, u8 *v)
{
	return obmf_lstp_ch_reg(ch, reg, 0, 0, v);
}

static int lt_reg_write(struct obmf_channel *ch, unsigned int reg, u8 mask,
			u8 val)
{
	return obmf_lstp_ch_reg(ch, reg, mask, val, NULL);
}

/* The channel header bytes LSTP lacks (OBMF-ICP Tables 65 and 69) */
static void lt_ch_reg(struct kunit *test)
{
	struct obmf_channel *i2c = lt_chan(test, OBMF_TYPE_I2C_CONTROLLER);
	struct obmf_channel *uart = lt_chan(test, OBMF_TYPE_SERIAL);
	struct obmf_lstp_ch *lc = i2c->dialect_data;
	struct obmf_lstp_ch *uc = uart->dialect_data;
	const u8 clear = OBMF_I2C_CLEAR_STATUS;
	const u8 en = OBMF_CH_CONTROL_ENABLE;
	u8 v = 0xff;

	KUNIT_EXPECT_EQ(test, lt_reg_read(i2c, OBMF_CH_REG_STATUS, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 0x21);
	KUNIT_EXPECT_EQ(test, lt_reg_read(uart, OBMF_CH_REG_CONTROL, &v), 0);
	KUNIT_EXPECT_EQ(test, v, en);
	lc->recovery = OBMF_I2C_RECOVERY_SUCCESS;
	KUNIT_EXPECT_EQ(test, lt_reg_read(i2c, LT_SPEC_STATUS, &v), 0);
	KUNIT_EXPECT_EQ(test, v, OBMF_I2C_RECOVERY_SUCCESS);
	uc->recovery = 0xff;
	KUNIT_EXPECT_EQ(test, lt_reg_read(uart, LT_SPEC_STATUS, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 0);
	KUNIT_EXPECT_EQ(test, lt_reg_read(i2c, LT_SPEC_CONTROL, &v), 0);
	KUNIT_EXPECT_EQ(test, v, 0);
	KUNIT_EXPECT_EQ(test, lt_reg_read(i2c, 0x17, &v), -EINVAL);

	/* CHANNEL_CONTROL is never written */
	KUNIT_EXPECT_EQ(test, lt_reg_write(i2c, OBMF_CH_REG_CONTROL, en, en),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, lt_reg_write(i2c, OBMF_CH_REG_CONTROL, en, 0),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			lt_reg_write(i2c, OBMF_CH_REG_CONTROL,
				     OBMF_CH_CONTROL_RESET,
				     OBMF_CH_CONTROL_RESET),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, lt_reg_write(i2c, OBMF_CH_REG_STATUS, 0xff, 0),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, lt_reg_write(uart, LT_SPEC_CONTROL, clear, clear),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, lt_reg_write(i2c, LT_SPEC_CONTROL, 1, 1),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, lt_reg_write(i2c, LT_SPEC_CONTROL, clear, clear),
			0);
	KUNIT_EXPECT_EQ(test, lc->recovery, 0);

	i2c->dialect_data = NULL;
	KUNIT_EXPECT_EQ(test, lt_reg_read(i2c, OBMF_CH_REG_STATUS, &v),
			-ENODEV);
}

/* LSTP 1.0, 2 */
static void lt_usb_ids(struct kunit *test)
{
	static const struct usb_device_id ids[] = { OBMF_LSTP_USB_IDS{} };
	static const struct usb_device_id want[] = {
		{ USB_DEVICE_AND_INTERFACE_INFO(0x0955, 0xcf11, 0xff, 0x3f, 1),
		  .driver_info = (kernel_ulong_t)&obmf_lstp_usb_proto },
		{ USB_DEVICE_AND_INTERFACE_INFO(0x0424, 0x4940, 0xff, 0x3f, 1),
		  .driver_info = (kernel_ulong_t)&obmf_lstp_usb_proto },
		{ USB_DEVICE_AND_INTERFACE_INFO(0x174c, 0x2214, 0xff, 0x3f, 1),
		  .driver_info = (kernel_ulong_t)&obmf_lstp_usb_proto },
		{ USB_VENDOR_AND_INTERFACE_INFO(0x0955, 0xff, 0x3f, 1),
		  .driver_info = (kernel_ulong_t)&obmf_lstp_usb_proto },
		{ USB_VENDOR_AND_INTERFACE_INFO(0x0424, 0xff, 0x3f, 1),
		  .driver_info = (kernel_ulong_t)&obmf_lstp_usb_proto },
		{ USB_VENDOR_AND_INTERFACE_INFO(0x174c, 0xff, 0x3f, 1),
		  .driver_info = (kernel_ulong_t)&obmf_lstp_usb_proto },
		{}
	};
	const struct obmf_usb_profile *pf = obmf_lstp_usb_proto.profile;
	int i;

	KUNIT_EXPECT_PTR_EQ(test, obmf_lstp_usb_proto.dialect,
			    &obmf_lstp_dialect);
	KUNIT_EXPECT_FALSE(test, pf->func_desc);
	KUNIT_EXPECT_FALSE(test, pf->zlp);
	KUNIT_EXPECT_EQ(test, pf->max_packet, 512);
	KUNIT_EXPECT_EQ(test, pf->resp_timeout_floor_ms, 1000);

	KUNIT_ASSERT_EQ(test, ARRAY_SIZE(ids), ARRAY_SIZE(want));
	for (i = 0; i < ARRAY_SIZE(want); i++)
		KUNIT_EXPECT_MEMEQ_MSG(test, &ids[i], &want[i], sizeof(want[i]),
				       "entry %d", i);
}

#define LT_BLOBS 11

/*
 * A device on a fake transport: channel 0 answers READ_CONFIG from the
 * blobs (base fields and configuration), other channels get @reply. With
 * no @reply a GPIO channel answers from @gpio_level, apart from @sends.
 */
struct lt_fake {
	struct kunit *test;
	struct obmf_transport tp;
	struct obmf_core *core;
	u8 *blob[LT_BLOBS];
	size_t blob_len[LT_BLOBS];
	u8 blob_status[LT_BLOBS];
	u8 end_status;
	int reads;
	size_t served;
	int flip_read;
	int flip_off;
	unsigned long busy;
	size_t cut;
	size_t slice;
	bool silent;
	bool long_read;
	u8 out[LT_MSG];
	u8 sent[LT_MSG];
	size_t sent_len;
	unsigned int sent_timeout_ms;
	int sends;
	u8 reply[LT_MSG];
	size_t reply_len;
	/* the next uart_left UART requests are answered with uart_status */
	u8 uart_status;
	int uart_left;
	DECLARE_BITMAP(gpio_level, OBMF_GPIO_LINES);
	DECLARE_BITMAP(gpio_wo, OBMF_GPIO_LINES);
	u8 gpio_status;
	unsigned int gpio_prefix;
	u8 gpio_sent[LT_MSG];
	size_t gpio_sent_len;
	int gpio_sends;
	u8 gpio_out[LT_MSG];
};

static atomic_t lt_ids = ATOMIC_INIT(0);

/*
 * The GPIO device: GET_VALUE answers the level of each line, SET_VALUE
 * sets them (LSTP 1.0, 4.3.2). With @gpio_status a SET_VALUE sets its
 * first @gpio_prefix lines and answers that status. A GET_VALUE naming a
 * line of @gpio_wo is answered NOT_SUPP, as firmware that cannot read back
 * an output does.
 */
static bool lt_gpio_device(struct lt_fake *f, const u8 *m, size_t len)
{
	struct obmf_channel *ch = *m ? f->core->chans[*m] : NULL;
	const u8 *p = m + OBMF_HDR_LEN;
	size_t plen = len - OBMF_HDR_LEN, k, n = 0;
	u8 status = 0;
	u16 idx;

	if (!ch || ch->info.type != OBMF_TYPE_GPIO)
		return false;
	memcpy(f->gpio_sent, m, len);
	f->gpio_sent_len = len;
	f->gpio_sends++;
	if (m[1] == OBMF_LSTP_GPIO_SET_VALUE) {
		status = f->gpio_status;
		for (k = 0; k < plen / 3; k++) {
			if (status && k >= f->gpio_prefix)
				break;
			idx = get_unaligned_le16(p + 3 * k);
			if (idx < OBMF_GPIO_LINES)
				__assign_bit(idx, f->gpio_level, p[3 * k + 2]);
		}
	} else if (m[1] == OBMF_LSTP_GPIO_GET_VALUE) {
		for (k = 0; k < plen / 2; k++) {
			idx = get_unaligned_le16(p + 2 * k);
			if (idx < OBMF_GPIO_LINES &&
			    test_bit(idx, f->gpio_wo)) {
				status = OBMF_LSTP_NOT_SUPP;
				n = 0;
				break;
			}
			f->gpio_out[OBMF_HDR_LEN + n++] =
				idx < OBMF_GPIO_LINES &&
				test_bit(idx, f->gpio_level);
		}
	}
	obmf_lstp_hdr(f->gpio_out, m[0], OBMF_HDR_RQRESP | status, n);
	obmf_core_rx(f->tp.core, f->gpio_out, OBMF_HDR_LEN + n);
	return true;
}

static void lt_read_config(struct lt_fake *f, const u8 *m)
{
	const size_t base = sizeof(struct obmf_lstp_cfg_base);
	u16 off = get_unaligned_le16(m + 5);
	u16 want = get_unaligned_le16(m + 7);
	size_t cfg, n = 0;
	u8 nr = m[4];
	u8 st = 0x01;

	f->reads++;
	KUNIT_EXPECT_NE(f->test, want, 0);
	KUNIT_EXPECT_LE(f->test, OBMF_HDR_LEN + base + want, f->tp.max_rx_msg);
	/* bit n of busy: the nth read of the device header is busy */
	if (!nr && f->reads < BITS_PER_LONG && (f->busy & BIT(f->reads))) {
		st = 0x03;
	} else if (nr < LT_BLOBS && f->blob[nr]) {
		st = f->blob_status[nr];
		cfg = f->blob_len[nr] - base;
		if (!st && off >= cfg)
			st = f->end_status;
		if (!st) {
			n = off < cfg ? min_t(size_t, want, cfg - off) : 0;
			memcpy(f->out + OBMF_HDR_LEN, f->blob[nr], base);
			memcpy(f->out + OBMF_HDR_LEN + base,
			       f->blob[nr] + base + off, n);
			/* one byte more than asked, where the packet has room */
			if (f->long_read && n == want &&
			    OBMF_HDR_LEN + base + want < f->tp.max_rx_msg)
				n++;
			n += base;
		}
	}
	if (f->reads == f->flip_read)
		f->out[OBMF_HDR_LEN + f->flip_off] ^= 1;
	if (f->cut)
		n = min(n, f->cut);
	if (nr && f->slice)
		n = min(n, base + f->slice);
	if (nr && n > base)
		f->served += n - base;
	if (f->silent)
		return;
	obmf_lstp_hdr(f->out, 0, OBMF_HDR_RQRESP | st, n);
	obmf_core_rx(f->tp.core, f->out, OBMF_HDR_LEN + n);
}

static int lt_send(struct obmf_transport *tp, const void *buf, size_t len,
		   unsigned int timeout_ms)
{
	struct lt_fake *f = container_of(tp, struct lt_fake, tp);
	const u8 *m = buf;

	if (len > LT_MSG)
		return -EMSGSIZE;
	if (!m[0] && m[1] == OBMF_LSTP_READ_CONFIG && len == 9) {
		lt_read_config(f, m);
		return 0;
	}
	if (!f->reply_len && len >= OBMF_HDR_LEN && lt_gpio_device(f, m, len))
		return 0;
	memcpy(f->sent, buf, len);
	f->sent_len = len;
	f->sent_timeout_ms = timeout_ms;
	f->sends++;
	if (m[0] == 2 && f->uart_left > 0) {
		f->uart_left--;
		obmf_lstp_hdr(f->out, 2, OBMF_HDR_RQRESP | f->uart_status, 0);
		obmf_core_rx(tp->core, f->out, OBMF_HDR_LEN);
	} else if (f->reply_len) {
		obmf_core_rx(tp->core, f->reply, f->reply_len);
	}
	return 0;
}

static int lt_start(struct obmf_transport *tp)
{
	return 0;
}

static void lt_stop(struct obmf_transport *tp)
{
}

/* The case unbinds a failed core itself */
static void lt_release(struct obmf_transport *tp)
{
}

static const struct obmf_transport_ops lt_ops = {
	.start = lt_start,
	.stop = lt_stop,
	.quiesce = lt_stop,
	.resume = lt_start,
	.send = lt_send,
	.release = lt_release,
};

static void lt_blob(struct lt_fake *f, int nr, u8 type, const char *name,
		    const u8 *cfg, size_t len)
{
	const size_t base = sizeof(struct obmf_lstp_cfg_base);
	u8 *b = kunit_kzalloc(f->test, base + len, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(f->test, b);
	b[0] = type;
	b[1] = 0x01;
	memcpy(b + 2, name, strlen(name));
	memcpy(b + base, cfg, len);
	f->blob[nr] = b;
	f->blob_len[nr] = base + len;
}

static const u8 lt_i2c_cfg[11] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2 };
static const u8 lt_uart_cfg[15] = { 0x00, 0xc2, 0x01, 0x00, 8, 1, 0x00 };

/* An LSTP GPIO (LSTP 1.0, 4.3.1.2) at @p, the fields not given 0 */
static struct obmf_lstp_gpio_cfg *lt_gpio(u8 *p, const char *name, u8 dir,
					  u8 dflt, u8 drive, u8 bias, u8 led)
{
	struct obmf_lstp_gpio_cfg *g = (struct obmf_lstp_gpio_cfg *)p;

	memset(g, 0, sizeof(*g));
	memcpy(g->line.name, name, strlen(name));
	g->line.direction = dir;
	g->line.default_output = dflt;
	g->line.drive = drive;
	g->line.bias = bias;
	g->led_support = led;
	return g;
}

/*
 * The fake's function devices have the test names, which no function
 * driver matches, except in the suite of the function drivers: there the
 * driver binds the fake's devices, which reach no hardware
 */
static bool lt_fn_names;

static int __maybe_unused lt_fn_suite_init(struct kunit_suite *suite)
{
	lt_fn_names = true;
	return 0;
}

static void __maybe_unused lt_fn_suite_exit(struct kunit_suite *suite)
{
	lt_fn_names = false;
}

/*
 * Channel 0 with @n channels, I2C first, then UART, then GPIO: a block
 * with one open-drain output released at start, and an empty block
 */
static struct lt_fake *lt_create(struct kunit *test, size_t max_msg, int n)
{
	struct lt_fake *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	u8 mgmt[2] = { OBMF_LSTP_VERSION, n };
	struct obmf_lstp_gpio_cfg *g;
	u8 gpio[50] = { 1 };
	char name[32];

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->test = test;
	/* a name byte */
	f->flip_off = 2;
	g = lt_gpio(gpio + 1, "reset", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_Z,
		    OBMF_LSTP_GPIO_OPEN_DRAIN, 0, 0);
	g->line.bias_strength = cpu_to_le16(0x0102);
	lt_blob(f, 0, OBMF_LSTP_TYPE_MGMT, "lstp-kunit", mgmt, sizeof(mgmt));
	if (n > 0)
		lt_blob(f, 1, OBMF_LSTP_TYPE_I2C, "i2c", lt_i2c_cfg,
			sizeof(lt_i2c_cfg));
	if (n > 1)
		lt_blob(f, 2, OBMF_LSTP_TYPE_UART, "uart", lt_uart_cfg,
			sizeof(lt_uart_cfg));
	if (n > 2)
		lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio", gpio, sizeof(gpio));

	snprintf(name, sizeof(name), "lt.%d", atomic_inc_return(&lt_ids));
	f->tp.dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->tp.dev);
	f->tp.ops = &lt_ops;
	f->tp.test_names = !lt_fn_names;
	f->tp.max_tx_msg = max_msg;
	f->tp.max_rx_msg = max_msg;
	f->tp.resp_timeout_floor_ms = 100;
	return f;
}

static void lt_unbind_action(void *data)
{
	struct lt_fake *f = data;

	obmf_core_unbind(f->core, true);
}

static int lt_bind(struct lt_fake *f)
{
	int ret;

	KUNIT_ASSERT_EQ(f->test,
			obmf_dialect_attach(&f->tp, &obmf_lstp_usb_proto), 0);
	f->core = obmf_core_create(&f->tp);
	KUNIT_ASSERT_NOT_NULL(f->test, f->core);
	KUNIT_ASSERT_EQ(f->test,
			kunit_add_action_or_reset(f->test, lt_unbind_action, f),
			0);
	ret = obmf_core_bind(f->core);
	if (ret)
		return ret;
	flush_work(&f->core->bind_work);
	return f->core->bind_err;
}

/*
 * Three channels on 64-byte packets: the GPIO configuration takes two reads
 * and an empty one, and only its translation is kept
 */
static void lt_discover(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 3);
	const struct obmf_gpio_cfg_entry *e;
	struct obmf_channel *ch;
	struct obmf_lstp_ch *lc;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_STREQ(test, f->core->device_name, "lstp-kunit");
	KUNIT_EXPECT_EQ(test, f->core->vendor_id, 0);
	KUNIT_EXPECT_EQ(test, f->reads, 7);

	ch = f->core->chans[1];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_EXPECT_EQ(test, ch->info.type, OBMF_TYPE_I2C_CONTROLLER);
	KUNIT_EXPECT_EQ(test, ch->info.role, OBMF_ROLE_PRODUCER);
	KUNIT_EXPECT_STREQ(test, ch->info.name, "i2c");
	KUNIT_EXPECT_EQ(test, ch->info.max_req, 61);
	KUNIT_EXPECT_EQ(test, ch->info.max_resp, 64);
	KUNIT_ASSERT_EQ(test, ch->info.config_len, 2);
	KUNIT_EXPECT_EQ(test, ch->info.config[0], 1);
	KUNIT_EXPECT_EQ(test, ch->info.config[1], OBMF_I2C_OPT_BUS_RECOVERY);
	KUNIT_EXPECT_TRUE(test, ch->info.quirks.i2c_dev_retries);
	KUNIT_EXPECT_EQ(test, ch->status, 0x21);
	KUNIT_EXPECT_EQ(test, ch->control, OBMF_CH_CONTROL_ENABLE);
	lc = ch->dialect_data;
	KUNIT_EXPECT_EQ(test, lc->type, OBMF_LSTP_TYPE_I2C);
	KUNIT_EXPECT_EQ(test, lc->cfg_len, sizeof(lt_i2c_cfg));

	ch = f->core->chans[2];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_EXPECT_EQ(test, ch->info.type, OBMF_TYPE_SERIAL);
	KUNIT_EXPECT_STREQ(test, ch->info.name, "uart");
	KUNIT_EXPECT_EQ(test, ch->info.max_req, 63);
	KUNIT_EXPECT_EQ(test, ch->info.max_resp, 63);
	KUNIT_ASSERT_EQ(test, ch->info.config_len, 7);
	KUNIT_EXPECT_MEMEQ(test, ch->info.config, lt_uart_cfg, 7);

	ch = f->core->chans[3];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_EXPECT_EQ(test, ch->info.type, OBMF_TYPE_GPIO);
	KUNIT_EXPECT_EQ(test, ch->info.max_req, 61);
	KUNIT_ASSERT_EQ(test, ch->info.config_len,
			OBMF_GPIO_COUNT_LEN + sizeof(*e));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(ch->info.config), 1);
	e = (const void *)(ch->info.config + OBMF_GPIO_COUNT_LEN);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(e->index), 0);
	KUNIT_EXPECT_STREQ(test, (const char *)e->line.name, "reset");
	KUNIT_EXPECT_EQ(test, e->line.default_output, OBMF_LSTP_GPIO_HIGH);
	KUNIT_EXPECT_EQ(test, e->line.drive, OBMF_LSTP_GPIO_OPEN_DRAIN);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(e->line.bias_strength), 0x0102);
	lc = ch->dialect_data;
	KUNIT_EXPECT_EQ(test, lc->cfg_len, 50);
	KUNIT_EXPECT_NULL(test, f->core->chans[4]);
}

/* A device with channel 0 only; an I2C channel that does not retry */
static void lt_discover_none(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 512, 1);

	/* LSTP 1.0, 4.4.1: arbitration retries */
	f->blob[1][sizeof(struct obmf_lstp_cfg_base) + 10] = 0;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 3);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[1]);
	KUNIT_EXPECT_FALSE(test,
			   f->core->chans[1]->info.quirks.i2c_dev_retries);
	KUNIT_EXPECT_EQ(test, f->core->chans[1]->info.max_req, 509);
	KUNIT_EXPECT_EQ(test, f->core->chans[1]->info.max_resp, 512);

	f = lt_create(test, 64, 0);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 2);
	KUNIT_EXPECT_NULL(test, f->core->chans[1]);
}

/*
 * Channels that are not registered, each after one read; the others
 * still are
 */
static void lt_discover_skip(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 7);
	static const u8 oem[100];
	int i;

	f->blob[0][sizeof(struct obmf_lstp_cfg_base) + 1] = 7;
	lt_blob(f, 2, OBMF_LSTP_TYPE_I2C, "error", lt_i2c_cfg, 11);
	f->blob_status[2] = 0x01;
	lt_blob(f, 3, 0x06, "vendor", lt_i2c_cfg, 0);
	lt_blob(f, 4, 0x07, "reserved", lt_i2c_cfg, 0);
	lt_blob(f, 5, 0x80, "oem", oem, sizeof(oem));
	lt_blob(f, 6, OBMF_LSTP_TYPE_I2C, "empty i2c", lt_i2c_cfg, 0);
	lt_blob(f, 7, OBMF_LSTP_TYPE_UART, "short uart", lt_uart_cfg, 6);

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_NOT_NULL(test, f->core->chans[1]);
	for (i = 2; i <= 7; i++)
		KUNIT_EXPECT_NULL_MSG(test, f->core->chans[i], "channel %d", i);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 7 + 1);
}

/*
 * An I2C configuration of 1 or 10 bytes keeps the channel, with its speed,
 * recovery offered and no retries by the device; an empty one does not
 */
static void lt_discover_i2c_short(struct kunit *test)
{
	static const u8 speed[1] = { 2 };
	struct obmf_channel *ch;
	struct lt_fake *f;

	f = lt_create(test, 64, 1);
	lt_blob(f, 1, OBMF_LSTP_TYPE_I2C, "i2c", speed, sizeof(speed));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[1];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_EXPECT_NOT_NULL(test, ch->fdev);
	KUNIT_ASSERT_EQ(test, ch->info.config_len, 2);
	KUNIT_EXPECT_EQ(test, ch->info.config[0], 2);
	KUNIT_EXPECT_EQ(test, ch->info.config[1], OBMF_I2C_OPT_BUS_RECOVERY);
	KUNIT_EXPECT_FALSE(test, ch->info.quirks.i2c_dev_retries);

	f = lt_create(test, 64, 1);
	lt_blob(f, 1, OBMF_LSTP_TYPE_I2C, "i2c", lt_i2c_cfg, 10);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[1];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_EXPECT_NOT_NULL(test, ch->fdev);
	KUNIT_ASSERT_EQ(test, ch->info.config_len, 2);
	KUNIT_EXPECT_EQ(test, ch->info.config[0], 1);
	KUNIT_EXPECT_FALSE(test, ch->info.quirks.i2c_dev_retries);

	f = lt_create(test, 64, 1);
	lt_blob(f, 1, OBMF_LSTP_TYPE_I2C, "i2c", lt_i2c_cfg, 0);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_NULL(test, f->core->chans[1]);
}

/* The SPI configuration (LSTP 1.0, 4.2.1) as OBMF-ICP Table 84 */
static struct obmf_channel *lt_spi_bind(struct kunit *test, const u8 *cfg,
					size_t len, unsigned int *warns)
{
	struct lt_fake *f = lt_create(test, 64, 1);

	lt_blob(f, 1, OBMF_LSTP_TYPE_SPI, "spi", cfg, len);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	*warns = f->core->bind_warns;
	return f->core->chans[1];
}

/*
 * The device count, clamped to the two chip selects a command can name
 * with one warning, and the clock in both directions; mode 0, 8 bits.
 * No devices or no clock are passed on; a configuration without the
 * clock drops the channel.
 */
static void lt_discover_spi(struct kunit *test)
{
	static const u8 full[13] = { 2, 0x00, 0x36, 0x6e, 0x01, 1, 0, 2, 0 };
	static const u8 three[5] = { 3, 0x40, 0x42, 0x0f, 0x00 };
	static const u8 none[5] = { 0, 0x40, 0x42, 0x0f, 0x00 };
	static const u8 slow[5] = { 1, 0, 0, 0, 0 };
	const struct obmf_spi_cfg *c;
	struct obmf_channel *ch;
	unsigned int warns;

	ch = lt_spi_bind(test, full, sizeof(full), &warns);
	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_EXPECT_EQ(test, ch->info.type, OBMF_TYPE_SPI);
	KUNIT_EXPECT_EQ(test, ch->info.role, OBMF_ROLE_PRODUCER);
	KUNIT_EXPECT_NOT_NULL(test, ch->fdev);
	KUNIT_ASSERT_EQ(test, ch->info.config_len, 20);
	c = (const struct obmf_spi_cfg *)ch->info.config;
	KUNIT_EXPECT_EQ(test, le32_to_cpu(c->supported_flags), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(c->flags), 0);
	KUNIT_EXPECT_EQ(test, c->num_devices, 2);
	KUNIT_EXPECT_EQ(test, c->reserved[0] | c->reserved[1] | c->reserved[2],
			0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(c->rd_max_speed_hz), 24000000);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(c->wr_max_speed_hz), 24000000);
	KUNIT_EXPECT_EQ(test, warns, 0);

	ch = lt_spi_bind(test, three, sizeof(three), &warns);
	KUNIT_ASSERT_NOT_NULL(test, ch);
	c = (const struct obmf_spi_cfg *)ch->info.config;
	KUNIT_EXPECT_EQ(test, c->num_devices, 2);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(c->rd_max_speed_hz), 1000000);
	KUNIT_EXPECT_EQ(test, warns, 1);

	ch = lt_spi_bind(test, none, sizeof(none), &warns);
	KUNIT_ASSERT_NOT_NULL(test, ch);
	c = (const struct obmf_spi_cfg *)ch->info.config;
	KUNIT_EXPECT_EQ(test, c->num_devices, 0);
	KUNIT_EXPECT_EQ(test, warns, 0);

	ch = lt_spi_bind(test, slow, sizeof(slow), &warns);
	KUNIT_ASSERT_NOT_NULL(test, ch);
	c = (const struct obmf_spi_cfg *)ch->info.config;
	KUNIT_EXPECT_EQ(test, c->num_devices, 1);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(c->rd_max_speed_hz), 0);
	KUNIT_EXPECT_EQ(test, le32_to_cpu(c->wr_max_speed_hz), 0);

	ch = lt_spi_bind(test, full, 4, &warns);
	KUNIT_EXPECT_NULL(test, ch);
	KUNIT_EXPECT_EQ(test, warns, 1);
}

/*
 * A UART configuration of 7 or 14 bytes keeps the channel with its line
 * fields; one of 6 does not
 */
static void lt_discover_uart_short(struct kunit *test)
{
	static const size_t lens[] = { 7, 14 };
	struct obmf_channel *ch;
	struct lt_fake *f;
	int i;

	for (i = 0; i < ARRAY_SIZE(lens); i++) {
		f = lt_create(test, 64, 2);
		lt_blob(f, 2, OBMF_LSTP_TYPE_UART, "uart", lt_uart_cfg,
			lens[i]);
		KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
		ch = f->core->chans[2];
		KUNIT_ASSERT_NOT_NULL_MSG(test, ch, "%zu bytes", lens[i]);
		KUNIT_EXPECT_NOT_NULL(test, ch->fdev);
		KUNIT_ASSERT_EQ(test, ch->info.config_len, 7);
		KUNIT_EXPECT_MEMEQ(test, ch->info.config, lt_uart_cfg, 7);
	}

	f = lt_create(test, 64, 2);
	lt_blob(f, 2, OBMF_LSTP_TYPE_UART, "uart", lt_uart_cfg, 6);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_NOT_NULL(test, f->core->chans[1]);
	KUNIT_EXPECT_NULL(test, f->core->chans[2]);
}

/*
 * Eight channels fill all but 6144 bytes of the budget; the ninth
 * reaches it and is ignored, and the tenth is not read
 */
static void lt_discover_budget(struct kunit *test)
{
	const size_t len = 32000;
	u8 *cfg = kunit_kzalloc(test, len, GFP_KERNEL);
	struct lt_fake *f;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, cfg);
	f = lt_create(test, 512, 0);
	f->blob[0][sizeof(struct obmf_lstp_cfg_base) + 1] = 10;
	for (i = 1; i <= 9; i++)
		lt_blob(f, i, OBMF_LSTP_TYPE_GPIO, "big", cfg, len);
	lt_blob(f, 10, OBMF_LSTP_TYPE_I2C, "i2c", lt_i2c_cfg,
		sizeof(lt_i2c_cfg));

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	for (i = 1; i <= 8; i++)
		KUNIT_EXPECT_NOT_NULL_MSG(test, f->core->chans[i], "channel %d",
					  i);
	KUNIT_EXPECT_NULL(test, f->core->chans[9]);
	KUNIT_EXPECT_NULL(test, f->core->chans[10]);
	/*
	 * 66 slices of up to 490 bytes and an empty one each, then 12 and one
	 * of 264
	 */
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 8 * 67 + 13 + 1);
	KUNIT_EXPECT_EQ(test, f->served, SZ_256K);
}

/*
 * An LSTP version other than 1, channel 0 of another type, and channel
 * 0's type on a channel
 */
static void lt_discover_fail(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 1);

	f->blob[0][sizeof(struct obmf_lstp_cfg_base)] = 2;
	KUNIT_EXPECT_EQ(test, lt_bind(f), -ENODEV);
	KUNIT_EXPECT_EQ(test, f->reads, 1);

	/* each pass ends at the type, the last with -ENODEV */
	f = lt_create(test, 64, 1);
	f->blob[0][0] = OBMF_LSTP_TYPE_I2C;
	KUNIT_EXPECT_EQ(test, lt_bind(f), -ENODEV);
	KUNIT_EXPECT_EQ(test, f->reads, 3);

	/* only the first reply has another type: the second pass binds */
	f = lt_create(test, 64, 1);
	f->flip_read = 1;
	f->flip_off = 0;
	KUNIT_EXPECT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 3);
	KUNIT_EXPECT_NOT_NULL(test, f->core->chans[1]);

	f = lt_create(test, 64, 1);
	f->blob[1][0] = OBMF_LSTP_TYPE_MGMT;
	KUNIT_EXPECT_EQ(test, lt_bind(f), -EPROTO);
	KUNIT_EXPECT_EQ(test, f->reads, 3 * 2);
}

/*
 * Still full at 0x7fff: cut there; a reply longer than asked: refused.
 * Expected: the zeroed GPIO table's warning, bytes after 255 empty blocks.
 */
static void lt_discover_cut(struct kunit *test)
{
	const size_t len = OBMF_LSTP_CFG_END + 10;
	u8 *cfg = kunit_kzalloc(test, len, GFP_KERNEL);
	struct obmf_lstp_ch *lc;
	struct lt_fake *f;

	KUNIT_ASSERT_NOT_NULL(test, cfg);
	f = lt_create(test, 512, 0);
	f->blob[0][sizeof(struct obmf_lstp_cfg_base) + 1] = 1;
	lt_blob(f, 1, OBMF_LSTP_TYPE_GPIO, "big", cfg, len);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[1]);
	lc = f->core->chans[1]->dialect_data;
	KUNIT_EXPECT_EQ(test, lc->cfg_len, (size_t)OBMF_LSTP_CFG_END);
	KUNIT_EXPECT_EQ(test, f->reads,
			2 + DIV_ROUND_UP(OBMF_LSTP_CFG_END, 490));

	f = lt_create(test, 512, 0);
	f->blob[0][sizeof(struct obmf_lstp_cfg_base) + 1] = 1;
	lt_blob(f, 1, OBMF_LSTP_TYPE_GPIO, "big", cfg, len);
	f->long_read = true;
	KUNIT_EXPECT_EQ(test, lt_bind(f), -EPROTO);
}

/*
 * One channel of @type on packets of @max_msg, its slices cut to @slice.
 * A zeroed GPIO table is 255 empty blocks; the bytes after them get a
 * warning.
 */
static struct lt_fake *lt_table(struct kunit *test, size_t max_msg, u8 type,
				const u8 *cfg, size_t len, size_t slice)
{
	struct lt_fake *f = lt_create(test, max_msg, 0);

	f->blob[0][sizeof(struct obmf_lstp_cfg_base) + 1] = 1;
	lt_blob(f, 1, type, "table", cfg, len);
	f->slice = slice;
	return f;
}

/*
 * A table whose slices end short of the window is read to an empty slice;
 * any configuration ends at an error status other than busy after the
 * first slice. An error on the first slice ignores the channel, and so do
 * busy and short slices past a channel's or a pass's bound. Full slices
 * do not count against it.
 */
static void lt_discover_table(struct kunit *test)
{
	const size_t len = 980, big = 32000;
	const u8 gpio = OBMF_LSTP_TYPE_GPIO;
	u8 *cfg = kunit_kzalloc(test, big, GFP_KERNEL);
	struct obmf_lstp_ch *lc;
	struct lt_fake *f;
	int i;

	KUNIT_ASSERT_NOT_NULL(test, cfg);
	f = lt_table(test, 512, gpio, cfg, len, 481);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[1]);
	lc = f->core->chans[1]->dialect_data;
	KUNIT_EXPECT_EQ(test, lc->cfg_len, len);
	/* 481, 481 and 18 bytes, then an empty slice */
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 4 + 1);

	f = lt_table(test, 512, gpio, cfg, len, 481);
	f->end_status = 0x01;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[1]);
	lc = f->core->chans[1]->dialect_data;
	KUNIT_EXPECT_EQ(test, lc->cfg_len, len);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 4 + 1);

	/* busy there: the device has more */
	f = lt_table(test, 512, gpio, cfg, len, 481);
	f->end_status = 0x03;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_NULL(test, f->core->chans[1]);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 4 + 1);

	f = lt_table(test, 512, gpio, cfg, len, 481);
	f->blob_status[1] = 0x01;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_NULL(test, f->core->chans[1]);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 1 + 1);

	/* one byte a slice: kept up to the bound, then ignored */
	f = lt_table(test, 512, gpio, cfg, OBMF_LSTP_CFG_SHORT_MAX, 1);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[1]);
	lc = f->core->chans[1]->dialect_data;
	KUNIT_EXPECT_EQ(test, lc->cfg_len, (size_t)OBMF_LSTP_CFG_SHORT_MAX);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + OBMF_LSTP_CFG_SHORT_MAX + 1 + 1);

	f = lt_table(test, 512, gpio, cfg, OBMF_LSTP_CFG_SHORT_MAX + 1, 1);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_NULL(test, f->core->chans[1]);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + OBMF_LSTP_CFG_SHORT_MAX + 1 + 1);

	/*
	 * Channels of 683 one-byte slices: the eighth passes the pass's bound
	 * after 681 and is ignored, and the ninth is not read
	 */
	f = lt_create(test, 512, 0);
	f->blob[0][sizeof(struct obmf_lstp_cfg_base) + 1] = 9;
	for (i = 1; i <= 9; i++)
		lt_blob(f, i, gpio, "table", cfg, OBMF_LSTP_CFG_SHORT_MAX);
	f->slice = 1;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	for (i = 1; i <= 7; i++)
		KUNIT_EXPECT_NOT_NULL_MSG(test, f->core->chans[i], "channel %d",
					  i);
	KUNIT_EXPECT_NULL(test, f->core->chans[8]);
	KUNIT_EXPECT_NULL(test, f->core->chans[9]);
	/* a read a byte, seven empty ones and the header twice */
	KUNIT_EXPECT_EQ(test, f->reads, OBMF_LSTP_PASS_SHORT_MAX + 1 + 7 + 2);

	/* 761 full slices of 42 bytes, one of 38 and an empty one */
	f = lt_table(test, 64, gpio, cfg, big, 0);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[1]);
	lc = f->core->chans[1]->dialect_data;
	KUNIT_EXPECT_EQ(test, lc->cfg_len, big);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 763 + 1);

	/* eight such channels: 6096 slices, 8 of them short, all kept */
	f = lt_create(test, 64, 0);
	f->blob[0][sizeof(struct obmf_lstp_cfg_base) + 1] = 8;
	for (i = 1; i <= 8; i++)
		lt_blob(f, i, gpio, "table", cfg, big);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	for (i = 1; i <= 8; i++) {
		KUNIT_ASSERT_NOT_NULL_MSG(test, f->core->chans[i], "channel %d",
					  i);
		lc = f->core->chans[i]->dialect_data;
		KUNIT_EXPECT_EQ(test, lc->cfg_len, big);
	}
	KUNIT_EXPECT_EQ(test, f->reads, 2 + 8 * 763);
	KUNIT_EXPECT_EQ(test, f->served, 8 * big);

	/* not a table: two full slices, then an error status at the end */
	f = lt_table(test, 64, OBMF_LSTP_TYPE_IPMI, cfg, 84, 0);
	f->end_status = 0x01;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[1]);
	lc = f->core->chans[1]->dialect_data;
	KUNIT_EXPECT_EQ(test, lc->cfg_len, (size_t)84);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 3 + 1);
}

static void lt_kfree(void *p)
{
	kfree(p);
}

/* A GPIO channel record of @f's core, @raw translated into it */
static struct obmf_channel *lt_gpio_translate(struct kunit *test,
					      struct lt_fake *f, const u8 *raw,
					      size_t len, int ret)
{
	struct obmf_channel *ch = lt_chan(test, OBMF_TYPE_GPIO);

	ch->core = f->core;
	ch->info.max_req = 61;
	ch->info.max_resp = 61;
	KUNIT_EXPECT_EQ(test, obmf_lstp_gpio_config(ch, raw, len), ret);
	if (ch->config)
		KUNIT_ASSERT_EQ(test,
				kunit_add_action_or_reset(test, lt_kfree,
							  ch->config),
				0);
	return ch;
}

static const struct obmf_gpio_cfg_entry *lt_gpio_entry(struct obmf_channel *ch,
						       int i)
{
	return (const void *)(ch->info.config + OBMF_GPIO_COUNT_LEN +
			      i * sizeof(struct obmf_gpio_cfg_entry));
}

/*
 * A GPIO table in OBMF-ICP form: each line indexed by its position across
 * the blocks, the lines out of range and the LED outputs left out, Z as the
 * released level, a line the table cuts dropped. Expected warnings: the cut
 * line 12, four lines out of range, LED support on two lines.
 */
static void lt_gpio_table(struct kunit *test)
{
	static const u16 want[] = { 0, 1, 2, 3, 6, 7, 11 };
	const size_t esz = sizeof(struct obmf_lstp_gpio_cfg);
	const struct obmf_gpio_cfg_entry *e;
	struct lt_fake *f = lt_create(test, 64, 0);
	struct obmf_lstp_gpio_cfg *g;
	struct obmf_channel *ch;
	u8 raw[599], *p = raw;
	int i;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	*p++ = 6;
	g = lt_gpio(p, "in0", OBMF_LSTP_GPIO_IN, 0, 0, 2, 0);
	g->line.persist = 1;
	g->line.bias_strength = cpu_to_le16(0x1234);
	g->line.drive_strength = cpu_to_le16(0x5678);
	g->line.slew_rate = cpu_to_le16(0x9abc);
	g->line.debounce = cpu_to_le16(0xdef0);
	g->reserved[0] = 0xff;
	lt_gpio(p += esz, "od", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_Z,
		OBMF_LSTP_GPIO_OPEN_DRAIN, 0, 0);
	lt_gpio(p += esz, "os", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_Z,
		OBMF_LSTP_GPIO_OPEN_SOURCE, 0, 0);
	lt_gpio(p += esz, "pp", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_Z, 0, 0, 0);
	lt_gpio(p += esz, "led", OBMF_LSTP_GPIO_OUT, 0, 0, 0,
		OBMF_LSTP_GPIO_LED_HOLD);
	lt_gpio(p += esz, "dir", 2, 0, 0, 0, 0);
	p += esz;
	*p++ = 5;
	lt_gpio(p, "in-led", OBMF_LSTP_GPIO_IN, 0, 0, 0,
		OBMF_LSTP_GPIO_LED_INTERPOLATE);
	lt_gpio(p += esz, "led3", OBMF_LSTP_GPIO_OUT, 0, 0, 0, 3);
	lt_gpio(p += esz, "bias", OBMF_LSTP_GPIO_OUT, 0, 0, 3, 0);
	lt_gpio(p += esz, "default", OBMF_LSTP_GPIO_OUT, 3, 0, 0, 0);
	lt_gpio(p += esz, "drive", OBMF_LSTP_GPIO_OUT, 0, 3, 0, 0);
	p += esz;
	*p++ = 2;
	lt_gpio(p, "\xff\xfex", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_HIGH, 0, 0,
		0);
	p += esz;
	memset(p, 0, 20);
	KUNIT_ASSERT_EQ(test, p + 20 - raw, sizeof(raw));

	ch = lt_gpio_translate(test, f, raw, sizeof(raw), 0);
	KUNIT_ASSERT_EQ(test, ch->info.config_len,
			OBMF_GPIO_COUNT_LEN + ARRAY_SIZE(want) * sizeof(*e));
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(ch->info.config),
			ARRAY_SIZE(want));
	for (i = 0; i < ARRAY_SIZE(want); i++) {
		e = lt_gpio_entry(ch, i);
		KUNIT_EXPECT_EQ_MSG(test, le16_to_cpu(e->index), want[i],
				    "entry %d", i);
		KUNIT_EXPECT_FALSE(test, memchr_inv(e->reserved, 0, 3));
	}
	e = lt_gpio_entry(ch, 0);
	KUNIT_EXPECT_STREQ(test, (const char *)e->line.name, "in0");
	KUNIT_EXPECT_EQ(test, e->line.direction, OBMF_LSTP_GPIO_IN);
	KUNIT_EXPECT_EQ(test, e->line.persist, 1);
	KUNIT_EXPECT_EQ(test, e->line.bias, 2);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(e->line.bias_strength), 0x1234);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(e->line.drive_strength), 0x5678);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(e->line.slew_rate), 0x9abc);
	KUNIT_EXPECT_EQ(test, le16_to_cpu(e->line.debounce), 0xdef0);
	KUNIT_EXPECT_EQ(test, lt_gpio_entry(ch, 1)->line.default_output,
			OBMF_LSTP_GPIO_HIGH);
	KUNIT_EXPECT_EQ(test, lt_gpio_entry(ch, 2)->line.default_output,
			OBMF_LSTP_GPIO_LOW);
	e = lt_gpio_entry(ch, 3);
	KUNIT_EXPECT_EQ(test, e->line.direction, OBMF_LSTP_GPIO_OUT);
	KUNIT_EXPECT_EQ(test, e->line.default_output, OBMF_LSTP_GPIO_LOW);
	KUNIT_EXPECT_EQ(test, lt_gpio_entry(ch, 4)->line.direction,
			OBMF_LSTP_GPIO_IN);
	e = lt_gpio_entry(ch, 6);
	KUNIT_EXPECT_EQ(test, e->line.name[0], 0xff);
	KUNIT_EXPECT_EQ(test, e->line.name[1], 0xfe);
	KUNIT_EXPECT_EQ(test, e->line.default_output, OBMF_LSTP_GPIO_HIGH);

	KUNIT_EXPECT_EQ(test, ch->info.quirks.gpio_per_get, 30);
	KUNIT_EXPECT_EQ(test, ch->info.quirks.gpio_per_set, 20);

	ch = lt_gpio_translate(test, f, raw, 0, -EINVAL);
	KUNIT_EXPECT_NULL(test, ch->info.config);
}

/*
 * 255 blocks at most: the bytes after them are ignored, with one warning.
 * Lines past index 4095 are left out, with one warning, and none of the
 * others wraps.
 */
static void lt_gpio_table_bounds(struct kunit *test)
{
	const size_t esz = sizeof(struct obmf_lstp_gpio_cfg);
	const size_t block = 1 + 255 * esz, len = 16 * block + 1 + 17 * esz;
	struct lt_fake *f = lt_create(test, 64, 0);
	struct obmf_channel *ch;
	int i;
	u8 *raw = kunit_kzalloc(test, len, GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, raw);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	raw[254] = 1;
	lt_gpio(raw + 255, "last", OBMF_LSTP_GPIO_IN, 0, 0, 0, 0);
	raw[255 + esz] = 1;
	ch = lt_gpio_translate(test, f, raw, 255 + 2 * (1 + esz) - 1, 0);
	KUNIT_ASSERT_EQ(test, ch->info.config_len,
			OBMF_GPIO_COUNT_LEN +
				sizeof(struct obmf_gpio_cfg_entry));
	KUNIT_EXPECT_EQ(test, le16_to_cpu(lt_gpio_entry(ch, 0)->index), 0);
	KUNIT_EXPECT_STREQ(test, (const char *)lt_gpio_entry(ch, 0)->line.name,
			   "last");

	memset(raw, 0, len);
	for (i = 0; i < 16; i++)
		raw[i * block] = 255;
	raw[16 * block] = 17;
	ch = lt_gpio_translate(test, f, raw, len, 0);
	KUNIT_ASSERT_EQ(test, get_unaligned_le16(ch->info.config), 4096);
	for (i = 0; i < 4096; i++)
		if (le16_to_cpu(lt_gpio_entry(ch, i)->index) != i) {
			KUNIT_FAIL(test, "entry %d has index %u", i,
				   le16_to_cpu(lt_gpio_entry(ch, i)->index));
			break;
		}
}

/*
 * The lines one Get and one Set carry on LSTP 1.0, 4.3.2: GET_VALUE takes
 * 2 bytes a line out and 1 back, rebuilt as OBMF-ICP's 1 + 2N; SET_VALUE
 * takes 3 a line out
 */
static void lt_gpio_quirks(struct kunit *test)
{
	static const size_t msg[][2] = {
		{ 64, 64 },
		{ 512, 512 },
		{ 512, 64 },
		{ 64, 512 },
	};
	static const u16 want[][2] = {
		{ 30, 20 },
		{ 254, 169 },
		{ 30, 169 },
		{ 30, 20 },
	};
	const struct obmf_channel_quirks *q;
	struct lt_fake *f;
	int i;

	for (i = 0; i < ARRAY_SIZE(msg); i++) {
		f = lt_create(test, 512, 3);
		f->tp.max_tx_msg = msg[i][0];
		f->tp.max_rx_msg = msg[i][1];
		KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
		KUNIT_ASSERT_NOT_NULL(test, f->core->chans[3]);
		q = &f->core->chans[3]->info.quirks;
		KUNIT_EXPECT_EQ_MSG(test, q->gpio_per_get, want[i][0],
				    "%zu/%zu", msg[i][0], msg[i][1]);
		KUNIT_EXPECT_EQ_MSG(test, q->gpio_per_set, want[i][1],
				    "%zu/%zu", msg[i][0], msg[i][1]);
	}
	/* the last: 61 bytes a request, of which 60 GET_VALUE offsets */
	KUNIT_EXPECT_EQ(test, f->core->chans[3]->info.max_req, 61);
}

/* A slice whose base fields differ, then a header that changes */
static void lt_discover_again(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 3);

	f->flip_read = 5;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 5 + 7);
	KUNIT_EXPECT_NOT_NULL(test, f->core->chans[3]);

	f = lt_create(test, 64, 3);
	f->flip_read = 7;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 14);
	KUNIT_EXPECT_STREQ(test, f->core->device_name, "lstp-kunit");
}

/*
 * A busy device header, first or last in a pass, retries the pass; busy
 * in the last pass, first or last, fails the probe as busy
 */
static void lt_discover_busy(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 1);

	f->busy = BIT(1);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 1 + 3);
	KUNIT_EXPECT_NOT_NULL(test, f->core->chans[1]);

	f = lt_create(test, 64, 1);
	f->busy = BIT(3);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->reads, 3 + 3);

	f = lt_create(test, 64, 1);
	f->busy = BIT(1) | BIT(2) | BIT(3);
	KUNIT_EXPECT_EQ(test, lt_bind(f), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->reads, 3);

	f = lt_create(test, 64, 1);
	f->busy = BIT(1) | BIT(2) | BIT(5);
	KUNIT_EXPECT_EQ(test, lt_bind(f), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->reads, 5);
}

/*
 * A management configuration shorter than its two fields, and a reply
 * shorter than the base fields, each fail every pass
 */
static void lt_discover_short(struct kunit *test)
{
	static const u8 mgmt[1] = { OBMF_LSTP_VERSION };
	struct lt_fake *f = lt_create(test, 64, 1);

	lt_blob(f, 0, OBMF_LSTP_TYPE_MGMT, "short", mgmt, sizeof(mgmt));
	KUNIT_EXPECT_EQ(test, lt_bind(f), -EPROTO);
	KUNIT_EXPECT_EQ(test, f->reads, 3);

	f = lt_create(test, 64, 1);
	f->cut = sizeof(struct obmf_lstp_cfg_base) - 1;
	KUNIT_EXPECT_EQ(test, lt_bind(f), -EPROTO);
	KUNIT_EXPECT_EQ(test, f->reads, 3);
}

/* A device that never answers: each pass times out, then the bind */
static void lt_discover_silent(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 1);

	f->silent = true;
	KUNIT_EXPECT_EQ(test, lt_bind(f), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->reads, 3);
}

static void lt_set_reply(struct lt_fake *f, const u8 *m, size_t len)
{
	memcpy(f->reply, m, len);
	f->reply_len = len;
}

/* An I2C write-read through the core, then the statuses as errnos */
static void lt_i2c_round_trip(struct kunit *test)
{
	static const u8 wire[] = { 1, 0x04, 4, 0, 0x50, 2, 0, 0x10 };
	static const u8 reply[] = { 1, 0x80, 2, 0, 0x5a, 0xa5 };
	static const u8 resp_want[] = { 2, 0, 0, 0x5a, 0xa5 };
	static const u8 req[] = { 0x50, 2, 0, 0x10 };
	static const u8 wr[] = { 0x50, 0, 0, 0xaa };
	static const struct {
		u8 status;
		int err;
	} st[] = {
		{ 0x01, -EIO },	     { 0x02, -ETIME },	{ 0x03, -EBUSY },
		{ 0x04, -ENXIO },    { 0x05, -EAGAIN }, { 0x06, -EOPNOTSUPP },
		{ 0x07, -EMSGSIZE }, { 0x09, -EIO },
	};
	struct lt_fake *f = lt_create(test, 64, 2);
	struct obmf_channel *ch;
	u8 resp[16], m[4];
	size_t len;
	long errs;
	int i;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[1];
	lt_set_reply(f, reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x03, req, sizeof(req), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(wire));
	KUNIT_EXPECT_MEMEQ(test, f->sent, wire, sizeof(wire));
	KUNIT_ASSERT_EQ(test, len, sizeof(resp_want));
	KUNIT_EXPECT_MEMEQ(test, resp, resp_want, sizeof(resp_want));
	/* The decoded response does not fit the caller's buffer */
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x03, req, sizeof(req), resp, 4,
					&len, 0),
			-EPROTO);

	for (i = 0; i < ARRAY_SIZE(st); i++) {
		obmf_lstp_hdr(m, 1, 0x80 | st[i].status, 0);
		lt_set_reply(f, m, sizeof(m));
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_ch_request(ch, 0x03, req, sizeof(req),
						    resp, sizeof(resp), &len,
						    0),
				    st[i].err, "status %#x", st[i].status);
	}
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_STATUS_ERRORS]),
			ARRAY_SIZE(st));
	/* ERROR and an unknown status report; NACK does not */
	errs = atomic_long_read(&f->core->events[OBMF_EV_DEVICE_ERROR]);
	KUNIT_EXPECT_EQ(test, errs, 2);

	/* A write's reply with data */
	lt_set_reply(f, reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, wr, sizeof(wr), resp,
					sizeof(resp), &len, 0),
			-EPROTO);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_PROTOCOL_ERRORS]), 2);
	/* Refused before it is sent */
	i = f->sends;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x04, wr, sizeof(wr), resp,
					sizeof(resp), &len, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, f->sends, i);
}

/* Channel 1 an SPI channel of one device at 1 MHz */
static struct lt_fake *lt_spi_fake(struct kunit *test, size_t max_msg)
{
	static const u8 cfg[5] = { 1, 0x40, 0x42, 0x0f, 0x00 };
	struct lt_fake *f = lt_create(test, max_msg, 1);

	lt_blob(f, 1, OBMF_LSTP_TYPE_SPI, "spi", cfg, sizeof(cfg));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[1]);
	KUNIT_ASSERT_EQ(test, f->core->chans[1]->info.type, OBMF_TYPE_SPI);
	return f;
}

/* An SPI write-read through the core, then the statuses as errnos */
static void lt_spi_round_trip(struct kunit *test)
{
	static const u8 wire[] = { 1, 0x33, 2, 0, 0x9f, 0x00 };
	static const u8 reply[] = { 1, 0x80, 2, 0, 0xff, 0xef };
	static const u8 resp_want[] = { 2, 0, 0xff, 0xef };
	static const u8 req[] = { 2, 0, 2, 0, 0x9f, 0x00 };
	static const u8 wr[] = { 1, 0, 0, 0, 0x06 };
	static const struct {
		u8 status;
		int err;
	} st[] = {
		{ 0x01, -EIO },	     { 0x02, -ETIME }, { 0x03, -EBUSY },
		{ 0x04, -EIO },	     { 0x05, -EIO },   { 0x06, -EOPNOTSUPP },
		{ 0x07, -EMSGSIZE }, { 0x09, -EIO },
	};
	struct lt_fake *f = lt_spi_fake(test, 64);
	struct obmf_channel *ch = f->core->chans[1];
	u8 resp[16], m[4];
	size_t len;
	int i;

	lt_set_reply(f, reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x33, req, sizeof(req), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(wire));
	KUNIT_EXPECT_MEMEQ(test, f->sent, wire, sizeof(wire));
	KUNIT_ASSERT_EQ(test, len, sizeof(resp_want));
	KUNIT_EXPECT_MEMEQ(test, resp, resp_want, sizeof(resp_want));

	for (i = 0; i < ARRAY_SIZE(st); i++) {
		obmf_lstp_hdr(m, 1, 0x80 | st[i].status, 0);
		lt_set_reply(f, m, sizeof(m));
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_ch_request(ch, 0x33, req, sizeof(req),
						    resp, sizeof(resp), &len,
						    0),
				    st[i].err, "status %#x", st[i].status);
	}
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_STATUS_ERRORS]),
			ARRAY_SIZE(st));

	/* A write's reply with data: success, nothing read */
	lt_set_reply(f, reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x22, wr, sizeof(wr), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_ASSERT_EQ(test, len, 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le16(resp), 0);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_PROTOCOL_ERRORS]), 0);
	/* Refused before it is sent */
	i = f->sends;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0xa2, wr, sizeof(wr), resp,
					sizeof(resp), &len, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, f->sends, i);
}

/*
 * At 64-byte packets P is 60: a write and a write-read of P bytes fill
 * the link, one byte more does not; a read reply of P bytes fits the
 * channel's response limit
 */
static void lt_spi_limit(struct kunit *test)
{
	static const u8 cmds[] = { 0x22, 0x23 };
	struct lt_fake *f = lt_spi_fake(test, 64);
	struct obmf_channel *ch = f->core->chans[1];
	size_t len, cap, p;
	u8 buf[LT_MSG], out[LT_MSG];
	int i;

	cap = f->core->tp->max_tx_msg;
	p = cap - OBMF_HDR_LEN;
	KUNIT_EXPECT_EQ(test, ch->info.max_req, p + 5);
	KUNIT_EXPECT_EQ(test, ch->info.max_resp, p + 3);
	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		len = OBMF_HDR_LEN + 5 + p;
		memset(buf, 0xa5, sizeof(buf));
		buf[0] = 1;
		buf[1] = 0;
		buf[4] = cmds[i];
		put_unaligned_le16(p, buf + 5);
		put_unaligned_le16(cmds[i] & 1 ? p : 0, buf + 7);
		KUNIT_EXPECT_EQ_MSG(test, obmf_lstp_encode(ch, buf, len, cap),
				    (ssize_t)cap, "command %#x", cmds[i]);
		buf[4] = cmds[i];
		put_unaligned_le16(p + 1, buf + 5);
		put_unaligned_le16(cmds[i] & 1 ? p + 1 : 0, buf + 7);
		KUNIT_EXPECT_EQ_MSG(test,
				    obmf_lstp_encode(ch, buf, len + 1, cap),
				    -EMSGSIZE, "command %#x", cmds[i]);
	}
	obmf_lstp_hdr(buf, 1, OBMF_HDR_RQRESP, p);
	KUNIT_EXPECT_EQ(test,
			obmf_lstp_decode(ch, buf, OBMF_HDR_LEN + p, out,
					 OBMF_HDR_LEN + ch->info.max_resp, 0x21,
					 NULL),
			(ssize_t)(OBMF_HDR_LEN + ch->info.max_resp));
}

/* A UART write through the core, ACK then NACK */
static void lt_uart_round_trip(struct kunit *test)
{
	static const u8 wire[] = { 2, 0x08, 2, 0, 'h', 'i' };
	static const u8 req[] = { 2, 0, 'h', 'i' };
	static const u8 ack[] = { 0, 2, 0 };
	static const u8 nack[] = { OBMF_SERIAL_NACK, 0, 0 };
	struct lt_fake *f = lt_create(test, 64, 2);
	struct obmf_channel *ch;
	u8 resp[8], m[4];
	size_t len;
	long errs;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[2];
	obmf_lstp_hdr(m, 2, 0x80, 0);
	lt_set_reply(f, m, sizeof(m));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, req, sizeof(req), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_EXPECT_MEMEQ(test, f->sent, wire, sizeof(wire));
	KUNIT_ASSERT_EQ(test, len, sizeof(ack));
	KUNIT_EXPECT_MEMEQ(test, resp, ack, sizeof(ack));

	obmf_lstp_hdr(m, 2, 0x80 | OBMF_LSTP_NACK, 0);
	lt_set_reply(f, m, sizeof(m));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, req, sizeof(req), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_ASSERT_EQ(test, len, sizeof(nack));
	KUNIT_EXPECT_MEMEQ(test, resp, nack, sizeof(nack));

	obmf_lstp_hdr(m, 2, 0x80 | OBMF_LSTP_ARB_LOST, 0);
	lt_set_reply(f, m, sizeof(m));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, req, sizeof(req), resp,
					sizeof(resp), &len, 0),
			-EIO);
	/* A NACK is the device's answer, ARB_LOST its failure */
	errs = atomic_long_read(&f->core->events[OBMF_EV_DEVICE_ERROR]);
	KUNIT_EXPECT_EQ(test, errs, 1);
}

/*
 * GPIO Gets and Sets through the core on 64-byte packets: the most lines
 * the capacities allow fit both ways, one more does not, a malformed
 * reply is a protocol error. Set Interrupt Config of one line goes out;
 * of two lines, and Get Interrupt Config, are refused before anything is
 * sent.
 */
static void lt_gpio_round_trip(struct kunit *test)
{
	static const u8 get_wire[] = { 3, 0x00, 4, 0, 0x00, 0x00, 0x05, 0x00 };
	static const u8 set_wire[] = { 3, 0x01, 3, 0, 0x00, 0x00, 0x01 };
	static const u8 irq_wire[] = { 3, 0x03, 3, 0, 0x05, 0x00, 0x01 };
	static const u8 irq[] = { 0x05, 0x30 };
	static const u8 two[] = { 0x05, 0x30, 0x06, 0x30 };
	static const u8 get_reply[] = { 3, 0x80, 2, 0, 0x01, 0x00 };
	static const u8 get_want[] = { 0x00, 0x10, 0x05, 0x00 };
	static const u8 get[] = { 0x00, 0x00, 0x05, 0x00 };
	static const u8 set[] = { 0x00, 0x10 };
	struct lt_fake *f = lt_create(test, 64, 3);
	const struct obmf_channel_quirks *q;
	u8 req[64], resp[64], m[64];
	struct obmf_channel *ch;
	size_t len;
	int i;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[3];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	q = &ch->info.quirks;

	lt_set_reply(f, get_reply, sizeof(get_reply));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x00, get, sizeof(get), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(get_wire));
	KUNIT_EXPECT_MEMEQ(test, f->sent, get_wire, sizeof(get_wire));
	KUNIT_ASSERT_EQ(test, len, sizeof(get_want));
	KUNIT_EXPECT_MEMEQ(test, resp, get_want, sizeof(get_want));

	obmf_lstp_hdr(m, 3, 0x80, 0);
	lt_set_reply(f, m, OBMF_HDR_LEN);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, set, sizeof(set), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_EXPECT_MEMEQ(test, f->sent, set_wire, sizeof(set_wire));
	KUNIT_EXPECT_EQ(test, len, 0);

	/* a full Get: line k has index 2k and state k & 1 */
	for (i = 0; i < q->gpio_per_get; i++) {
		put_unaligned_le16(2 * i, req + 2 * i);
		m[OBMF_HDR_LEN + i] = i & 1;
	}
	obmf_lstp_hdr(m, 3, 0x80, q->gpio_per_get);
	lt_set_reply(f, m, OBMF_HDR_LEN + q->gpio_per_get);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x00, req, 2 * q->gpio_per_get,
					resp, sizeof(resp), &len, 0),
			0);
	KUNIT_EXPECT_EQ(test, f->sent_len, 64);
	KUNIT_ASSERT_EQ(test, len, 2 * q->gpio_per_get);
	for (i = 0; i < q->gpio_per_get; i++)
		if (get_unaligned_le16(resp + 2 * i) !=
		    (2 * i | (i & 1) << 12)) {
			KUNIT_FAIL(test, "line %d: %#x", i,
				   get_unaligned_le16(resp + 2 * i));
			break;
		}
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x00, req, 2 * q->gpio_per_get + 2,
					resp, sizeof(resp), &len, 0),
			-EMSGSIZE);

	/* a full Set, and one line more */
	memset(req, 0, sizeof(req));
	obmf_lstp_hdr(m, 3, 0x80, 0);
	lt_set_reply(f, m, OBMF_HDR_LEN);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, req, 2 * q->gpio_per_set,
					resp, sizeof(resp), &len, 0),
			0);
	KUNIT_EXPECT_EQ(test, f->sent_len, 64);
	i = f->sends;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x01, req, 2 * q->gpio_per_set + 2,
					resp, sizeof(resp), &len, 0),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test, f->sends, i);

	/* one state for a Get of two lines, and an error status */
	m[0] = 3;
	m[1] = 0x80;
	put_unaligned_le16(1, m + 2);
	m[4] = 1;
	lt_set_reply(f, m, OBMF_HDR_LEN + 1);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x00, get, sizeof(get), resp,
					sizeof(resp), &len, 0),
			-EPROTO);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_PROTOCOL_ERRORS]), 1);
	obmf_lstp_hdr(m, 3, 0x80 | OBMF_LSTP_NOT_SUPP, 0);
	lt_set_reply(f, m, OBMF_HDR_LEN);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x00, get, sizeof(get), resp,
					sizeof(resp), &len, 0),
			-EOPNOTSUPP);

	/* Set Interrupt Config of line 5, rising */
	obmf_lstp_hdr(m, 3, 0x80, 0);
	lt_set_reply(f, m, OBMF_HDR_LEN);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x03, irq, sizeof(irq), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(irq_wire));
	KUNIT_EXPECT_MEMEQ(test, f->sent, irq_wire, sizeof(irq_wire));
	KUNIT_EXPECT_EQ(test, len, 0);

	i = f->sends;
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x02, get, 2, resp, sizeof(resp),
					&len, 0),
			-EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x03, two, sizeof(two), resp,
					sizeof(resp), &len, 0),
			-EMSGSIZE);
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x06, get, 2, resp, sizeof(resp),
					&len, 0),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, f->sends, i);
}

/*
 * With no driver the UART device's write is refused with a NACK; an
 * unknown command is malformed and answered NOT_SUPP, so the device does
 * not send it again. A malformed IPMI request is owed an answer, which
 * LSTP has no form for, so none is sent; a malformed I2C device request
 * is owed none.
 */
static void lt_default_answer(struct kunit *test)
{
	static const u8 write[] = { 2, 0x00, 2, 0, 'h', 'i' };
	static const u8 unknown[] = { 2, 0x01, 0, 0 };
	static const u8 i2c[] = { 1, 0x00, 0, 0 };
	static const u8 ipmi[] = { 3, 0x00, 0, 0 };
	static const u8 nack[] = { 2, 0x84, 0, 0 };
	static const u8 not_supp[] = { 2, 0x86, 0, 0 };
	struct lt_fake *f = lt_create(test, 64, 3);
	struct obmf_core *core;
	struct obmf_channel *ch;

	lt_blob(f, 3, OBMF_LSTP_TYPE_IPMI, "ipmi", lt_i2c_cfg, 0);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	core = f->core;
	ch = core->chans[2];
	KUNIT_ASSERT_NULL(test, ch->fdev->adev.dev.driver);

	obmf_core_rx(core, write, sizeof(write));
	flush_workqueue(core->wq);
	KUNIT_EXPECT_EQ(test, f->sends, 1);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(nack));
	KUNIT_EXPECT_MEMEQ(test, f->sent, nack, sizeof(nack));
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_DEFAULT_ANSWERS]), 1);

	f->sent_len = 0;
	obmf_core_rx(core, unknown, sizeof(unknown));
	flush_workqueue(core->wq);
	KUNIT_EXPECT_EQ(test, f->sends, 2);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(not_supp));
	KUNIT_EXPECT_MEMEQ(test, f->sent, not_supp, sizeof(not_supp));
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&core->cnt[OBMF_IF_RX_MALFORMED]), 1);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_DEFAULT_ANSWERS]), 2);

	ch = core->chans[1];
	obmf_core_rx(core, i2c, sizeof(i2c));
	flush_workqueue(core->wq);
	KUNIT_EXPECT_EQ(test, f->sends, 2);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&core->cnt[OBMF_IF_RX_MALFORMED]), 2);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_DEFAULT_ANSWERS]), 0);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_RX_DROPPED]),
			0);

	ch = core->chans[3];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	obmf_core_rx(core, ipmi, sizeof(ipmi));
	flush_workqueue(core->wq);
	KUNIT_EXPECT_EQ(test, f->sends, 2);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&core->cnt[OBMF_IF_RX_MALFORMED]), 3);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_DEFAULT_ANSWERS]), 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_RX_DROPPED]),
			1);
}

/*
 * Channel 0 takes device requests before publish; an empty one
 * is counted, not reported, while discovering
 */
static void lt_rx_before_publish(struct kunit *test)
{
	static const u8 empty[] = { 0, 0x00, 0, 0 };
	struct lt_fake *f = lt_create(test, 64, 1);
	struct obmf_core *core;
	int ret;

	KUNIT_ASSERT_EQ(test, obmf_dialect_attach(&f->tp, &obmf_lstp_usb_proto),
			0);
	f->core = obmf_core_create(&f->tp);
	KUNIT_ASSERT_NOT_NULL(test, f->core);
	ret = kunit_add_action_or_reset(test, lt_unbind_action, f);
	KUNIT_ASSERT_EQ(test, ret, 0);
	core = f->core;
	obmf_core_rx(core, empty, sizeof(empty));
	flush_workqueue(core->wq);
	ret = atomic_long_read(&core->ch0->cnt[OBMF_CH_RX_DEVICE_REQUESTS]);
	KUNIT_EXPECT_EQ(test, ret, 1);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&core->cnt[OBMF_IF_RX_MALFORMED]), 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&core->events[OBMF_EV_BAD_DREQ]),
			0);
}

/* The serial driver on an LSTP device; only when built in */
#if IS_BUILTIN(CONFIG_OBMF_TTY)

/*
 * Table 23: the serial driver refuses the UART device's write while no one
 * has the port open, with the NACK status, and no default answer is sent
 */
static void lt_uart_closed(struct kunit *test)
{
	static const u8 write[] = { 2, 0x00, 2, 0, 'h', 'i' };
	static const u8 nack[] = { 2, 0x84, 0, 0 };
	struct lt_fake *f = lt_create(test, 64, 2);
	struct obmf_channel *ch;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[2];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev->adev.dev.driver);

	obmf_core_rx(f->core, write, sizeof(write));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, f->sends, 1);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(nack));
	KUNIT_EXPECT_MEMEQ(test, f->sent, nack, sizeof(nack));
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_DEFAULT_ANSWERS]), 0);
}

static void lt_tty_close(void *data)
{
	struct tty_struct *tty = data;

	tty_lock(tty);
	tty->ops->close(tty, NULL);
	tty_unlock(tty);
	tty_kclose(tty);
}

/* The tty of the UART channel's function device, opened in the kernel */
static struct tty_struct *lt_tty_open(struct kunit *test,
				      struct obmf_channel *ch)
{
	struct tty_struct *tty;
	struct device *dev;
	dev_t devt;
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	dev = device_find_any_child(&ch->fdev->adev.dev);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	devt = dev->devt;
	put_device(dev);
	tty = tty_kopen_exclusive(devt);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, tty);
	ret = tty->ops->open(tty, NULL);
	tty_unlock(tty);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, lt_tty_close, tty), 0);
	KUNIT_ASSERT_EQ(test, ret, 0);
	return tty;
}

/* Keeps, in test->priv, the last queue-full report of the case's thread */
static void lt_qf_stub(struct device *dev, enum obmf_event ev, int ch, long a,
		       long b, struct ratelimit_state *rs, const char *func)
{
	long *refused = kunit_get_current_test()->priv;

	if (ev == OBMF_EV_QUEUE_FULL)
		*refused = a;
}

/*
 * UART writes sent while the first waits for the serial driver
 * take the reserve and the queue, and the open port takes them; the next
 * is refused with the NACK, counted in rx_dropped and reported as
 * refused, and every write is answered once
 */
static void lt_uart_queue_full(struct kunit *test)
{
	static const u8 write[] = { 2, 0x00, 2, 0, 'h', 'i' };
	static const u8 nack[] = { 2, 0x84, 0, 0 };
	int n = OBMF_DREQ_RESERVE + OBMF_DREQ_QUEUE_MAX + 1, i;
	struct lt_fake *f = lt_create(test, 64, 2);
	struct obmf_channel *ch;
	struct tty_struct *tty;
	long refused = -1;
	struct ktermios t;
	int sends;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[2];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	tty = lt_tty_open(test, ch);
	/* No echo, so the line discipline sends nothing */
	down_read(&tty->termios_rwsem);
	t = tty->termios;
	up_read(&tty->termios_rwsem);
	t.c_lflag &= ~ECHO;
	tty_set_termios(tty, &t);
	sends = f->sends;
	test->priv = &refused;
	kunit_activate_static_stub(test, obmf_report_log, lt_qf_stub);

	/* Expectations only while the lock is held */
	mutex_lock(&ch->dispatch_lock);
	for (i = 0; i < n; i++)
		obmf_core_rx(f->core, write, sizeof(write));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_RX_DROPPED]),
			1);
	mutex_unlock(&ch->dispatch_lock);
	kunit_deactivate_static_stub(test, obmf_report_log);
	test->priv = NULL;
	flush_workqueue(f->core->wq);

	KUNIT_EXPECT_EQ(test, refused, 1);
	KUNIT_EXPECT_EQ(test, f->sends - sends, n);
	/* The queued writes are taken; the default answer goes last */
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(nack));
	KUNIT_EXPECT_MEMEQ(test, f->sent, nack, sizeof(nack));
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&ch->cnt[OBMF_CH_DEFAULT_ANSWERS]), 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_RX_DROPPED]),
			1);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&f->core->events[OBMF_EV_QUEUE_FULL]),
			1);
}

/* Waits up to @ms for @tx characters sent in all and none left */
static void lt_tty_wait(struct kunit *test, struct tty_struct *tty, u32 tx,
			unsigned int ms)
{
	struct serial_icounter_struct ic;
	unsigned int t;

	for (t = 0; t <= ms / 20; t++) {
		memset(&ic, 0, sizeof(ic));
		KUNIT_ASSERT_EQ(test, tty->ops->get_icount(tty, &ic), 0);
		if (ic.tx == tx && !tty->ops->chars_in_buffer(tty))
			break;
		msleep(20);
	}
	KUNIT_EXPECT_EQ(test, ic.tx, tx);
	KUNIT_EXPECT_EQ(test, tty->ops->chars_in_buffer(tty), 0);
}

/*
 * A host write the device NACKs is sent again by the serial
 * driver until the device takes it, here after more NACKs than the
 * driver once allowed; so is one answered with the timeout or the busy
 * status. None of them makes the driver wait longer for an answer.
 */
static void lt_uart_refused(struct kunit *test)
{
	static const struct {
		u8 status;
		int tries, sends;
		u32 tx;
	} st[] = {
		{ OBMF_LSTP_NACK, 25, 26, 2 },
		{ 0x02, 3, 4, 4 },
		{ 0x03, 3, 4, 6 },
	};
	static const u8 hi[] = { 'h', 'i' };
	struct lt_fake *f = lt_create(test, 64, 2);
	unsigned int wait = 0;
	struct tty_struct *tty;
	struct obmf_channel *ch;
	int i, sends;
	u8 m[4];

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[2];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	tty = lt_tty_open(test, ch);
	obmf_lstp_hdr(m, 2, 0x80, 0);
	lt_set_reply(f, m, sizeof(m));
	for (i = 0; i < ARRAY_SIZE(st); i++) {
		sends = READ_ONCE(f->sends);
		WRITE_ONCE(f->uart_status, st[i].status);
		WRITE_ONCE(f->uart_left, st[i].tries);
		KUNIT_ASSERT_EQ(test, tty->ops->write(tty, hi, sizeof(hi)),
				sizeof(hi));
		lt_tty_wait(test, tty, st[i].tx, 2000);
		msleep(50);
		KUNIT_EXPECT_EQ_MSG(test, READ_ONCE(f->sends) - sends,
				    st[i].sends, "status %#x", st[i].status);
		KUNIT_EXPECT_EQ_MSG(test, f->sent_len,
				    OBMF_HDR_LEN + sizeof(hi), "status %#x",
				    st[i].status);
		KUNIT_EXPECT_MEMEQ_MSG(test, f->sent + OBMF_HDR_LEN, hi,
				       sizeof(hi), "status %#x", st[i].status);
		if (!i)
			wait = f->sent_timeout_ms;
		KUNIT_EXPECT_EQ_MSG(test, f->sent_timeout_ms, wait,
				    "status %#x", st[i].status);
	}
	KUNIT_EXPECT_GE(test, wait, OBMF_DEFAULT_TIMEOUT_MS);
	KUNIT_EXPECT_LT(test, wait, 2 * OBMF_DEFAULT_TIMEOUT_MS);
}

#endif /* IS_BUILTIN(CONFIG_OBMF_TTY) */

/* After a wire transfer on its slot, a request's reply is decoded again */
static void lt_wire_then_request(struct kunit *test)
{
	static const u8 reply[] = { 1, 0x80, 2, 0, 0x5a, 0xa5 };
	static const u8 resp_want[] = { 2, 0, 0, 0x5a, 0xa5 };
	static const u8 req[] = { 0x50, 2, 0, 0x10 };
	struct lt_fake *f = lt_create(test, 64, 1);
	struct obmf_channel *ch;
	u8 resp[16], m[4];
	size_t len;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[1];
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	obmf_lstp_hdr(m, 1, 0x80, 0);
	lt_set_reply(f, m, sizeof(m));
	KUNIT_ASSERT_EQ(test,
			obmf_ch_control_update(ch->fdev,
					       OBMF_I2C_TRIGGER_RECOVERY,
					       OBMF_I2C_TRIGGER_RECOVERY),
			0);

	lt_set_reply(f, reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_request(ch, 0x03, req, sizeof(req), resp,
					sizeof(resp), &len, 0),
			0);
	KUNIT_ASSERT_EQ(test, len, sizeof(resp_want));
	KUNIT_EXPECT_MEMEQ(test, resp, resp_want, sizeof(resp_want));
}

/* A response with no request waiting is counted late, not delivered */
static void lt_late_reply(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 1);
	struct obmf_channel *ch;
	u8 m[4];

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[1];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	obmf_lstp_hdr(m, 1, 0x80, 0);
	obmf_core_rx(f->core, m, sizeof(m));
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_RX_LATE]), 1);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_RX_RESPONSES]),
			0);
}

/* Recovery through the channel API: BUS_RECOVERY, then its result */
static void lt_i2c_recovery(struct kunit *test)
{
	static const u8 wire[] = { 1, OBMF_LSTP_I2C_BUS_RECOVERY, 0, 0 };
	static const u8 body[] = { 1, 0x80, 1, 0, 0xaa };
	struct lt_fake *f = lt_create(test, 64, 1);
	struct obmf_channel *ch;
	u8 status, specific, m[4];
	int sends;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[1];
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);

	obmf_lstp_hdr(m, 1, 0x80, 0);
	lt_set_reply(f, m, sizeof(m));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_control_update(ch->fdev,
					       OBMF_I2C_TRIGGER_RECOVERY,
					       OBMF_I2C_TRIGGER_RECOVERY),
			0);
	KUNIT_EXPECT_MEMEQ(test, f->sent, wire, sizeof(wire));
	KUNIT_EXPECT_EQ(test, obmf_ch_status_read(ch->fdev, &status, &specific),
			0);
	KUNIT_EXPECT_EQ(test, status, 0x21);
	KUNIT_EXPECT_EQ(test, specific, OBMF_I2C_RECOVERY_SUCCESS);

	KUNIT_EXPECT_EQ(test,
			obmf_ch_control_update(ch->fdev, OBMF_I2C_CLEAR_STATUS,
					       OBMF_I2C_CLEAR_STATUS),
			0);
	obmf_lstp_hdr(m, 1, 0x80 | 0x01, 0);
	lt_set_reply(f, m, sizeof(m));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_control_update(ch->fdev,
					       OBMF_I2C_TRIGGER_RECOVERY,
					       OBMF_I2C_TRIGGER_RECOVERY),
			0);
	KUNIT_EXPECT_EQ(test, obmf_ch_status_read(ch->fdev, &status, &specific),
			0);
	KUNIT_EXPECT_EQ(test, specific, OBMF_I2C_RECOVERY_FAILED);

	/* A mask of 0 changes nothing and sends nothing */
	sends = f->sends;
	KUNIT_EXPECT_EQ(test, obmf_ch_control_update(ch->fdev, 0, 0xff), 0);
	KUNIT_EXPECT_EQ(test, f->sends, sends);
	KUNIT_EXPECT_EQ(test, obmf_ch_status_read(ch->fdev, &status, &specific),
			0);
	KUNIT_EXPECT_EQ(test, specific, OBMF_I2C_RECOVERY_FAILED);

	/* BUS_RECOVERY's reply has no body */
	lt_set_reply(f, body, sizeof(body));
	KUNIT_EXPECT_EQ(test,
			obmf_ch_control_update(ch->fdev,
					       OBMF_I2C_TRIGGER_RECOVERY,
					       OBMF_I2C_TRIGGER_RECOVERY),
			-EMSGSIZE);
}

/* An OBMF-ICP I2C request of @len bytes, header included, to 0x50 */
static void lt_i2c_req(u8 *buf, u8 cmd, u16 rlen, size_t len)
{
	buf[0] = 1;
	buf[1] = 0;
	put_unaligned_le16(len - OBMF_HDR_LEN, buf + 2);
	buf[4] = cmd;
	buf[5] = 0x50;
	put_unaligned_le16(rlen, buf + 6);
}

/*
 * At 64-byte packets, a write-read and a write whose write part is the
 * adapter's limit (max_req less the command and its three bytes) both
 * fit the link; a write-read one byte longer does not
 */
static void lt_i2c_write_limit(struct kunit *test)
{
	static const u8 cmds[] = { 0x03, 0x01 };
	static const ssize_t wire[] = { 64, 62 };
	struct lt_fake *f = lt_create(test, 64, 1);
	struct obmf_channel *ch;
	size_t len, cap;
	u8 buf[LT_MSG];
	int i;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[1];
	KUNIT_ASSERT_EQ(test, ch->info.type, OBMF_TYPE_I2C_CONTROLLER);
	cap = f->core->tp->max_tx_msg;
	len = OBMF_HDR_LEN + ch->info.max_req;
	KUNIT_ASSERT_LT(test, len, sizeof(buf));
	for (i = 0; i < ARRAY_SIZE(cmds); i++) {
		memset(buf, 0xa5, sizeof(buf));
		lt_i2c_req(buf, cmds[i], i ? 0 : 1, len);
		KUNIT_EXPECT_EQ_MSG(test, obmf_lstp_encode(ch, buf, len, cap),
				    wire[i], "command %#x", cmds[i]);
	}
	lt_i2c_req(buf, 0x03, 1, len + 1);
	KUNIT_EXPECT_EQ(test, obmf_lstp_encode(ch, buf, len + 1, cap),
			-EMSGSIZE);
}

/* The I2C controller driver on an LSTP device; only when built in */
#if IS_BUILTIN(CONFIG_I2C_OBMF)

#define LT_I2C_ADDR 0x50
#define LT_I2C_CMD 0x10

KUNIT_DEFINE_ACTION_WRAPPER(lt_put_device, put_device, struct device *);

/* The adapter the I2C driver registered on channel 1 */
static struct i2c_adapter *lt_adapter(struct lt_fake *f)
{
	struct kunit *test = f->test;
	struct obmf_channel *ch = f->core->chans[1];
	struct i2c_adapter *adap;
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	dev = device_find_any_child(&ch->fdev->adev.dev);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, lt_put_device, dev), 0);
	adap = i2c_verify_adapter(dev);
	KUNIT_ASSERT_NOT_NULL(test, adap);
	return adap;
}

/* The PEC of a byte read of LT_I2C_CMD at LT_I2C_ADDR returning @val */
static u8 lt_read_pec(u8 val)
{
	u8 b[] = { LT_I2C_ADDR << 1, LT_I2C_CMD, LT_I2C_ADDR << 1 | 1, val };

	return i2c_smbus_pec(0, b, sizeof(b));
}

/*
 * An SMBus byte read and write of a client with PEC: the LSTP Write Read
 * reads the PEC byte last, the LSTP Write writes it last; a wrong PEC
 * from the device is -EBADMSG from the I2C core
 */
static void lt_i2c_pec(struct kunit *test)
{
	static const u8 rd[] = { 1, 0x04, 4, 0, LT_I2C_ADDR, 2, 0, LT_I2C_CMD };
	u8 reply[] = { 1, 0x80, 2, 0, 0x5a, lt_read_pec(0x5a) };
	u8 wr[] = { 1, 0x02, 4, 0, LT_I2C_ADDR, LT_I2C_CMD, 0x77, 0 };
	u8 pw[] = { LT_I2C_ADDR << 1, LT_I2C_CMD, 0x77 };
	struct lt_fake *f = lt_create(test, 64, 1);
	union i2c_smbus_data data;
	struct i2c_adapter *adap;
	u8 ack[4];

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	adap = lt_adapter(f);
	lt_set_reply(f, reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test,
			i2c_smbus_xfer(adap, LT_I2C_ADDR, I2C_CLIENT_PEC,
				       I2C_SMBUS_READ, LT_I2C_CMD,
				       I2C_SMBUS_BYTE_DATA, &data),
			0);
	KUNIT_EXPECT_EQ(test, data.byte, 0x5a);
	KUNIT_ASSERT_EQ(test, f->sent_len, sizeof(rd));
	KUNIT_EXPECT_MEMEQ(test, f->sent, rd, sizeof(rd));

	reply[5] ^= 0x01;
	lt_set_reply(f, reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test,
			i2c_smbus_xfer(adap, LT_I2C_ADDR, I2C_CLIENT_PEC,
				       I2C_SMBUS_READ, LT_I2C_CMD,
				       I2C_SMBUS_BYTE_DATA, &data),
			-EBADMSG);

	obmf_lstp_hdr(ack, 1, 0x80, 0);
	lt_set_reply(f, ack, sizeof(ack));
	data.byte = 0x77;
	KUNIT_EXPECT_EQ(test,
			i2c_smbus_xfer(adap, LT_I2C_ADDR, I2C_CLIENT_PEC,
				       I2C_SMBUS_WRITE, LT_I2C_CMD,
				       I2C_SMBUS_BYTE_DATA, &data),
			0);
	wr[7] = i2c_smbus_pec(0, pw, sizeof(pw));
	KUNIT_ASSERT_EQ(test, f->sent_len, sizeof(wr));
	KUNIT_EXPECT_MEMEQ(test, f->sent, wr, sizeof(wr));
	KUNIT_EXPECT_EQ(test, f->sends, 3);
}

/* An SMBus byte read of LT_I2C_CMD at LT_I2C_ADDR, without PEC */
static int lt_read_byte(struct i2c_adapter *adap, union i2c_smbus_data *data)
{
	return i2c_smbus_xfer(adap, LT_I2C_ADDR, 0, I2C_SMBUS_READ, LT_I2C_CMD,
			      I2C_SMBUS_BYTE_DATA, data);
}

/* A recovery under the bus lock, which the transfer path holds */
static int lt_recover(struct i2c_adapter *adap)
{
	int ret;

	i2c_lock_bus(adap, I2C_LOCK_SEGMENT);
	ret = i2c_recover_bus(adap);
	i2c_unlock_bus(adap, I2C_LOCK_SEGMENT);
	return ret;
}

/*
 * The adapter has no bus recovery, though LSTP offers BUS_RECOVERY, and
 * i2c_recover_bus() gives -EBUSY without a request. LSTP_BUSY,
 * LSTP_TIMEOUT or no response fails the transfer, and nothing else is
 * sent.
 */
static void lt_i2c_no_recovery(struct kunit *test)
{
	static const struct {
		u8 status;
		int err;
	} st[] = {
		{ 0x03, -EBUSY },
		{ 0x02, -ETIMEDOUT },
	};
	struct lt_fake *f = lt_create(test, 64, 1);
	union i2c_smbus_data data;
	struct i2c_adapter *adap;
	u8 m[4];
	int i;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	adap = lt_adapter(f);
	KUNIT_EXPECT_NULL(test, adap->bus_recovery_info);
	KUNIT_EXPECT_EQ(test, lt_recover(adap), -EBUSY);
	KUNIT_EXPECT_EQ(test, f->sends, 0);
	for (i = 0; i < ARRAY_SIZE(st); i++) {
		obmf_lstp_hdr(m, 1, 0x80 | st[i].status, 0);
		lt_set_reply(f, m, sizeof(m));
		KUNIT_EXPECT_EQ_MSG(test, lt_read_byte(adap, &data), st[i].err,
				    "status %#x", st[i].status);
		KUNIT_EXPECT_EQ_MSG(test, f->sends, i + 1, "status %#x",
				    st[i].status);
	}

	f->reply_len = 0;
	KUNIT_EXPECT_EQ(test, lt_read_byte(adap, &data), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->sends, ARRAY_SIZE(st) + 1);
}

/* A device reply delivered from a work item, after a delay */
struct lt_late {
	struct delayed_work work;
	struct lt_fake *f;
	u8 msg[8];
	size_t len;
};

static void lt_late_work(struct work_struct *w)
{
	struct lt_late *l =
		container_of(to_delayed_work(w), struct lt_late, work);

	obmf_core_rx(l->f->core, l->msg, l->len);
}

KUNIT_DEFINE_ACTION_WRAPPER(lt_cancel_late, cancel_delayed_work_sync,
			    struct delayed_work *);

/*
 * No response, and the reply comes late, while the next transfer waits:
 * the next transfer is sent only after the late reply, which is dropped,
 * so it times out in turn
 */
static void lt_i2c_late_reply(struct kunit *test)
{
	static const u8 reply[] = { 1, 0x80, 1, 0, 0x5a };
	struct lt_fake *f = lt_create(test, 64, 1);
	union i2c_smbus_data data = {};
	struct i2c_adapter *adap;
	struct obmf_channel *ch;
	struct lt_late *l;
	ktime_t t;
	s64 ms;
	int ret;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	adap = lt_adapter(f);
	ch = f->core->chans[1];
	l = kunit_kzalloc(test, sizeof(*l), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, l);
	l->f = f;
	memcpy(l->msg, reply, sizeof(reply));
	l->len = sizeof(reply);
	INIT_DELAYED_WORK(&l->work, lt_late_work);
	ret = kunit_add_action_or_reset(test, lt_cancel_late, &l->work);
	KUNIT_ASSERT_EQ(test, ret, 0);

	queue_delayed_work(system_dfl_wq, &l->work,
			   msecs_to_jiffies(OBMF_DEFAULT_TIMEOUT_MS + 500));
	t = ktime_get();
	KUNIT_EXPECT_EQ(test, lt_read_byte(adap, &data), -ETIMEDOUT);
	KUNIT_EXPECT_EQ(test, f->sends, 1);
	KUNIT_EXPECT_EQ(test, lt_read_byte(adap, &data), -ETIMEDOUT);
	ms = ktime_ms_delta(ktime_get(), t);
	KUNIT_EXPECT_GE(test, ms,
			2 * OBMF_DEFAULT_TIMEOUT_MS + 500 -
				jiffies_to_msecs(2));
	/* the late reply, not the end of the hold, sent the second */
	KUNIT_EXPECT_LT(test, ms, 2 * OBMF_DEFAULT_TIMEOUT_MS + 900);
	KUNIT_EXPECT_EQ(test, f->sends, 2);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_TIMEOUTS]), 2);
	KUNIT_EXPECT_EQ(test, atomic_long_read(&ch->cnt[OBMF_CH_RX_LATE]), 1);
}

#endif /* IS_BUILTIN(CONFIG_I2C_OBMF) */

/*
 * The line of a bound device with I2C on channel 2, GPIO on channel 3 and
 * a UART on channel 5: the LSTP version, the channel 0 name and each kept
 * channel, the GPIO one marked as it gets no function device; a type not
 * kept and a channel that fails its read are left out
 */
static void lt_info_line(struct kunit *test)
{
	static const u8 other[2];
	struct lt_fake *f = lt_create(test, 512, 5);
	u8 mgmt[2] = { OBMF_LSTP_VERSION, 5 };
	char buf[OBMF_INFO_LEN];

	lt_blob(f, 0, OBMF_LSTP_TYPE_MGMT, "MCU", mgmt, sizeof(mgmt));
	lt_blob(f, 1, 0x06, "other", other, sizeof(other));
	lt_blob(f, 2, OBMF_LSTP_TYPE_I2C, "I2C", lt_i2c_cfg,
		sizeof(lt_i2c_cfg));
	lt_blob(f, 5, OBMF_LSTP_TYPE_UART, "UART", lt_uart_cfg,
		sizeof(lt_uart_cfg));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[3]);
	KUNIT_EXPECT_NULL(test, f->core->chans[4]);
	obmf_core_info(f->core, buf);
	KUNIT_EXPECT_STREQ(test, buf,
			   "LSTP 1 device 'MCU': I2C ch2 'I2C', GPIO ch3 'gpio', serial ch5 'UART'");
}

/* The SPI controller driver on an LSTP device; only when built in */
#if IS_BUILTIN(CONFIG_SPI_OBMF)

KUNIT_DEFINE_ACTION_WRAPPER(lt_spi_put, put_device, struct device *);
KUNIT_DEFINE_ACTION_WRAPPER(lt_spi_unregister, spi_unregister_device,
			    struct spi_device *);

/*
 * A flash's JEDEC ID read on CS1 of two: one WRITE_READ that asserts and
 * releases CS1, the ID back in the same transfer
 */
static void lt_spi_rdid(struct kunit *test)
{
	static const u8 cfg[5] = { 2, 0x40, 0x42, 0x0f, 0x00 };
	static const u8 wire[] = { 1, 0x73, 4, 0, 0x9f, 0, 0, 0 };
	static const u8 reply[] = { 1, 0x80, 4, 0, 0xff, 0xef, 0x40, 0x21 };
	struct lt_fake *f = lt_create(test, 64, 1);
	u8 tx[4] = { 0x9f }, rx[4] = {};
	struct spi_transfer t = { .tx_buf = tx, .rx_buf = rx, .len = 4 };
	struct spi_board_info info = {
		.modalias = "obmf-kunit-spi",
		.chip_select = 1,
	};
	struct spi_controller *ctlr;
	struct obmf_channel *ch;
	struct spi_device *spi;
	struct device *dev;

	lt_blob(f, 1, OBMF_LSTP_TYPE_SPI, "flash", cfg, sizeof(cfg));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	ch = f->core->chans[1];
	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	dev = device_find_any_child(&ch->fdev->adev.dev);
	KUNIT_ASSERT_NOT_NULL(test, dev);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, lt_spi_put, dev),
			0);
	ctlr = container_of(dev, struct spi_controller, dev);
	KUNIT_EXPECT_EQ(test, ctlr->num_chipselect, 2);
	KUNIT_EXPECT_EQ(test, ctlr->max_speed_hz, 1000000);
	KUNIT_EXPECT_EQ(test, ctlr->mode_bits, 0);

	spi = spi_new_device(ctlr, &info);
	KUNIT_ASSERT_NOT_NULL_MSG(test, spi, "client not added");
	KUNIT_ASSERT_EQ(test,
			kunit_add_action_or_reset(test, lt_spi_unregister, spi),
			0);

	lt_set_reply(f, reply, sizeof(reply));
	KUNIT_EXPECT_EQ(test, spi_sync_transfer(spi, &t, 1), 0);
	KUNIT_EXPECT_EQ(test, f->sent_len, sizeof(wire));
	KUNIT_EXPECT_MEMEQ(test, f->sent, wire, sizeof(wire));
	KUNIT_EXPECT_MEMEQ(test, rx, reply + 4, 4);
}

#endif /* IS_BUILTIN(CONFIG_SPI_OBMF) */

/* The GPIO driver on an LSTP device; only when built in */
#if IS_BUILTIN(CONFIG_GPIO_OBMF)

static int lt_gpio_match(struct gpio_chip *gc, const void *data)
{
	return gc->parent == data;
}

KUNIT_DEFINE_ACTION_WRAPPER(lt_put_gdev, gpio_device_put,
			    struct gpio_device *);
KUNIT_DEFINE_ACTION_WRAPPER(lt_free_desc, gpiochip_free_own_desc,
			    struct gpio_desc *);

/* The chip the GPIO driver registered on channel @nr, or NULL */
static struct gpio_chip *lt_gpio_chip(struct lt_fake *f, int nr)
{
	struct kunit *test = f->test;
	struct obmf_channel *ch = f->core->chans[nr];
	struct gpio_device *gdev;

	KUNIT_ASSERT_NOT_NULL(test, ch);
	KUNIT_ASSERT_NOT_NULL(test, ch->fdev);
	gdev = gpio_device_find(&ch->fdev->adev.dev, lt_gpio_match);
	if (!gdev)
		return NULL;
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, lt_put_gdev, gdev),
			0);
	return gpio_device_get_chip(gdev);
}

static struct gpio_desc *lt_gpio_desc(struct kunit *test, struct gpio_chip *gc,
				      unsigned int hw)
{
	struct gpio_desc *d;

	d = gpiochip_request_own_desc(gc, hw, "kunit",
				      GPIO_LOOKUP_FLAGS_DEFAULT, GPIOD_ASIS);
	KUNIT_ASSERT_FALSE(test, IS_ERR(d));
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, lt_free_desc, d),
			0);
	return d;
}

/*
 * gpiod_set_value_cansleep() and the chip's set_multiple() with their
 * result. Before v6.17 they return void, and a case that checks the
 * result is skipped.
 */
static int lt_gpio_set_value(struct kunit *test, struct gpio_desc *desc,
			     int value)
{
#ifdef OBMF_COMPAT_GPIO_SET_VOID
	kunit_skip(test, "gpiod_set_value() returns void on this kernel");
	return 0;
#else
	return gpiod_set_value_cansleep(desc, value);
#endif
}

static int lt_gpio_set_multiple(struct kunit *test, struct gpio_chip *gc,
				unsigned long *mask, unsigned long *bits)
{
#ifdef OBMF_COMPAT_GPIO_SET_VOID
	kunit_skip(test, "set_multiple() returns void on this kernel");
	return 0;
#else
	return gc->set_multiple(gc, mask, bits);
#endif
}

/*
 * An LSTP output reads back the value last set, from its default output
 * on (Z on an open drain is high), whatever level the pin reports: nothing
 * is sent at probe or for the Get of an output. SET_VALUE sets it.
 */
static void lt_gpio_chip_io(struct kunit *test)
{
	static const u8 set_wire[] = { 3, 0x01, 3, 0, 0x00, 0x00, 0x00 };
	struct lt_fake *f = lt_create(test, 64, 3);
	struct gpio_desc *reset;
	struct gpio_chip *gc;

	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 0);
	gc = lt_gpio_chip(f, 3);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_EXPECT_EQ(test, gc->ngpio, 1);
	KUNIT_EXPECT_STREQ(test, gc->names[0], "reset");
	reset = lt_gpio_desc(test, gc, 0);

	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(reset), 1);
	KUNIT_EXPECT_EQ(test, lt_gpio_set_value(test, reset, 0), 0);
	KUNIT_EXPECT_EQ(test, f->gpio_sent_len, sizeof(set_wire));
	KUNIT_EXPECT_MEMEQ(test, f->gpio_sent, set_wire, sizeof(set_wire));
	KUNIT_EXPECT_FALSE(test, test_bit(0, f->gpio_level));
	__set_bit(0, f->gpio_level);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(reset), 0);
	KUNIT_EXPECT_EQ(test, lt_gpio_set_value(test, reset, 1), 0);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(reset), 1);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 2);
	KUNIT_EXPECT_EQ(test, f->sends, 0);
}

/*
 * A device that refuses GET_VALUE on an output (NOT_SUPP)
 * gets its chip. The output reads its default output, then the value last
 * set; a SET_VALUE that fails, though the device set the line, reads
 * nothing back and keeps the value last set.
 */
static void lt_gpio_out_shadow(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 3);
	struct gpio_desc *reset;
	struct gpio_chip *gc;

	__set_bit(0, f->gpio_wo);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	gc = lt_gpio_chip(f, 3);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	reset = lt_gpio_desc(test, gc, 0);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(reset), 1);
	KUNIT_EXPECT_EQ(test, lt_gpio_set_value(test, reset, 0), 0);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(reset), 0);

	f->gpio_status = OBMF_LSTP_NACK;
	f->gpio_prefix = 1;
	KUNIT_EXPECT_EQ(test, lt_gpio_set_value(test, reset, 1), -EIO);
	KUNIT_EXPECT_TRUE(test, test_bit(0, f->gpio_level));
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(reset), 0);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 2);
}

/* A GPIO channel whose lines are all LEDs gets no chip and sends nothing */
static void lt_gpio_leds_only(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 3);
	u8 gpio[50] = { 1 };

	lt_gpio(gpio + 1, "led", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_LOW, 0, 0,
		OBMF_LSTP_GPIO_LED_HOLD);
	lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio", gpio, sizeof(gpio));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_ASSERT_NOT_NULL(test, f->core->chans[3]);
	KUNIT_EXPECT_EQ(test,
			get_unaligned_le16(f->core->chans[3]->info.config), 0);
	KUNIT_EXPECT_NULL(test, lt_gpio_chip(f, 3));
	KUNIT_EXPECT_NULL(test, f->core->chans[3]->fdev->adev.dev.driver);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 0);
}

/* Kernel log lines that contain @text, while the probe is registered */
struct lt_log {
	const char *text;
	atomic_t n;
};

/* Called with interrupts off, from every printk */
static void lt_log_probe(void *data, const char *buf, size_t len)
{
	struct lt_log *log = data;

	if (strnstr(buf, log->text, len))
		atomic_inc(&log->n);
}

static void lt_log_stop(void *data)
{
	unregister_trace_console(lt_log_probe, data);
	tracepoint_synchronize_unregister();
}

/*
 * The count of LED lines left out is a debug line, so a channel
 * with an LED binds its other lines with no log line and no event
 */
static void lt_gpio_leds_quiet(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 3);
	u8 gpio[1 + 2 * sizeof(struct obmf_lstp_gpio_cfg)] = { 2 };
	struct lt_log *log;
	int ev;

	log = kunit_kzalloc(test, sizeof(*log), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, log);
	log->text = "LED lines left out";
	if (!IS_ENABLED(CONFIG_PRINTK) ||
	    register_trace_console(lt_log_probe, log))
		kunit_skip(test, "needs CONFIG_PRINTK and CONFIG_TRACEPOINTS");
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, lt_log_stop, log),
			0);

	lt_gpio(gpio + 1, "led", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_LOW, 0, 0,
		OBMF_LSTP_GPIO_LED_HOLD);
	lt_gpio(gpio + 1 + sizeof(struct obmf_lstp_gpio_cfg), "in",
		OBMF_LSTP_GPIO_IN, 0, 0, 0, 0);
	lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio", gpio, sizeof(gpio));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	KUNIT_EXPECT_NOT_NULL(test, lt_gpio_chip(f, 3));
	KUNIT_EXPECT_EQ(test, atomic_read(&log->n), 0);
	for (ev = 0; ev < OBMF_EV_NR; ev++)
		KUNIT_EXPECT_EQ_MSG(test,
				    atomic_long_read(&f->core->events[ev]), 0,
				    "event %d", ev);
}

/*
 * A push-pull output whose default is Z is an output translated to default
 * low, and reads low though the pin reports high
 */
static void lt_gpio_z_level(struct kunit *test)
{
	const struct obmf_gpio_cfg_entry *e;
	struct lt_fake *f = lt_create(test, 64, 3);
	u8 gpio[50] = { 1 };
	struct gpio_desc *pp;
	struct gpio_chip *gc;

	lt_gpio(gpio + 1, "pp", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_Z, 0, 0, 0);
	lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio", gpio, sizeof(gpio));
	__set_bit(0, f->gpio_level);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	e = lt_gpio_entry(f->core->chans[3], 0);
	KUNIT_EXPECT_EQ(test, e->line.direction, OBMF_LSTP_GPIO_OUT);
	KUNIT_EXPECT_EQ(test, e->line.default_output, OBMF_LSTP_GPIO_LOW);
	gc = lt_gpio_chip(f, 3);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	pp = lt_gpio_desc(test, gc, 0);
	KUNIT_EXPECT_EQ(test, gpiod_get_direction(pp), GPIO_LINE_DIRECTION_OUT);
	KUNIT_EXPECT_EQ(test, gpiod_get_value_cansleep(pp), 0);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 0);
}

/*
 * Only an output with LED_SUPPORT 1 or 2 is an LED and left out; an input
 * with LED_SUPPORT 1 or an output with 3 is a GPIO line
 */
static void lt_gpio_leds(struct kunit *test)
{
	const size_t esz = sizeof(struct obmf_lstp_gpio_cfg);
	struct lt_fake *f = lt_create(test, 64, 3);
	u8 gpio[1 + 3 * sizeof(struct obmf_lstp_gpio_cfg)] = { 3 };
	struct gpio_desc *d;
	struct gpio_chip *gc;

	lt_gpio(gpio + 1, "led", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_LOW, 0, 0,
		OBMF_LSTP_GPIO_LED_HOLD);
	lt_gpio(gpio + 1 + esz, "in-led", OBMF_LSTP_GPIO_IN, 0, 0, 0,
		OBMF_LSTP_GPIO_LED_INTERPOLATE);
	lt_gpio(gpio + 1 + 2 * esz, "led3", OBMF_LSTP_GPIO_OUT,
		OBMF_LSTP_GPIO_LOW, 0, 0, 3);
	lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio", gpio, sizeof(gpio));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	gc = lt_gpio_chip(f, 3);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_EXPECT_EQ(test, gc->ngpio, 3);
	KUNIT_EXPECT_FALSE(test, gpiochip_line_is_valid(gc, 0));
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, 1));
	KUNIT_EXPECT_TRUE(test, gpiochip_line_is_valid(gc, 2));
	KUNIT_EXPECT_STREQ(test, gc->names[1], "in-led");
	KUNIT_EXPECT_STREQ(test, gc->names[2], "led3");
	d = gpiochip_request_own_desc(gc, 0, "kunit", GPIO_LOOKUP_FLAGS_DEFAULT,
				      GPIOD_ASIS);
	KUNIT_EXPECT_TRUE(test, IS_ERR(d));
	if (!IS_ERR(d))
		gpiochip_free_own_desc(d);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 0);
}

/*
 * The chip label keeps the channel name: a 16-byte device name and a
 * 16-byte channel name are cut to the 31 bytes the GPIO uAPI reports
 */
static void lt_gpio_label(struct kunit *test)
{
	struct lt_fake *f = lt_create(test, 64, 3);
	u8 mgmt[2] = { OBMF_LSTP_VERSION, 3 };
	u8 gpio[50] = { 1 };
	struct gpio_chip *gc;

	lt_blob(f, 0, OBMF_LSTP_TYPE_MGMT, "0123456789abcdef", mgmt,
		sizeof(mgmt));
	lt_gpio(gpio + 1, "in", OBMF_LSTP_GPIO_IN, 0, 0, 0, 0);
	lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio-0123456789a", gpio,
		sizeof(gpio));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	gc = lt_gpio_chip(f, 3);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_EXPECT_STREQ(test, gc->label, "0123456789abcd_gpio-0123456789a");
}

static irqreturn_t lt_irq_thread(int irq, void *data)
{
	atomic_inc(data);
	return IRQ_HANDLED;
}

/*
 * An input's rising edge interrupt (LSTP 1.0, 4.3.2): a level one is
 * refused with nothing sent; SET_IRQ_CONFIG carries the line and the
 * rising type when it is requested, none when it is freed. An IRQ_EVENT
 * of the line runs the handler and is not answered; one of a line with
 * no interrupt is counted unused, one of a line past the chip malformed.
 */
static void lt_gpio_irq(struct kunit *test)
{
	static const u8 on[] = { 3, 0x03, 3, 0, 0x00, 0x00, 0x01 };
	static const u8 off[] = { 3, 0x03, 3, 0, 0x00, 0x00, 0x00 };
	static const u8 ev[] = { 3, 0x04, 3, 0, 0x00, 0x00, 0x01 };
	static const u8 unused[] = { 3, 0x04, 3, 0, 0x01, 0x00, 0x00 };
	static const u8 past[] = { 3, 0x04, 3, 0, 0x02, 0x00, 0x01 };
	const size_t esz = sizeof(struct obmf_lstp_gpio_cfg);
	struct lt_fake *f = lt_create(test, 64, 3);
	u8 gpio[1 + 2 * sizeof(struct obmf_lstp_gpio_cfg)] = { 2 };
	atomic_t *calls = kunit_kzalloc(test, sizeof(*calls), GFP_KERNEL);
	struct gpio_chip *gc;
	long bad;
	int irq;

	KUNIT_ASSERT_NOT_NULL(test, calls);
	lt_gpio(gpio + 1, "irq", OBMF_LSTP_GPIO_IN, 0, 0, 0, 0);
	lt_gpio(gpio + 1 + esz, "reset", OBMF_LSTP_GPIO_OUT, OBMF_LSTP_GPIO_LOW,
		0, 0, 0);
	lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio", gpio, sizeof(gpio));
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	gc = lt_gpio_chip(f, 3);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	irq = gpiod_to_irq(lt_gpio_desc(test, gc, 0));
	KUNIT_ASSERT_GT(test, irq, 0);
	KUNIT_ASSERT_EQ(test,
			request_threaded_irq(irq, NULL, lt_irq_thread,
					     IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
					     "kunit", calls),
			-EINVAL);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 0);
	KUNIT_ASSERT_EQ(test,
			request_threaded_irq(irq, NULL, lt_irq_thread,
					     IRQF_ONESHOT | IRQF_TRIGGER_RISING,
					     "kunit", calls),
			0);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 1);
	KUNIT_EXPECT_EQ(test, f->gpio_sent_len, sizeof(on));
	KUNIT_EXPECT_MEMEQ(test, f->gpio_sent, on, sizeof(on));

	obmf_core_rx(f->core, ev, sizeof(ev));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(calls), 1);
	obmf_core_rx(f->core, unused, sizeof(unused));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&f->core->events[OBMF_EV_GPIO_IRQ_UNUSED]),
			1);
	bad = atomic_long_read(&f->core->cnt[OBMF_IF_RX_MALFORMED]);
	obmf_core_rx(f->core, past, sizeof(past));
	flush_workqueue(f->core->wq);
	KUNIT_EXPECT_EQ(test, atomic_read(calls), 1);
	KUNIT_EXPECT_EQ(test,
			atomic_long_read(&f->core->cnt[OBMF_IF_RX_MALFORMED]),
			bad + 1);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 1);
	KUNIT_EXPECT_EQ(test, f->sends, 0);

	free_irq(irq, calls);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 2);
	KUNIT_EXPECT_MEMEQ(test, f->gpio_sent, off, sizeof(off));
}

/*
 * Several lines in one SET_VALUE; a Get of inputs and outputs
 * sends a GET_VALUE of the inputs only, on a device that refuses one of an
 * output, the reply matched by position. A SET_VALUE that fails, though
 * the device set some lines, reads nothing back and keeps the values last
 * set.
 */
static void lt_gpio_multiple(struct kunit *test)
{
	static const u8 set_wire[] = { 3, 0x01, 9, 0, 0x00, 0x00, 1,
				       0x01, 0x00, 0, 0x03, 0x00, 1 };
	static const u8 get_wire[] = { 3, 0x00, 4, 0, 0x02, 0x00, 0x04, 0x00 };
	const size_t esz = sizeof(struct obmf_lstp_gpio_cfg);
	struct lt_fake *f = lt_create(test, 64, 3);
	u8 gpio[1 + 5 * sizeof(struct obmf_lstp_gpio_cfg)] = { 5 };
	unsigned long mask, bits;
	struct gpio_chip *gc;
	int i;

	/* inputs 2 and 4, the others outputs */
	for (i = 0; i < 5; i++)
		lt_gpio(gpio + 1 + i * esz, "line",
			BIT(i) & 0x14 ? OBMF_LSTP_GPIO_IN : OBMF_LSTP_GPIO_OUT,
			OBMF_LSTP_GPIO_LOW, 0, 0, 0);
	lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio", gpio, sizeof(gpio));
	f->gpio_wo[0] = 0xb;
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	gc = lt_gpio_chip(f, 3);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_ASSERT_EQ(test, gc->ngpio, 5);

	mask = 0xb;
	bits = 0x9;
	KUNIT_EXPECT_EQ(test, lt_gpio_set_multiple(test, gc, &mask, &bits), 0);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 1);
	KUNIT_EXPECT_EQ(test, f->gpio_sent_len, sizeof(set_wire));
	KUNIT_EXPECT_MEMEQ(test, f->gpio_sent, set_wire, sizeof(set_wire));
	KUNIT_EXPECT_EQ(test, f->gpio_level[0], 0x9);

	__set_bit(4, f->gpio_level);
	mask = 0x1f;
	bits = 0;
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), 0);
	KUNIT_EXPECT_EQ(test, bits, 0x19);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 2);
	KUNIT_EXPECT_EQ(test, f->gpio_sent_len, sizeof(get_wire));
	KUNIT_EXPECT_MEMEQ(test, f->gpio_sent, get_wire, sizeof(get_wire));

	f->gpio_status = OBMF_LSTP_NACK;
	f->gpio_prefix = 2;
	mask = 0xb;
	bits = 0x2;
	KUNIT_EXPECT_EQ(test, lt_gpio_set_multiple(test, gc, &mask, &bits),
			-EIO);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 3);
	KUNIT_EXPECT_EQ(test, f->gpio_level[0], 0x1a);
	f->gpio_status = 0;
	bits = 0;
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, &mask, &bits), 0);
	KUNIT_EXPECT_EQ(test, bits, 0x9);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 3);
}

/*
 * A Get of more lines than one GET_VALUE carries is split at 30 lines, a
 * Set at 20 (64-byte messages)
 */
static void lt_gpio_split(struct kunit *test)
{
	const size_t esz = sizeof(struct obmf_lstp_gpio_cfg);
	struct lt_fake *f = lt_create(test, 64, 3);
	unsigned long *mask, *bits;
	struct gpio_chip *gc;
	u8 *gpio;
	int i;

	gpio = kunit_kzalloc(test, 1 + 62 * esz, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, gpio);
	gpio[0] = 62;
	for (i = 0; i < 62; i++)
		lt_gpio(gpio + 1 + i * esz, "line",
			i < 31 ? OBMF_LSTP_GPIO_OUT : OBMF_LSTP_GPIO_IN,
			OBMF_LSTP_GPIO_LOW, 0, 0, 0);
	lt_blob(f, 3, OBMF_LSTP_TYPE_GPIO, "gpio", gpio, 1 + 62 * esz);
	KUNIT_ASSERT_EQ(test, lt_bind(f), 0);
	gc = lt_gpio_chip(f, 3);
	KUNIT_ASSERT_NOT_NULL(test, gc);
	KUNIT_ASSERT_EQ(test, gc->ngpio, 62);

	mask = kunit_kcalloc(test, BITS_TO_LONGS(62), sizeof(long), GFP_KERNEL);
	bits = kunit_kcalloc(test, BITS_TO_LONGS(62), sizeof(long), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, mask);
	KUNIT_ASSERT_NOT_NULL(test, bits);
	bitmap_set(mask, 0, 31);
	bitmap_fill(bits, 62);
	KUNIT_EXPECT_EQ(test, lt_gpio_set_multiple(test, gc, mask, bits), 0);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 2);
	KUNIT_EXPECT_EQ(test, f->gpio_sent_len, OBMF_HDR_LEN + 11 * 3);
	KUNIT_EXPECT_TRUE(test, bitmap_full(f->gpio_level, 31));

	bitmap_zero(mask, 62);
	bitmap_set(mask, 31, 31);
	bitmap_zero(bits, 62);
	bitmap_set(f->gpio_level, 31, 30);
	KUNIT_EXPECT_EQ(test, gc->get_multiple(gc, mask, bits), 0);
	KUNIT_EXPECT_EQ(test, f->gpio_sends, 4);
	KUNIT_EXPECT_EQ(test, f->gpio_sent_len, OBMF_HDR_LEN + 2);
	KUNIT_EXPECT_EQ(test, bitmap_weight(bits, 62), 30);
	KUNIT_EXPECT_FALSE(test, test_bit(61, bits));
}

#endif /* IS_BUILTIN(CONFIG_GPIO_OBMF) */

static struct kunit_case obmf_lstp_cases[] = {
	KUNIT_CASE(lt_i2c_encode_table),
	KUNIT_CASE(lt_i2c_decode_table),
	KUNIT_CASE(lt_i2c_encode_bad),
	KUNIT_CASE(lt_i2c_decode_status),
	KUNIT_CASE(lt_i2c_decode_bad),
	KUNIT_CASE(lt_spi_encode_table),
	KUNIT_CASE(lt_spi_encode_bad),
	KUNIT_CASE(lt_spi_decode_table),
	KUNIT_CASE(lt_spi_decode_write_data),
	KUNIT_CASE(lt_uart_encode_table),
	KUNIT_CASE(lt_uart_dreq_table),
	KUNIT_CASE(lt_uart_bad),
	KUNIT_CASE(lt_uart_decode_status),
	KUNIT_CASE(lt_uart_answer),
	KUNIT_CASE(lt_gpio_encode_table),
	KUNIT_CASE(lt_gpio_encode_bad),
	KUNIT_CASE(lt_gpio_decode_table),
	KUNIT_CASE(lt_gpio_decode_bad),
	KUNIT_CASE(lt_gpio_irq_table),
	KUNIT_CASE(lt_gpio_irq_event),
	KUNIT_CASE(lt_other_types),
	KUNIT_CASE(lt_status_errno),
	KUNIT_CASE(lt_ch_reg),
	KUNIT_CASE(lt_usb_ids),
	KUNIT_CASE(lt_discover),
	KUNIT_CASE(lt_discover_none),
	KUNIT_CASE(lt_discover_skip),
	KUNIT_CASE(lt_discover_i2c_short),
	KUNIT_CASE(lt_discover_uart_short),
	KUNIT_CASE(lt_discover_spi),
	KUNIT_CASE(lt_discover_budget),
	KUNIT_CASE(lt_info_line),
	KUNIT_CASE_SLOW(lt_discover_fail),
	KUNIT_CASE_SLOW(lt_discover_cut),
	KUNIT_CASE(lt_discover_table),
	KUNIT_CASE(lt_gpio_table),
	KUNIT_CASE(lt_gpio_table_bounds),
	KUNIT_CASE(lt_gpio_quirks),
	KUNIT_CASE_SLOW(lt_discover_again),
	KUNIT_CASE_SLOW(lt_discover_busy),
	KUNIT_CASE_SLOW(lt_discover_short),
	KUNIT_CASE_SLOW(lt_discover_silent),
	KUNIT_CASE(lt_i2c_round_trip),
	KUNIT_CASE(lt_spi_round_trip),
	KUNIT_CASE(lt_spi_limit),
	KUNIT_CASE(lt_uart_round_trip),
	KUNIT_CASE(lt_gpio_round_trip),
	KUNIT_CASE(lt_default_answer),
	KUNIT_CASE(lt_rx_before_publish),
	KUNIT_CASE(lt_wire_then_request),
	KUNIT_CASE(lt_late_reply),
	KUNIT_CASE(lt_i2c_recovery),
	KUNIT_CASE(lt_i2c_write_limit),
	{}
};

static struct kunit_suite obmf_lstp_suite = {
	.name = "obmf-lstp",
	.test_cases = obmf_lstp_cases,
};

#if IS_BUILTIN(CONFIG_OBMF_TTY) || IS_BUILTIN(CONFIG_I2C_OBMF) || \
	IS_BUILTIN(CONFIG_GPIO_OBMF) || IS_BUILTIN(CONFIG_SPI_OBMF)
/* The function drivers on an LSTP device, those built in */
static struct kunit_case obmf_lstp_fn_cases[] = {
#if IS_BUILTIN(CONFIG_OBMF_TTY)
	KUNIT_CASE(lt_uart_closed),
	KUNIT_CASE(lt_uart_refused),
	KUNIT_CASE(lt_uart_queue_full),
#endif
#if IS_BUILTIN(CONFIG_I2C_OBMF)
	KUNIT_CASE(lt_i2c_pec),
	KUNIT_CASE_SLOW(lt_i2c_no_recovery),
	KUNIT_CASE_SLOW(lt_i2c_late_reply),
#endif
#if IS_BUILTIN(CONFIG_SPI_OBMF)
	KUNIT_CASE(lt_spi_rdid),
#endif
#if IS_BUILTIN(CONFIG_GPIO_OBMF)
	KUNIT_CASE(lt_gpio_chip_io),
	KUNIT_CASE(lt_gpio_out_shadow),
	KUNIT_CASE(lt_gpio_leds_only),
	KUNIT_CASE(lt_gpio_z_level),
	KUNIT_CASE(lt_gpio_leds),
	KUNIT_CASE(lt_gpio_leds_quiet),
	KUNIT_CASE(lt_gpio_multiple),
	KUNIT_CASE(lt_gpio_split),
	KUNIT_CASE(lt_gpio_label),
	KUNIT_CASE(lt_gpio_irq),
#endif
	{}
};

static struct kunit_suite obmf_lstp_fn_suite = {
	.name = "obmf-lstp-fn",
	.suite_init = lt_fn_suite_init,
	.suite_exit = lt_fn_suite_exit,
	.test_cases = obmf_lstp_fn_cases,
};

kunit_test_suites(&obmf_lstp_suite, &obmf_lstp_fn_suite);
#else
kunit_test_suite(obmf_lstp_suite);
#endif

#endif /* CONFIG_OBMF_LSTP_KUNIT_TEST */
