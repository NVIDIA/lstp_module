/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OBMF-ICP over USB: interface for function drivers
 *
 * The core binds the USB interface, discovers the channels through
 * channel 0 and creates one auxiliary device per channel that has a
 * driver. A function driver binds that device and talks to its channel
 * through the calls below.
 */

#ifndef __LINUX_USB_OBMF_H
#define __LINUX_USB_OBMF_H

#include <linux/auxiliary_bus.h>
#include <linux/bits.h>
#include <linux/ratelimit_types.h>
#include <linux/types.h>

/* OBMF-ICP 1.0.0, 5.15, Table 65: CHANNEL_TYPE */
#define OBMF_TYPE_CONFIG 0x00
#define OBMF_TYPE_MMIO 0x01
#define OBMF_TYPE_GPIO 0x02
#define OBMF_TYPE_SERIAL 0x03
#define OBMF_TYPE_I2C_CONTROLLER 0x04
#define OBMF_TYPE_I2C_TARGET 0x05
#define OBMF_TYPE_I3C 0x06
#define OBMF_TYPE_IPMI 0x07
#define OBMF_TYPE_SPI 0x08
#define OBMF_TYPE_IO 0x09
#define OBMF_TYPE_MEMDEV 0x0a
#define OBMF_TYPE_TPM 0x0b
#define OBMF_TYPE_RTC 0x0c

/* OBMF-ICP 1.0.0, 5.15, Table 65: DEVICE_ROLE */
#define OBMF_ROLE_PRODUCER 0
#define OBMF_ROLE_CONSUMER 1
#define OBMF_ROLE_SYMMETRIC 2

/* OBMF-ICP 1.0.0, 5.2, Table 4: generic header status codes */
#define OBMF_STATUS_SUCCESS 0x00
#define OBMF_STATUS_INVALID 0x01
#define OBMF_STATUS_TIMEOUT 0x02
#define OBMF_STATUS_BUSY 0x03
#define OBMF_STATUS_PERMANENT_ERROR 0x04
#define OBMF_STATUS_UNKNOWN_CHANNEL 0x05
#define OBMF_STATUS_SIZE_UNSUPPORTED 0x06

/* OBMF-ICP 1.0.0, 5.6, Table 25: I2C controller status codes */
#define OBMF_STATUS_I2C_TIMEOUT 0x40
#define OBMF_STATUS_I2C_ARB_LOST 0x41
#define OBMF_STATUS_I2C_BUS_BUSY 0x42
#define OBMF_STATUS_I2C_NACK 0x43

/* OBMF-ICP 1.0.0, 5.5, Table 21: serial status codes */
#define OBMF_STATUS_SERIAL_LINE_TIMEOUT 0x40

/* OBMF-ICP 1.0.0, 5.4, Table 17: GPIO status codes */
#define OBMF_STATUS_GPIO_INDEX_UNSUPPORTED 0x40
#define OBMF_STATUS_GPIO_IRQ_UNSUPPORTED 0x41
#define OBMF_STATUS_GPIO_INVALID_OPERATION 0x42

/* OBMF-ICP 1.0.0, 5.8, Table 34: I3C controller status codes */
#define OBMF_STATUS_I3C_TIMEOUT 0x40
#define OBMF_STATUS_I3C_ARB_LOST 0x41
#define OBMF_STATUS_I3C_BUS_BUSY 0x42
#define OBMF_STATUS_I3C_NACK 0x43
#define OBMF_STATUS_I3C_BUS_FAULT 0x44
#define OBMF_STATUS_I3C_PARITY 0x45
#define OBMF_STATUS_I3C_BAD_CCC_REPLY 0x46

/* OBMF-ICP 1.0.0, 5.11, Table 54: I/O status codes */
#define OBMF_STATUS_IO_OUT_OF_RANGE 0x40
#define OBMF_STATUS_IO_ACCESS_DENIED 0x41

/* OBMF-ICP 1.0.0, 5.12, Table 60: memory device status codes */
#define OBMF_STATUS_MEMDEV_ACCESS_DENIED 0x40
#define OBMF_STATUS_MEMDEV_SIZE_UNSUPPORTED 0x41
#define OBMF_STATUS_MEMDEV_ERASE_MISALIGNED 0x42
#define OBMF_STATUS_MEMDEV_WRITE_PROTECTED 0x43

/* OBMF-ICP 1.0.0, 5.15, Table 65: CHANNEL_STATUS */
#define OBMF_CH_STATUS_HEALTH GENMASK(3, 0)
#define OBMF_CH_STATUS_RESET_DONE BIT(4)
#define OBMF_CH_STATUS_CONFIG_VALID BIT(5)
#define OBMF_CH_STATUS_LOCKED BIT(6)
#define OBMF_CH_STATUS_ERROR BIT(7)

/* OBMF-ICP 1.0.0, 5.15, Table 65: CHANNEL_CONTROL */
#define OBMF_CH_CONTROL_ENABLE BIT(0)
#define OBMF_CH_CONTROL_RESET BIT(1)
#define OBMF_CH_CONTROL_APPLY BIT(2)
#define OBMF_CH_CONTROL_LOCK_REQ BIT(3)
#define OBMF_CH_CONTROL_CLEAR_ERROR BIT(4)

#define OBMF_DEFAULT_TIMEOUT_MS 1000
#define OBMF_MAX_TIMEOUT_MS 10000

struct obmf_channel;

/**
 * struct obmf_channel_quirks - what the device does on a channel beyond
 *	its configuration, for the function driver to adapt to
 * @i2c_dev_retries: the device retries a lost I2C arbitration itself
 */
