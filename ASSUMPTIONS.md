# Assumptions

Where the specifications leave a point open, this driver takes one
reading until the point is confirmed; each item says what is open, the
readings, what this driver does, what your device needs to work with
it, and what a device that reads it otherwise sees. The numbers are
stable: please quote them in reports, and tell us if your device takes
another reading.

## AS-1: one outstanding request per channel, channels in parallel

- Specification: OBMF-ICP over USB 1.0, 6 (p14), allows "only one
  outstanding OBMF-ICP Message that requires response" without saying
  whether per channel or per interface; OBMF-ICP 1.0.0 Table 1 #15
  (p12) says "one outstanding request per communication channel".
- Options: one request per channel, channels in parallel; or one
  request per interface, all channels serialised.
- This driver: one request in flight on each channel, and requests on
  different channels at the same time. A response is matched to its
  request by the channel number.
- Prerequisites: accept a request on one channel while a request on
  another channel is still unanswered, and answer each on its own
  channel, in any order.
- If your device differs: a device that holds one request per
  interface gets a second one while busy. Answering it busy gives the
  caller `-EBUSY`; dropping it gives `-ETIMEDOUT` after the response
  timeout.
- Closes when: the specification confirms the scope.

## AS-2: whether the size limits count the 4-byte header

- Specification: `wMaxWrTransferSize` and `wMaxRdTransferSize`
  (OBMF-ICP over USB 1.0, 5.2, p11) and `MAX_REQUEST_PAYLOAD_SIZE` and
  `MAX_RESPONSE_PAYLOAD_SIZE` (OBMF-ICP 1.0.0, Table 65, p65) give the
  largest transfer, request and response, but not whether the 4-byte
  message header is counted.
- Options: the header is counted; only the payload is counted.
- This driver: works under both. It sends messages of at most
  `wMaxWrTransferSize` bytes and request payloads of at most
  `MAX_REQUEST_PAYLOAD_SIZE` - 4, header included in the limit. It
  accepts up to 4 bytes more than the limits in the other direction:
  each bulk IN buffer is `wMaxRdTransferSize` + 4 bytes.
- Prerequisites: transfer sizes of at least 64 bytes; channel limits
  with room for the header and the channel's fixed fields; a
  zero-length packet after a message whose length is a multiple of the
  bulk IN packet size (OBMF-ICP over USB 1.0, 6.1).
- If your device differs: a device that sends no zero-length packet
  after a message of exactly `wMaxRdTransferSize` bytes has it merged
  with the next message; both are dropped and counted in
  `rx_malformed` (KI-4). A device that takes 4 bytes more than its
  limits gets slightly smaller transfers than it could; nothing fails.
- Closes when: the specification settles it; the buffer is then sized
  exactly.

## AS-3: a serial NACK is status 0x00 with the NACK bit set

- Specification: OBMF-ICP 1.0.0, 5.5.2, Table 23 (p35), defines the
  NACK bit and the accepted character count of a serial response, but
  not the header status that goes with a NACK.
- Options: status 0x00 with the NACK bit; or an error status.
- This driver: a response with status 0x00 and the NACK bit gives the
  accepted count, and the rest is sent again. A NACK that accepts
  nothing is retried up to 20 times, 10 ms apart; after that, or on any
  error status, the characters in flight are dropped with the warning
  "dropped <n> bytes: <errno>". When the host has no room for a device
  write, it answers status 0x00 with the NACK bit and a count of 0.
- Prerequisites: refuse a host write, fully or partly, with status
  0x00, bit 0 of byte 0 set and the count accepted; accept some
  characters within about 200 ms of refusing all; use error statuses
  only for real failures; send a refused device write again.
- If your device differs: a NACK sent as an error status loses the
  characters at once ("dropped <n> bytes: -16" for status 0x03).
- Closes when: the specification names the status.

## AS-4: a device does not answer busy during discovery

