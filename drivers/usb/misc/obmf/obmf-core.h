/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OBMF-ICP over USB: private interface between the transport, the
 * message layer, channel 0 and the function device registry
 */

#ifndef __OBMF_CORE_H
#define __OBMF_CORE_H

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/ratelimit_types.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/usb/obmf.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

/* OBMF-ICP 1.0.0, 5.1, Table 3: message header */
struct obmf_hdr {
	u8 channel;
	u8 status;
	__le16 size;
} __packed;

#define OBMF_HDR_LEN sizeof(struct obmf_hdr)
#define OBMF_HDR_STATUS GENMASK(6, 0)
#define OBMF_HDR_RQRESP BIT(7)

#define OBMF_NR_CHANNELS 256

/* OBMF-ICP 1.0.0, 5.15, Table 65: the channel header bytes of ch_reg */
#define OBMF_CH_REG_STATUS 0x13
#define OBMF_CH_REG_CONTROL 0x14
#define OBMF_CH_REG_SPECIFIC_STATUS 0x15
#define OBMF_CH_REG_SPECIFIC_CONTROL 0x16

struct obmf_channel;
struct obmf_core;
struct obmf_transport;
struct dentry;
struct fwnode_handle;

/**
 * struct obmf_usb_profile - how a protocol uses its USB interface
 * @func_desc: the OBMF-ICP functional descriptor gives the transfer
 *	limits; otherwise one message per packet, the limits from the bulk
 *	endpoints
 * @zlp: a zero-length packet ends an OUT message that fills its last
 *	packet
 * @max_packet: without @func_desc, the largest limit per direction
 * @resp_timeout_floor_ms: lower bound of every response wait, rounded to
 *	ticks: a wait that starts within a tick may end up to a tick early
 */
struct obmf_usb_profile {
	bool func_desc;
	bool zlp;
	u16 max_packet;
	unsigned int resp_timeout_floor_ms;
};

/**
 * struct obmf_usb_proto - what a USB match entry's driver_info points to
 * @profile: the protocol's use of the interface
 */
struct obmf_usb_proto {
	const struct obmf_usb_profile *profile;
#ifdef CONFIG_OBMF_DIALECTS
	/** @dialect: its edge translation, NULL for OBMF-ICP */
	const struct obmf_dialect *dialect;
#endif
};

/**
 * struct obmf_transport_ops - what the core needs from a transport
 * @start: arm reception
 * @stop: stop for good: cancel and refuse all transfers
 * @quiesce: cancel reception until @resume
 * @resume: arm reception again after @quiesce
 * @send: send one message, sleeping, at most @timeout_ms
 */
struct obmf_transport_ops {
	int (*start)(struct obmf_transport *tp);
	void (*stop)(struct obmf_transport *tp);
	void (*quiesce)(struct obmf_transport *tp);
	int (*resume)(struct obmf_transport *tp);
	int (*send)(struct obmf_transport *tp, const void *buf, size_t len,
		    unsigned int timeout_ms);
};

/**
 * struct obmf_transport - one transport instance
 * @ops: the transport's operations
 * @dev: the device the transport binds, parent of the function devices
 * @max_tx_msg: largest message to send, header included
 * @max_rx_msg: largest message to ask for, header included; the transport
 *	accepts messages one header longer
 * @resp_timeout_floor_ms: lower bound of every response wait, rounded to
 *	ticks as in struct obmf_usb_profile
 * @acpi_adr: the _ADR of the ACPI object that holds the channel objects,
 *	a child of @dev's companion
 * @core: the core on this transport
 */
struct obmf_transport {
	const struct obmf_transport_ops *ops;
	struct device *dev;
	size_t max_tx_msg;
	size_t max_rx_msg;
	unsigned int resp_timeout_floor_ms;
	u32 acpi_adr;
#ifdef CONFIG_OBMF_DIALECTS
	/**
	 * @dialect: the edge translation of the device's protocol, NULL for
	 *	OBMF-ICP; set before obmf_core_create()
	 */
	const struct obmf_dialect *dialect;
#endif
	struct obmf_core *core;
};

enum obmf_state {
	OBMF_INIT,
	OBMF_LIVE,
	OBMF_QUIESCED,
	OBMF_UNBINDING,
	OBMF_STOPPING,
	OBMF_FAILED,
	OBMF_STOPPED,
};

/* How a device lays out its MMIO read responses */
enum obmf_mmio_form {
	OBMF_MMIO_FORM_UNKNOWN,
	OBMF_MMIO_FORM_HEAD,
	OBMF_MMIO_FORM_DATA,
};