struct obmf_channel_quirks {
	bool i2c_dev_retries;
};

/**
 * struct obmf_channel_info - one discovered channel, read only
 * @number: channel number on the wire, 1 to 255
 * @type: CHANNEL_TYPE
 * @role: DEVICE_ROLE
 * @name: CHANNEL_NAME as a string, empty if the device gave none
 * @max_req: largest request payload, command byte included
 * @max_resp: largest response payload, echo byte included
 * @config: the channel's configuration data
 * @config_len: length of @config
 * @quirks: the device's behaviour on the channel
 */
struct obmf_channel_info {
	u8 number;
	u8 type;
	u8 role;
	char name[17];
	size_t max_req;
	size_t max_resp;
	const u8 *config;
	size_t config_len;
	struct obmf_channel_quirks quirks;
};

/**
 * struct obmf_fdev - function device, one per channel with a driver
 * @adev: the auxiliary device
 * @info: the channel, valid while the device exists
 * @ch: core private
 */
struct obmf_fdev {
	struct auxiliary_device adev;
	const struct obmf_channel_info *info;
	struct obmf_channel *ch;
};

#define to_obmf_fdev(_adev) container_of(_adev, struct obmf_fdev, adev)

/**
 * struct obmf_dreq - a request the device sent
 * @cmd: payload byte 0
 * @answer_due: the device waits for a response
 * @data: payload after byte 0
 * @len: length of @data
 */
struct obmf_dreq {
	u8 cmd;
	bool answer_due;
	const u8 *data;
	size_t len;
};

/**
 * struct obmf_fdev_ops - device requests for a published driver
 * @device_request: called in process context, one at a time per channel.
 *	Return 0 when answered (or not due), -EINPROGRESS to keep @dreq and
 *	answer later with obmf_respond() or obmf_dreq_drop(), or an error
 *	for the core to send the default answer. A return of 0 that left
 *	a due answer unsent gets the default answer too, with a warning.
 * @failed: optional; called once in process context after a transport
 *	error stopped the device. It can run while the function device is
 *	being removed, until the action devm_obmf_publish() added has run.
 *	Every request fails from then on; obmf_fdev_errno() gives its errno.
 *	It runs under the lock that delivers @device_request: it may sleep
 *	and call obmf_request(), obmf_respond() and obmf_dreq_drop(), but
 *	must not unbind its device, publish, or wait for a @device_request
 *	call to return.
 */
struct obmf_fdev_ops {
	int (*device_request)(struct obmf_fdev *fdev, struct obmf_dreq *dreq);
	void (*failed)(struct obmf_fdev *fdev);
};

int obmf_request(struct obmf_fdev *fdev, u8 cmd, const void *req,
		 size_t req_len, void *resp, size_t resp_cap, size_t *resp_len,
		 unsigned int timeout_ms);
int obmf_respond(struct obmf_fdev *fdev, struct obmf_dreq *dreq, u8 status,
		 const void *data, size_t len);
void obmf_dreq_drop(struct obmf_fdev *fdev, struct obmf_dreq *dreq);

int devm_obmf_enable(struct obmf_fdev *fdev);
int devm_obmf_publish(struct obmf_fdev *fdev, const struct obmf_fdev_ops *ops);

void obmf_fdev_label(struct obmf_fdev *fdev, char *buf, size_t size);
void obmf_fdev_protocol_error(struct obmf_fdev *fdev);
int obmf_fdev_errno(struct obmf_fdev *fdev);

/*
 * What the core counts and logs; the names are in obmf-bus.c. A function
 * driver reports only the events from OBMF_EV_FDEV_FIRST on.
 */
enum obmf_event {
	OBMF_EV_FATAL,
	OBMF_EV_IN_STALL,
	OBMF_EV_OUT_STALL,
	OBMF_EV_IN_CLEAR_HALT,
	OBMF_EV_OUT_CLEAR_HALT,
	OBMF_EV_NO_RESPONSE,
	OBMF_EV_DEVICE_ERROR,
	OBMF_EV_BAD_RESPONSE,
	OBMF_EV_RX_SHORT,
	OBMF_EV_RX_OVERSIZE,
	OBMF_EV_RX_TRAILING,
	OBMF_EV_RX_NO_CHANNEL,
	OBMF_EV_BAD_DREQ,
	OBMF_EV_ANSWER_NOT_SENT,
	OBMF_EV_CH_HEALTH,
	OBMF_EV_QUEUE_FULL,
	OBMF_EV_FDEV_FIRST,
	OBMF_EV_TX_DROPPED = OBMF_EV_FDEV_FIRST,
	OBMF_EV_NR,
};

void __obmf_fdev_report(struct obmf_fdev *fdev, enum obmf_event ev, long a,
			long b, struct ratelimit_state *rs, const char *func);

/*
 * Counts @ev, then logs it on @fdev: at most DEFAULT_RATELIMIT_BURST
 * lines per DEFAULT_RATELIMIT_INTERVAL from this call site, for all
 * devices. @a and @b are the event's values. Any context.
 */
#define obmf_fdev_report(fdev, ev, a, b)                                       \
	do {                                                                   \
		static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL, \
					      DEFAULT_RATELIMIT_BURST);        \
									       \
		__obmf_fdev_report(fdev, ev, a, b, &_rs, __func__);            \
	} while (0)

#if IS_ENABLED(CONFIG_OBMF_KUNIT_TEST)
struct tty_struct;

void obmf_tty_kunit_counts(struct tty_struct *tty, unsigned long *dropped,
			   unsigned long *stalls);
#endif

#endif /* __LINUX_USB_OBMF_H */