- Specification: OBMF-ICP 1.0.0, Table 4 (p26), has 0x03 "Channel not
  ready / busy", and LSTP 1.0, 3.1.1 (p8), `LSTP_BUSY` "retry later";
  neither says whether channel 0 may be busy right after enumeration,
  for how long, or whether the host retries.
- Options: no busy during discovery; busy with a bounded host retry;
  busy leaving the channel out.
- This driver: on OBMF-ICP, a busy answer to a channel 0 read fails
  the bind with "discovery failed: -16". On LSTP, a busy device header
  is retried for a few discovery passes and then fails the bind with
  "device busy"; a busy answer to a channel's configuration read leaves
  that channel out with "channel <n>: status 0x3, ignored".
- Prerequisites: answer channel 0 with status 0x00 from the first
  request after the interface is configured; if start-up takes time,
  connect to the bus later rather than answer busy.
- If your device differs: it binds without channels, or without the
  busy channel, until the interface is bound again or the device is
  replugged (KI-14).
- Closes when: the specification answers it; with a bounded retry if
  busy is allowed.

## AS-5: no response arrives after the host's timeout

- Specification: OBMF-ICP responses carry no tag (OBMF-ICP 1.0.0,
  Table 3, p26), and `CHANNEL_RESET` (Table 65, p64) is not said to
  discard a response the device still owes.
- Options: accept a late response as a known limit; reset the channel
  after each timeout and wait for `CHANNEL_RESET_DONE`; a flush message
  or a tag in a later specification.
- This driver: a request that gets no response in time fails with
  `-ETIMEDOUT` and is counted in `timeouts`; the channel is not reset.
  The default response timeout is 1000 ms; a serial write adds twice
  its time on the line. A response that arrives with no request waiting
  is counted in `rx_late` and dropped; one that arrives while the next
  request waits is taken as its answer.
- Prerequisites: answer every request within the timeout; if a
  request cannot finish in time, answer it with an error status (for
  example 0x02) before the timeout rather than late; send no response
  to a request after the timeout.
- If your device differs: the next request on that channel gets the
  late answer: on I2C a false ACK, or data read from the earlier
  address (KI-1).
- Closes when: the specification says what a channel reset does to a
  pending response, or adds a flush or a tag.

## AS-6: after a stall the message fails, is not resent, and the device is not reset

- Specification: OBMF-ICP over USB 1.0, 8.1 (p17), has the device
  "silently discard" a message whose transfer failed; 8.2 (p17-18)
  lists `CLEAR_FEATURE(ENDPOINT_HALT)`, then a port or bus reset,
  "typically executed automatically by the USB Host stack". Neither
  says whether the host sends the message again or whether the class
  driver resets the device.
- Options: fail the message, no reset; resend it once; reset the device
  after repeated stalls.
- This driver: on a bulk OUT stall it clears the halt and fails the
  message with `-EPIPE` (`-EIO` on I2C); a message is never sent twice.
  On a bulk IN stall it clears the halt and goes on reading. It resets
  neither the device nor the port; only the USB core does that, on its
  own errors.
- Prerequisites: after `CLEAR_FEATURE(ENDPOINT_HALT)` on either bulk
  endpoint, take and answer new messages with no reset (8.2 step 1);
  treat the stalled message as gone.
- If your device differs: a device that expects a resend loses the
  request; a device that needs a reset after a stall stays stopped
  until it is re-enumerated (KI-11).
- Closes when: the specification answers both points.

## AS-7: major version 1 in both version fields, a newer minor accepted

- Specification: OBMF-ICP 1.0.0, Table 1 #2 (p12), has the host reject
  a version it does not support; the version is in `OBMF_VER` (Table
  64, p62) and in `bcdOCPOBMFVersion` (OBMF-ICP over USB 1.0, 5.2,
  p11). Neither text says which field counts if they differ, or how a
  1.0 host treats a later 1.x device.
- Options: major version in both, any minor; an exact 1.0 match; one
  field only.