enum obmf_if_counter {
	OBMF_IF_RX_MESSAGES,
	OBMF_IF_TX_MESSAGES,
	OBMF_IF_RX_MALFORMED,
	OBMF_IF_RX_NO_CHANNEL,
	OBMF_IF_RX_URB_ERRORS,
	OBMF_IF_TX_URB_ERRORS,
	OBMF_IF_STALLS,
	OBMF_IF_FATAL_ERRORS,
	OBMF_IF_NR,
};

enum obmf_ch_counter {
	OBMF_CH_TX_REQUESTS,
	OBMF_CH_RX_RESPONSES,
	OBMF_CH_RX_LATE,
	OBMF_CH_TIMEOUTS,
	OBMF_CH_STATUS_ERRORS,
	OBMF_CH_PROTOCOL_ERRORS,
	OBMF_CH_TX_ERRORS,
	OBMF_CH_RX_DEVICE_REQUESTS,
	OBMF_CH_TX_ANSWERS,
	OBMF_CH_DEFAULT_ANSWERS, /* due, counted before the send */
	OBMF_CH_RX_DROPPED,
	OBMF_CH_RX_REPLACED,
	OBMF_CH_DREQ_MAX_DEPTH, /* most records in use at once */
	OBMF_CH_NR,
};

/**
 * struct obmf_slot - the one request in flight on a channel
 * @lock: held by the waiter for the whole request
 * @rx_lock: guards the fields below against the receive upcall
 * @armed: a request waits for its response
 * @cmd: payload byte 0 of the request
 * @status: header status of the response in OBMF-ICP form
 * @err: nonzero when the slot was completed without a response
 * @buf: response payload
 * @cap: size of @buf
 * @len: length of the response payload in @buf
 * @held: the last request timed out; the next one waits for its late
 *	reply, or until @held_until
 * @held_until: in jiffies
 * @done: completed by the upcall or by the core
 */
struct obmf_slot {
	struct mutex lock; /* one request at a time */
	spinlock_t rx_lock; /* the fields below */
	bool armed;
#ifdef CONFIG_OBMF_DIALECTS
	/** @wire: the request takes its reply as received, header included */
	bool wire;
	/** @raw_status: header status of the response as received */
	u8 raw_status;
#endif
	u8 cmd;
	u8 status;
	int err;
	u8 *buf;
	size_t cap;
	size_t len;
	bool held;
	unsigned long held_until;
	struct completion done;
};

/* A late reply, taken by nobody, ends the wait obmf_slot_xfer() holds */
static inline void obmf_slot_late(struct obmf_slot *slot)
{
	lockdep_assert_held(&slot->rx_lock);
	if (slot->held) {
		slot->held = false;
		complete(&slot->done);
	}
}

/**
 * struct obmf_dreq_rec - a queued device request
 * @node: entry in the channel's queue or kept list
 * @dreq: what the driver sees
 * @prealloc: one of the channel's reserved records
 * @kept: on the channel's kept list
 * @done: answered or dropped by the driver
 * @cap: size of @buf
 * @buf: the payload
 */
struct obmf_dreq_rec {
	struct list_head node;
	struct obmf_dreq dreq;
	bool prealloc;
	bool kept;
	bool done;
	size_t cap;
	u8 buf[] __counted_by(cap);
};

#define OBMF_DREQ_RESERVE 3
#define OBMF_DREQ_QUEUE_MAX 16

/**
 * struct obmf_channel - core record of one channel
 * @core: the core
 * @info: what function drivers see
 * @hdr_off: channel header offset in the channel 0 space, 0 for channel 0
 * @status: CHANNEL_STATUS read at discovery
 * @control: CHANNEL_CONTROL read at discovery
 * @no_device: firmware says the channel has no device
 * @bound: a function driver called devm_obmf_enable(), under the
 *	function device's lock
 * @config: configuration data
 * @slot: the request slot
 * @tx_buf: encode buffer, under @slot.lock
 * @tx_cap: size of @tx_buf
 * @dreq_lock: guards the device request fields below
 * @queue: queued device requests
 * @kept: device requests a driver kept to answer later
 * @reserve: the reserved records not in use
 * @queued: allocated records on @queue
 * @used: records not in @reserve, queued, delivered or kept
 * @owed: answers owed but not queued, for want of a record
 * @owed_cmd: payload byte 0 of the last owed request
 * @owed_invalid: invalid answers owed to requests that could not be decoded
 * @work: delivers the device requests
 * @dispatch_lock: held while a device request is delivered
 * @fdev: the function device, NULL if none
 * @ops: device request operations, set while published
 * @publisher: the function device while published
 * @fwnode: firmware node matched to the channel
 * @cnt: counters
 * @dbg: debugfs directory
 */
