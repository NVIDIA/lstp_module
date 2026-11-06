/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * OBMF-ICP over USB: the edge translation of a device that speaks another
 * wire protocol, and the core's calls into it
 */

#ifndef __OBMF_DIALECT_H
#define __OBMF_DIALECT_H

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/types.h>

#include "obmf-core.h"

/**
 * struct obmf_dialect - edge translation for a device that speaks another
 *	wire protocol; the core works in OBMF-ICP form
 * @name: the protocol's name in the line a bound device logs, followed
 *	by version_major in decimal
 * @encode: rewrite the OBMF-ICP message of @len bytes in @buf into the
 *	wire form within @cap; returns the new length, 0 to drop an answer,
 *	or an errno that fails a host request before it is sent
 * @decode: rebuild the OBMF-ICP form of the wire message @in in @out;
 *	@pending_cmd is the request's payload byte 0 for a response; atomic;
 *	returns the decoded length, header included, or an errno
 * @discover: one discovery pass, filling @found by channel number as the
 *	native pass does; the caller frees @found after a failed pass.
 *	With @last set no pass follows, so a fault the caller would retry
 *	is final
 * @status_to_errno: the errno for a wire status with no OBMF-ICP code,
 *	or 0 to map the decoded status
 * @ch_reg: read (@mask 0, into @old) or update a channel header byte
 *	@reg; process context, under ch0_lock
 * @ch0_whole_msgs: channel 0 takes whole wire messages, not OBMF-ICP
 *	chunks; a dialect reads them with obmf_ch_xfer_wire()
 * @keep_ch_limits: the channel limits are the device's, in OBMF-ICP
 *	form, and the transport's do not cut them; needs @encode, which
 *	fits such a message to the link
 *
 * The wire header has the OBMF-ICP layout, so the core routes a message
 * by its channel and request/response bit before any op sees it.
 *
 * Every member is optional, but @encode and @decode come as a pair:
 * obmf_dialect_attach() fails the probe on a table with only one of
 * them, with @keep_ch_limits but no @encode, or with nothing but @name
 * set. Where the core would take a native step that a member replaces,
 * it tests that member; only obmf-dialect.c calls the ops:
 *
 * - @name: obmf_core_info(); without it the line names OBMF-ICP and
 *   its version;
 * - @encode: obmf_ch_request() and obmf_send_answer(), through
 *   obmf_dialect_encode_request() and obmf_dialect_encode_answer();
 *   without it the message goes out as it is;
 * - @decode: obmf_core_rx() gives every message to obmf_dialect_rx(),
 *   and obmf_ch_alloc() adds the decode buffers; without it the message
 *   is routed as received;
 * - @discover: obmf_ctrl_discover(), through obmf_dialect_discover();
 *   without it the pass reads the channel headers through channel 0;
 * - @status_to_errno: obmf_reply(), through obmf_dialect_status_errno(),
 *   on the status before decode, or as received without @decode;
 *   without it the status maps through the OBMF-ICP status tables;
 * - @ch_reg: obmf_ch_status_read() and obmf_ch_control_update(), through
 *   their obmf_dialect_*() versions; without it the bytes are read and
 *   written through channel 0;
 * - @ch0_whole_msgs: obmf_core_create(); without it channel 0 takes
 *   chunks of at most 64 bytes, and responses of a chunk and its header;
 * - @keep_ch_limits: obmf_ch_alloc(); without it the transport's
 *   message sizes cut the limits.
 *
 * rx_cap is what the slot and the device request records hold: a
 * decoded payload, or a whole wire message, header included. dec_cap is
 * one header more, the decoded form's own.
 *
 * The type is complete in every configuration, so that a test of a
 * member compiles where no dialect is built.
 */
struct obmf_dialect {
	const char *name;
	ssize_t (*encode)(struct obmf_channel *ch, u8 *buf, size_t len,
			  size_t cap);
	ssize_t (*decode)(struct obmf_channel *ch, const u8 *in, size_t len,
			  u8 *out, size_t cap, u8 pending_cmd,
			  unsigned long *levels);
	int (*discover)(struct obmf_core *core, struct obmf_channel **found,
			bool last);
	int (*status_to_errno)(const struct obmf_channel *ch, u8 raw);
	int (*ch_reg)(struct obmf_channel *ch, unsigned int reg, u8 mask,
		      u8 val, u8 *old);
	bool ch0_whole_msgs;
	bool keep_ch_limits;
};

#ifdef CONFIG_OBMF_DIALECTS
/* The device's dialect, NULL for an OBMF-ICP device */
static inline const struct obmf_dialect *
obmf_dialect(const struct obmf_core *core)
{
	return core->tp->dialect;
}

/* obmf-dialect.c */
int obmf_dialect_attach(struct obmf_transport *tp,
			const struct obmf_usb_proto *proto);
int obmf_dialect_ch_alloc(struct obmf_channel *ch, size_t rx_cap);
void obmf_dialect_ch_free(struct obmf_channel *ch);
int obmf_dialect_status_errno(struct obmf_channel *ch);
int obmf_dialect_encode_request(struct obmf_channel *ch, size_t *len);
int obmf_dialect_encode_answer(struct obmf_channel *ch, u8 *msg, size_t *len);
int obmf_ch_xfer_wire(struct obmf_channel *ch, const void *msg, size_t len,
		      void *reply, size_t cap, size_t *reply_len,
		      unsigned int timeout_ms);
void obmf_dialect_rx(struct obmf_channel *ch, const u8 *msg, size_t len);
int obmf_dialect_discover(struct obmf_core *core, struct obmf_channel **found,
			  bool last);
int obmf_dialect_control_update(struct obmf_channel *ch, u8 specific_mask,
				u8 specific_val);
int obmf_dialect_status_read(struct obmf_channel *ch, u8 *status, u8 *specific);
#else /* CONFIG_OBMF_DIALECTS */
static inline const struct obmf_dialect *
obmf_dialect(const struct obmf_core *core)
{
	return NULL;
}

static inline int obmf_dialect_attach(struct obmf_transport *tp,
				      const struct obmf_usb_proto *proto)
{
	return 0;
}

static inline int obmf_dialect_ch_alloc(struct obmf_channel *ch, size_t rx_cap)
{
	/* A constant, so the compiler drops every branch on a member */
	BUILD_BUG_ON(obmf_dialect(ch->core));
	return 0;
}

static inline void obmf_dialect_ch_free(struct obmf_channel *ch)
{
}

static inline int obmf_dialect_status_errno(struct obmf_channel *ch)
{
	return 0;
}

static inline int obmf_dialect_encode_request(struct obmf_channel *ch,
					      size_t *len)
{
	return 0;
}

static inline int obmf_dialect_encode_answer(struct obmf_channel *ch, u8 *msg,
					     size_t *len)
{
	return 0;
}

/* The rest replace native steps, under a test of their op */
static inline void obmf_dialect_rx(struct obmf_channel *ch, const u8 *msg,
				   size_t len)
{
}

static inline int obmf_dialect_discover(struct obmf_core *core,
					struct obmf_channel **found, bool last)
{
	return -EOPNOTSUPP;
}

static inline int obmf_dialect_control_update(struct obmf_channel *ch,
					      u8 specific_mask, u8 specific_val)
{
	return -EOPNOTSUPP;
}

static inline int obmf_dialect_status_read(struct obmf_channel *ch, u8 *status,
					   u8 *specific)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_OBMF_DIALECTS */

#endif /* __OBMF_DIALECT_H */