- This driver: requires major version 1 in both fields and accepts any
  minor version and a class descriptor longer than 14 bytes. It
  refuses a nonzero `bMultimessageSupport`, a value 1.0 reserves.
- Prerequisites: the same version in both fields, major 1;
  `bMultimessageSupport` 0x00; a class descriptor of at least 14
  bytes.
- If your device differs: it is not bound: "unsupported OBMF-ICP
  version <major>.<minor>" or "multimessage transfers not supported".
- Closes when: the specification answers both points.

## AS-8: on a serial channel both ends may have a request outstanding

- Specification: OBMF-ICP 1.0.0, Table 20 (p34), makes both ends of a
  serial channel requesters, and Table 1 #15 (p12) allows one
  outstanding request per channel without saying per direction. LSTP
  UART (LSTP 1.0, 4.5.2, p26) has the same pattern.
- Options: one request per direction; one per channel in total.
- This driver: answers a device write while its own write to the
  device may be in flight; the two are independent.
- Prerequisites: accept and answer a host write while the device's own
  write is still unanswered; tell the host's answer from the host's
  request by the request/response bit.
- If your device differs: a host write it answers busy or drops loses
  its characters ("dropped <n> bytes").
- Closes when: the specification confirms the count.

## AS-9: the host only reads channel configuration

- Specification: OBMF-ICP 1.0.0, 5.15 (p60), describes writing and
  applying runtime configuration, but not whether a host driver is
  meant to.
- Options: read only; the host writes and applies configuration, for
  example from serial line settings.
- This driver: reads each channel's configuration at discovery and
  never writes `CONFIGURATION_DATA` or `APPLY_CONFIGURATION`; its only
  channel 0 write is the channel-specific control byte (it never
  writes the enable bit either, see `REQUIREMENTS.md`). A serial port
  keeps the device's speed and framing.
- Prerequisites: provision each channel's configuration on the device
  (serial speed, data bits, stop bits and parity; I2C speed).
- If your device differs: a device that waits to be configured runs
  with its own settings; `stty` changes are not kept (KI-6).
- Closes when: the specification answers it; then a write path.

## AS-10: an LSTP UART NACK is LSTP_NACK (0x04) only

- Specification: LSTP 1.0, 4.5.2 (p27): "Status field in the header is
  used to indicate ACK/NACK", with no value given; 3.1.1 (p8) names
  0x04 `LSTP_NACK` "LSTP link error or not established".
- Options: 0x04 only; any nonzero status; 0x04 and 0x03.
- This driver: retries a write the device answers with 0x04, as in
  AS-3; any other nonzero status drops the characters. It answers a
  device write it cannot take with 0x04.
- Prerequisites: refuse a host write with 0x04; read 0x04 from the
  host as "send again".
- If your device differs: a refusal with another status loses the
  characters ("dropped <n> bytes: <errno>").
- Closes when: the specification names the value.

## AS-11: LSTP_TIMEOUT and LSTP_BUSY on I2C are the generic timeout and busy

- Specification: LSTP 1.0, 3.1.1 (p8), has one status set for all
  channels; on I2C, 0x02 "Command timed out" and 0x03 "LSTP link busy,
  retry later" can mean the OBMF-ICP I2C codes 0x40 and 0x42 (Table 25,
  p37) or the generic 0x02 and 0x03 (Table 4, p26).
- Options: generic codes; I2C codes.
- This driver: maps them to the generic codes. The caller gets
  `-ETIMEDOUT` and `-EBUSY` either way, and either starts one bus
  recovery.
- Prerequisites: none beyond LSTP 1.0 today.
- If your device differs: nothing changes today. If a later version
  starts recovery only on a bus timeout (KI-3), a device that means a
  bus timeout by `LSTP_TIMEOUT` gets no recovery.
- Closes when: the specification answers it.

## AS-12: DEVICE_ROLE 2 is symmetric, 3-255 reserved