struct obmf_channel {
	struct obmf_core *core;
	struct obmf_channel_info info;
	u32 hdr_off;
	u8 status;
	u8 control;
	bool no_device;
	bool bound;
	u8 *config;
	struct obmf_slot slot;
	u8 *tx_buf;
	size_t tx_cap;
#ifdef CONFIG_OBMF_DIALECTS
	/** @rx_dec: a dialect's decoded response, under @slot.rx_lock */
	u8 *rx_dec;
	/** @dreq_dec: a dialect's decoded device request, under @dreq_lock */
	u8 *dreq_dec;
	/** @dec_cap: size of @rx_dec and of @dreq_dec */
	size_t dec_cap;
	/** @dialect_data: the dialect's state for the channel, freed with it */
	void *dialect_data;
#endif
	spinlock_t dreq_lock; /* queue to owed_invalid */
	struct list_head queue;
	struct list_head kept;
	struct obmf_dreq_rec *reserve[OBMF_DREQ_RESERVE];
	unsigned int queued;
	unsigned int used;
	unsigned int owed;
	u8 owed_cmd;
	unsigned int owed_invalid;
	struct work_struct work;
	struct mutex dispatch_lock; /* ops and publisher */
	struct obmf_fdev *fdev;
	const struct obmf_fdev_ops *ops;
	struct obmf_fdev *publisher;
	struct fwnode_handle *fwnode;
	atomic_long_t cnt[OBMF_CH_NR];
	struct dentry *dbg;
};

/**
 * struct obmf_core - one OBMF-ICP device
 * @tp: the transport
 * @dev: the transport's device
 * @state: enum obmf_state; every edge but the one to STOPPED is a
 *	compare-and-exchange, then every slot's rx_lock is taken
 * @inflight_lock: guards @inflight
 * @inflight: calls from function drivers in progress
 * @inflight_wq: woken when @inflight drops to zero
 * @ch0_lock: serializes channel 0 read-modify-write sequences
 * @wq: device request delivery
 * @fail_work: calls each published driver's failed operation after a
 *	transport error stopped the device
 * @ch0: channel 0
 * @chans: discovered channels by number, published by @table
 * @table: set once @chans is complete
 * @version_major: OBMF-ICP major version, BCD; for another wire protocol,
 *	its version as its discovery gives it
 * @version_minor: OBMF-ICP minor version, BCD
 * @vendor_id: VENDOR_ID, a PCI-SIG vendor ID; 0 where the wire protocol
 *	gives none, as a USB vendor ID is not one
 * @device_id: DEVICE_ID
 * @device_name: DEVICE_NAME as a string, empty if none
 * @ch0_chunk: largest channel 0 read
 * @mmio_form: enum obmf_mmio_form, the last read response's
 * @cnt: interface counters
 * @events: reported events, logged or not
 * @dbg: debugfs directory
 */
struct obmf_core {
	struct obmf_transport *tp;
	struct device *dev;
	int state;
	spinlock_t inflight_lock; /* inflight */
	unsigned int inflight;
	wait_queue_head_t inflight_wq;
	struct mutex ch0_lock; /* channel 0 read-modify-write */
	struct workqueue_struct *wq;
	struct work_struct fail_work;
	struct obmf_channel *ch0;
	struct obmf_channel *chans[OBMF_NR_CHANNELS];
	bool table;
	u8 version_major;
	u8 version_minor;
	u16 vendor_id;
	u16 device_id;
	char device_name[33];
	size_t ch0_chunk;
	int mmio_form;
	atomic_long_t cnt[OBMF_IF_NR];
	atomic_long_t events[OBMF_EV_NR];
	struct dentry *dbg;
};

static inline void obmf_count(struct obmf_core *core, enum obmf_if_counter c)
{
	atomic_long_inc(&core->cnt[c]);
}

void obmf_report_log(struct device *dev, enum obmf_event ev, int ch, long a,
		     long b, struct ratelimit_state *rs, const char *func);

/*
 * Counts @ev, then logs it on @dev: at most DEFAULT_RATELIMIT_BURST lines
 * per DEFAULT_RATELIMIT_INTERVAL from this call site, for all devices.
 * @ch is the channel or -1, @a and @b the event's values. Any context.
 */
#define obmf_report(core, dev, ev, ch, a, b)                                   \
	do {                                                                   \
		static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL, \
					      DEFAULT_RATELIMIT_BURST);        \
		enum obmf_event _ev = (ev);                                    \
									       \
		atomic_long_inc(&(core)->events[_ev]);                         \
		obmf_report_log(dev, _ev, ch, a, b, &_rs, __func__);           \
	} while (0)

/* An event the core's state machine bounds: counted, always logged */
static inline void obmf_report_bounded(struct obmf_core *core,
				       struct device *dev, enum obmf_event ev,
				       int ch, long a, long b)
{
	atomic_long_inc(&core->events[ev]);
	obmf_report_log(dev, ev, ch, a, b, NULL, NULL);
}

static inline void obmf_ch_count(struct obmf_channel *ch,
				 enum obmf_ch_counter c)
{
	atomic_long_inc(&ch->cnt[c]);
}

/* The line a bound device logs, NUL included */
#define OBMF_INFO_LEN 256

/* obmf-core.c */
struct obmf_core *obmf_core_create(struct obmf_transport *tp);
int obmf_core_bind(struct obmf_core *core);
void obmf_core_info(struct obmf_core *core, char *buf);
void obmf_core_unbind(struct obmf_core *core, bool attached);
void obmf_core_rx(struct obmf_core *core, const u8 *buf, size_t len);
void obmf_core_fatal(struct obmf_core *core, int err);
int obmf_core_quiesce(struct obmf_core *core);
int obmf_core_resume(struct obmf_core *core);
int obmf_core_state(struct obmf_core *core);

/*
 * Discovery runs before the core is live and takes timeouts, malformed
 * replies and strays as leftovers of an earlier binding; its own failure
 * is logged. Response and message events stay quiet until then.
 */
static inline bool obmf_core_discovering(struct obmf_core *core)
{
	return READ_ONCE(core->state) == OBMF_INIT;
}

struct obmf_channel *obmf_ch_alloc(struct obmf_core *core, u8 number,
				   size_t max_req, size_t max_resp,
				   size_t rx_cap);
void obmf_ch_free(struct obmf_channel *ch);
DEFINE_FREE(obmf_ch, struct obmf_channel *, obmf_ch_free(_T))
int obmf_ch_request(struct obmf_channel *ch, u8 cmd, const void *req,
		    size_t req_len, void *resp, size_t resp_cap,
		    size_t *resp_len, unsigned int timeout_ms);
void obmf_ch_answer_leftovers(struct obmf_channel *ch);
struct obmf_core *obmf_inflight_enter(struct obmf_core *core);
void obmf_inflight_exit(struct obmf_core *core);
int obmf_slot_xfer(struct obmf_channel *ch, u8 cmd, size_t len,
		   unsigned int timeout_ms);
bool obmf_dreq_len_ok(const struct obmf_channel *ch, const u8 *data,
		      size_t len);
bool obmf_dreq_add(struct obmf_channel *ch, const u8 *data, size_t len,
		   bool *due);
void obmf_dreq_full(struct obmf_channel *ch, bool due);
void obmf_dreq_owe_invalid(struct obmf_channel *ch);

/* A call from a function driver, waited for by obmf_core_stop() */
DEFINE_CLASS(obmf_inflight, struct obmf_core *, obmf_inflight_exit(_T),
	     obmf_inflight_enter(core), struct obmf_core *core)

/* obmf-ctrl.c */
void obmf_copy_name(char *dst, const u8 *src, size_t n);
int obmf_ctrl_read(struct obmf_core *core, u32 addr, void *buf, size_t len);
int obmf_ctrl_write(struct obmf_core *core, u32 addr, const void *buf,
		    size_t len);
int obmf_ctrl_discover(struct obmf_core *core);
int obmf_ch_status_read(struct obmf_fdev *fdev, u8 *status, u8 *specific);
int obmf_ch_control_update(struct obmf_fdev *fdev, u8 specific_mask,
			   u8 specific_val);

/* obmf-bus.c */
void obmf_bus_publish(struct obmf_core *core);
void obmf_bus_add(struct obmf_core *core);
void obmf_bus_del(struct obmf_core *core);
void obmf_debugfs_add(struct obmf_core *core);
void obmf_debugfs_del(struct obmf_core *core);
void obmf_debugfs_init(struct dentry *parent);
void obmf_debugfs_exit(void);
extern const struct attribute_group *obmf_intf_groups[];

#endif /* __OBMF_CORE_H */