- Specification: OBMF-ICP 1.0.0, Table 65 (p64), lists "2 -
  Symmetric" and then "2-255 - Reserved".
- Options: 2 is symmetric and 3-255 reserved; or 2 is reserved.
- This driver: gives a serial port to serial channels with role 0 or
  2, and an I2C adapter to I2C controller channels with role 0.
- Prerequisites: report role 0, or 2 for a symmetric serial channel;
  not 3-255.
- If your device differs: a channel with another role is discovered
  but gets no device.
- Closes when: the specification corrects the table.

## AS-13: over USB the host does not answer a GPIO Interrupt Notify Datagram

- Specification: OBMF-ICP 1.0.0, 5.4.1 (p32): a Datagram needs no
  response "if binding allows for that"; OBMF-ICP over USB 1.0 is
  silent on it.
- Options: no answer to a Datagram; answer it like an Interrupt
  Notify.
- This driver: with no GPIO driver yet (KI-9), the core answers an
  Interrupt Notify (command 4) with status 0x00 on bulk OUT and sends
  nothing for a Datagram (command 5).
- Prerequisites: expect no answer to command 5; send GPIO events on
  bulk IN, since this driver reads no interrupt IN endpoint.
- If your device differs: a device that waits for an answer to a
  Datagram sends no further GPIO event.
- Closes when: the specification answers it.

## AS-14: a device sends no device requests until discovery is done

- Specification: OBMF-ICP 1.0.0, 4.3 (p22), step 2a has the host
  enable a consumed channel to tell the device it may use it; step 3
  has channels enabled from manufacturing and says only when the host
  starts using them. This driver never writes the enable bit
  (`REQUIREMENTS.md`), so nothing tells the device the host is ready.
- Options: no device requests before the host has read channel 0;
  the host answers early ones busy (0x03) so the device retries; the
  host keeps early ones and delivers them once the channel is set up.
- This driver: drops a device request that arrives before discovery
  has finished, without an answer, and counts it in `rx_no_channel`
  (debugfs). If discovery has to read channel 0 a second time, it
  waits while such messages keep arriving, up to its 30 s limit.
- Prerequisites: send no device request (Notify, Datagram, serial
  data) on a channel until the host has sent its first request there
  or has answered one; resend a device request that gets no answer
  right after enumeration.
- If your device differs: its first device requests get no answer,
  and a repeated discovery takes longer.
- Closes when: the specification says when a device may start using
  a channel enabled from manufacturing.

## AS-15: discovery does not gate a channel on HEALTH_STATUS

- Specification: OBMF-ICP 1.0.0, 5.15 (p59-60), judges an applied
  configuration by CONFIGURATION_VALID together with HEALTH_STATUS
  (Table 65, p64: 0 unknown, 1 OK, 2 disabled, 3 not ready or
  configuration error, 15 channel error). It does not say whether a
  host that only discovers channels, and applies no configuration,
  must also require HEALTH_STATUS 1 before using one.
- Options: use a channel when CHANNEL_ENABLE and CONFIGURATION_VALID
  are set; also require HEALTH_STATUS 1; also leave out a channel
  with CHANNEL_ERROR_INDICATOR set or HEALTH_STATUS 15.
- This driver: uses every channel with CHANNEL_ENABLE and
  CONFIGURATION_VALID set, and does not read HEALTH_STATUS or
  CHANNEL_ERROR_INDICATOR. HEALTH_STATUS sums up several conditions,
  some of which a host cannot act on at discovery, so it is not used
  to leave a channel out.
- Prerequisites: clear CHANNEL_ENABLE or CONFIGURATION_VALID on a
  channel the host must not use; while an enabled, valid channel is
  not ready, answer its requests with the channel type's status codes.
- If your device differs: the host binds a channel the device reports
  unhealthy; its requests fail with the device's status codes or time
  out, and the kernel log does not mention the health field.
- Closes when: the specification says whether a discovering host must
  check HEALTH_STATUS or CHANNEL_ERROR_INDICATOR.
