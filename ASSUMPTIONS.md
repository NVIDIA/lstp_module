# Assumptions

Where the specifications leave a point open, this driver takes one
reading until the point is confirmed; each item says what is open, the
readings, what this driver does, what your device needs to work with
it, and what a device that reads it otherwise sees. The numbers are
stable: please quote them in reports, and tell us if your device takes
another reading. Gaps in the numbering are items already closed, or
items about code or tools that are not in this drop.

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
  accepted count, and the rest is sent again, from the first character
  not accepted. A NACK that accepts nothing, a busy answer (status
  0x03, AS-49) or a timeout keeps the characters, and they are sent
  again 10 ms later, with no limit, until the device takes them or the
  port is flushed, hung up or closed; after 5 s one "transmit stalled"
  line is logged. Any other error status drops the characters in
  flight with the warning "dropped <n> bytes: <errno>". A refused
  Generate Break is sent again up to 20 times, 10 ms apart, and then
  fails with `-EIO`. When the host cannot take a device write (its
  port is closed and the write carries characters, or its buffer is
  full), it answers status 0x00 with the NACK bit and a count of 0.
- Prerequisites: refuse a host write, fully or partly, with status
  0x00, bit 0 of byte 0 set and the count accepted; use error
  statuses other than busy only for real failures; send a refused
  device write again.
- If your device differs: a NACK sent as an error status other than
  busy or a timeout loses the characters at once ("dropped <n> bytes:
  <errno>"). A device that expects an error status from the host
  takes the host's NACK (status 0x00) as success with a count of 0.
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
  "device busy"; a busy answer to any read of a channel's
  configuration leaves that channel out with "channel <n>: status 0x3,
  ignored".
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
  request waits is taken as its answer. During a software unbind, the
  first timeout stops the device instead.
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
- If your device differs: a host write it answers busy, or does not
  answer, is kept and sent again 10 ms later until the device takes
  it, so the host's transmit stalls while the device's own write is
  pending ("transmit stalled" after 5 s); a device that took the
  characters before it dropped the request gets them twice (AS-48).
- Closes when: the specification confirms the count.

## AS-9: the host only reads channel configuration

- Specification: OBMF-ICP 1.0.0, 5.15 (p60), describes writing and
  applying runtime configuration, but not whether a host driver is
  meant to.
- Options: read only; the host writes and applies configuration, for
  example from serial line settings.
- This driver: reads each channel's configuration at discovery and
  never writes `CONFIGURATION_DATA` or `APPLY_CONFIGURATION`; it makes
  no channel 0 write at all (it never writes the enable bit either,
  see `REQUIREMENTS.md`). A serial port keeps the device's speed and
  framing.
- Prerequisites: provision each channel's configuration on the device
  (serial speed, data bits, stop bits and parity; I2C speed).
- If your device differs: a device that waits to be configured runs
  with its own settings; `stty` changes are not kept (KI-6).
- Closes when: the specification answers it; then a write path.

## AS-10: an LSTP UART NACK is LSTP_NACK (0x04) only

- Specification: LSTP 1.0, 4.5.2 (p27): "Status field in the header is
  used to indicate ACK/NACK", with no value given; 3.1.1 (p8) names
  0x04 `LSTP_NACK` "LSTP link error or not established" and 0x06
  `LSTP_NOT_SUPP` "Device does not support this command or option".
- Options: 0x04 only; any nonzero status; 0x04 and 0x03.
- This driver: keeps the characters of a write the device answers
  with 0x04, busy 0x03 or timeout 0x02, and sends them again 10 ms
  later, as in AS-3; any other nonzero status drops them. It answers a
  device write it refuses (its port is closed, or its buffer is full)
  with 0x04. A device write it cannot decode, an unknown UART command
  for example, is answered with 0x06, so that the device does not send
  it again.
- Prerequisites: refuse a host write with 0x04; read 0x04 from the
  host as "send again later", and 0x06 as "do not send this again".
- If your device differs: a refusal sent as another status loses the
  characters ("dropped <n> bytes: <errno>"). A device that sends a
  write again after 0x06 gets 0x06 again.
- Closes when: the specification names the value.

## AS-11: LSTP_TIMEOUT and LSTP_BUSY on I2C are the generic timeout and busy

- Specification: LSTP 1.0, 3.1.1 (p8), has one status set for all
  channels; on I2C, 0x02 "Command timed out" and 0x03 "LSTP link busy,
  retry later" can mean the OBMF-ICP I2C codes 0x40 and 0x42 (Table 25,
  p37) or the generic 0x02 and 0x03 (Table 4, p26).
- Options: generic codes; I2C codes.
- This driver: maps them to the generic codes. The caller gets
  `-ETIMEDOUT` and `-EBUSY` either way; the adapter has no bus
  recovery, and the client decides whether to retry.
- Prerequisites: none beyond LSTP 1.0 today.
- If your device differs: nothing changes today: both readings give
  the caller the same error.
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

## AS-18: every LSTP I2C channel implements BUS_RECOVERY, with no side effect

- Specification: LSTP 1.0, 4.4.2, lists BUS_RECOVERY (command 0x00)
  among the I2C commands, with no further text; the I2C configuration
  of 4.4.1 has no field that says whether a channel implements it.
- Options: offer recovery on every LSTP I2C channel; never offer it
  for LSTP; offer it from a firmware version or capability bit, which
  LSTP 1.0 does not define.
- This driver: marks every LSTP I2C channel as offering recovery, but
  `i2c-obmf.ko` registers no bus recovery with the I2C core, so
  nothing sends BUS_RECOVERY: no status says the bus is held, and a
  recovery sent after a timeout could take the late response as its
  own. A caller of `i2c_recover_bus()` gets `-EBUSY`.
- Prerequisites: none in this version.
- If your device differs: nothing changes in this version. A bus held
  low stays held until the device clears it or is reset or replugged.
- Closes when: the specification says whether BUS_RECOVERY is
  mandatory and free of side effects, or adds a capability bit.

## AS-19: a nonzero LSTP Bus Arbitration Retries means the device retries

- Specification: LSTP 1.0, 4.4.1, has a Bus Arbitration Retries byte
  (0x00 "No Multimaster Support", 0x01 "1 Retry", 0xFF "256 Retries"),
  but does not say who retries a lost arbitration, or whether
  `LSTP_ARB_LOST` comes only after the device's own retries.
- Options: the device retries when the byte is nonzero, the host does
  not; the host always retries; the host retries the configured count.
- This driver: with a nonzero byte the I2C adapter does no retries of
  its own; with 0, or a configuration too short to hold the byte, the
  I2C core retries a lost arbitration up to three times.
- Prerequisites: set the byte to 0 on a channel whose firmware does
  not retry a lost arbitration; with a nonzero value, retry before
  answering `LSTP_ARB_LOST`.
- If your device differs: a device that sets the byte but does not
  retry gets no retry at all; the client sees `-EAGAIN` at the first
  lost arbitration. Only multimaster buses see it.
- Closes when: the specification says who retries.

## AS-20: an OBMF-ICP I2C bus recovery posts its result within 100 ms

- Specification: OBMF-ICP 1.0.0, 5.15.1, Table 69 (p67), defines
  TRIGGER_BUS_RECOVERY and the sticky BUS_RECOVERY_SUCCESS and
  BUS_RECOVERY_FAILED bits, but no time from the trigger to either.
- Options: poll for 100 ms; a longer window; also poll after a lost
  trigger response.
- This driver: registers no bus recovery, even on a channel that
  offers one (5.15.2, Table 79, options bit 0), and never writes
  TRIGGER_BUS_RECOVERY; a caller of `i2c_recover_bus()` gets
  `-EBUSY`. (Drop 1 triggered a recovery and polled for its result
  for 100 ms.)
- Prerequisites: none in this version.
- If your device differs: nothing changes in this version.
- Closes when: the specification bounds the time, and a case where a
  recovery is due is defined.

## AS-22: a device whose channel configuration changes re-enumerates

- Specification: OBMF-ICP 1.0.0, 5.15 (p59-60), describes the
  configuration the host reads at discovery, but not whether the
  channel table may change while the USB device stays enumerated.
- Options: read the table once at bind, a change needs re-enumeration;
  read it again after a device-side reset is seen.
- This driver: reads the channel table once, when the interface is
  bound, and keeps it until it is unbound. It has no USB reset
  handling of its own, so a USB reset unbinds and binds the interface,
  and the table is read again.
- Prerequisites: re-enumerate, or reset the USB device, whenever the
  channel configuration changes.
- If your device differs: requests go to channels that changed type or
  limits; they fail with device errors or time out, and the function
  drivers stay bound to channels that no longer match.
- Closes when: the specification says what may change while the
  device stays enumerated.

## AS-24: an LSTP device ends configuration slices short a bounded number of times

- Specification: LSTP 1.0, 4.3.1.4, has a GPIO table of up to 255
  blocks of a count byte and 48-byte entries, and 4.1.2 lets a
  READ_CONFIG return "as much data as is available"; nothing bounds
  how short a device may make each slice.
- Options: bound the short slices of a channel and of a discovery
  pass; bound only the bytes read; also allow a slice per field
  boundary.
- This driver: reads a GPIO table until an empty slice. A channel may
  have at most 683 short slices (one 48-byte entry per slice over its
  32 KiB space), and a discovery pass at most 5461 (its 256 KiB budget
  over 48); full slices do not count. Past the channel's bound that
  channel is left out, and past the pass's bound every later channel
  of the pass too, each with "channel <n>: over budget, ignored".
- Prerequisites: send a GPIO table in full slices, or in slices that
  average at least 48 bytes.
- If your device differs: a long table, or the later channels of the
  pass, are left out with that warning.
- Closes when: the specification bounds the slicing, or the slice
  pattern of each device is known.

## AS-25: a device keeps its serial bytes under a NACK that lasts until the port opens

- Specification: OBMF-ICP 1.0.0, 5.5.2, Table 23, defines the NACK of
  a serial write, but neither specification says what a device does
  with a NACK that lasts minutes or hours, as for a console nobody has
  opened.
- Options: refuse device writes with characters while the port is
  closed; take and drop them; keep them in the tty buffer for the next
  open.
- This driver: while the port is closed, a device write with
  characters gets a NACK with a count of 0; one with events only is
  taken. On an open port a write gets the same NACK when the buffer
  has no room for its characters and the marks of its events. A
  refused write is not acted on at all: its events count, and Carrier
  Down hangs up, only when the write is sent again and taken. The host
  keeps the characters the device refuses in the same way: until the
  device takes them, or the port is flushed, hung up or closed (AS-3).
- Prerequisites: keep a refused write and send it again, with its
  events; retry no faster than every few milliseconds, and keep the
  other channels going between retries; do not reset the channel
  because of the refusals.
- If your device differs: a device that retries without a delay fills
  the bus while a port is closed; one that drops a refused write loses
  that input (KI-20); one that resets the channel or stops its other
  channels makes a closed port cost more than lost output.
- Closes when: the device's retry pattern under a long NACK is known.

## AS-26: a device reports a nonzero serial line speed

- Specification: OBMF-ICP 1.0.0, 5.15.2, Table 78, gives the line
  speed and framing of a serial channel; it does not say whether 0 or
  an out-of-range field may appear.
- Options: refuse a speed of 0; use a default speed for it.
- This driver: refuses a channel whose speed is 0, with "line speed
  0", because the wait for each write is computed from it. Data bits,
  stop bits or parity out of range give 8N1, with a warning.
- Prerequisites: report the real line speed of every serial channel.
- If your device differs: that channel gets no tty.
- Closes when: the specification requires a nonzero speed.

## AS-27: the serial "transmit overrun" event means characters lost on their way to the host

- Specification: OBMF-ICP 1.0.0, 5.5.1, Table 22, bit 2, "Transmit
  overrun (typically remote Serial/UART interface to BMC)". It does
  not say whether the lost characters were bound for the host or for
  the device's remote line.
- Options: a loss mark in the host's receive stream and a count; a
  count only.
- This driver: inserts a `TTY_OVERRUN` mark where the event arrives,
  and counts it in `overrun` of `TIOCGICOUNT`.
- Prerequisites: set the bit only when characters on their way to the
  host were lost.
- If your device differs: a reader with `PARMRK` sees a loss mark
  where no character bound for the host was lost; the count is still
  right as a count of events.
- Closes when: the specification says which direction the bit
  describes.

## AS-28: the device chooses the length of a Generate Break

- Specification: OBMF-ICP 1.0.0, 5.5.1, Table 22, bit 0, Generate
  Break, has no length and no "break off" form.
- Options: one request per "break on"; a request per "on" and per
  "off", which has no form in Table 22.
- This driver: sends one Generate Break when the tty layer turns a
  break on, and nothing when it turns it off. A refused break is sent
  again, up to 20 times 10 ms apart, then fails with `-EIO`.
- Prerequisites: generate a complete break for each request.
- If your device differs: a device that waits for an "off" holds the
  line in break until the next request; `tcsendbreak()` lengths are
  not honoured.
- Closes when: the specification defines the break length.

## AS-29: a device answers a serial write within the default timeout plus its wire time

- Specification: OBMF-ICP 1.0.0, 5.5.2, Table 23, gives the answer to
  a serial write, but no time for it, for example while the device's
  own flow control holds its line.
- Options: a wait bounded by the default timeout and the characters'
  time on the wire, then drop the characters (drop 1); the same wait,
  then send them again (AS-48); no bound while the device's line is
  stopped.
- This driver: waits the default response timeout (1000 ms) plus twice
  the characters' time on the wire at ten bits a character, and cuts
  each request so that the wait fits 10 s. A write that gets no answer
  in time is kept and sent again 10 ms later, with one "transmit
  stalled" line after 5 s. On LSTP, a device write the host cannot
  decode, too large or with an unknown command, is answered 0x06
  (`LSTP_NOT_SUPP`), not 0x07 (`LSTP_TOO_LARGE`).
- Prerequisites: answer each serial write at once: accept what you can
  take and NACK the rest; do not hold the answer until your line
  drains. Treat 0x06 for an oversized write as 0x07.
- If your device differs: while its remote line is stopped, writers
  block and the transmit stalls (the "transmit stalled" line and the
  channel's `timeouts` counter show it); a late answer can repeat or
  lose characters (AS-48).
- Closes when: the specification bounds the answer time.

## AS-30: a device does not send again a request answered "invalid"

- Specification: OBMF-ICP 1.0.0, 5.2, Table 4, defines the invalid
  status, but no rule on whether a device may send the same request
  again after it.
- Options: answer an undecodable request invalid; leave it
  unanswered; send the type's default answer, which for serial is a
  NACK the device sends again unchanged.
- This driver: answers a serial write or IPMI request that it cannot
  decode with the invalid status and no body past the IPMI echo, after
  the other answers owed on the channel, and counts it in
  `rx_malformed`.
- Prerequisites: treat an invalid answer as final for that request;
  keep one outstanding request per channel.
- If your device differs: a device that sends an invalid request again
  loops with the driver, one answer per resend; one that has more than
  one request outstanding may match the invalid answer to a later
  request.
- Closes when: the specification defines the retry rule.

## AS-44: a board description puts no devices below a serial channel's node

- Specification: neither specification covers firmware descriptions.
  A serial channel's node names the channel or disables it; the
  kernel could also bind devices described below it.
- Options: no devices below a serial node; register the port so that
  devices described below it bind.
- This driver: registers the tty without serdev, so a device described
  below a serial node (a Bluetooth chip, a GNSS receiver or a console)
  is not bound.
- Prerequisites: none for the device; describe no client devices
  below a serial channel's node.
- If your device differs: a client described below a serial node never
  appears; user space can still open the tty.
- Closes when: a board needs such a client.

## AS-45: ACPI tables for the device use the interface-object layout

- Specification: neither specification covers ACPI tables, and a USB
  interface has no ACPI object of its own (`REQUIREMENTS.md`, "ACPI
  example").
- Options: only channel objects below an interface object; also
  channel objects directly below the port object.
- This driver: looks for channel objects only below the interface
  object whose `_ADR` is the interface number, and says nothing when
  there is none.
- Prerequisites: place the channel objects below such an interface
  object.
- If your device differs: with channel objects directly below the port
  object, no channel gets a node, and a channel object with `_STA` 0
  there does not keep its channel from getting a device. If a channel
  number equals the interface number, that channel object is taken as
  the interface object. No warning says why.
- Closes when: a fielded ACPI table for such a device is known.

## AS-46: an LSTP status 0x04 on an I2C channel is a target NACK

- Specification: LSTP 1.0, 3.1.1 (p8), names 0x04 `LSTP_NACK` "LSTP
  link error or not established"; the I2C text of 4.4 gives no status
  for a target that does not acknowledge.
- Options: 0x04 is a target NACK; 0x04 is a link failure.
- This driver: on an I2C channel, takes 0x04 as a target NACK and
  returns `-ENXIO`, as for an absent device, without a message.
- Prerequisites: on an I2C channel, answer a target NACK with 0x04,
  and a link or controller failure with another nonzero status.
- If your device differs: a link failure on an LSTP I2C channel looks
  like an empty bus: `i2cdetect` finds nothing, and a client probe
  fails quietly.
- Closes when: the specification names the status of a target NACK.

## AS-47: the device enables every channel the host is to use

- Specification: OBMF-ICP 1.0.0, 4.3 (p22), step 3, has producer
  channels "pre-configured in Manufacturing and always enabled", but
  step 2a makes the host's enable the condition for channels the
  device consumes ("If it is not met, the Device boot will not
  proceed"), and 5.15 (p59) says the host "should enable all the
  necessary channels".
- Options: the device enables its channels and the host never writes
  the enable; the host enables a valid, disabled channel it is to use.
- This driver: leaves out, with a warning, a channel that is not
  enabled or not valid, and never writes CHANNEL_ENABLE.
- Prerequisites: enable every channel the host is to use, with a
  valid configuration, before the host reads channel 0; do not wait
  for an enable from the host.
- If your device differs: its disabled channels get no device
  ("channel <n>: not enabled, ignored"); a device that waits for the
  host's enable on a channel it consumes does not finish booting.
- Closes when: a device with consumer channels, or one that boots
  with its channels disabled, is to be supported, or the specification
  says the host must enable.

## AS-48: a device treats a serial write it answers late as one answered in time

- Specification: OBMF-ICP 1.0.0, 5.5.2, Table 23, does not say what a
  device does with characters it answers after the host stopped
  waiting, and no request carries a tag (6.6), so the late answer is
  taken by the next request on the channel.
- Options: send the same characters again after a timeout; drop them
  on a timeout (drop 1); after a timeout, hold the channel's next
  request until the late answer comes or one more timeout passes.
- This driver: keeps the characters after no answer, status 0x02
  (Table 4) or the line timeout 0x40 (Table 21), and sends them again
  10 ms later. After a late answer, the next try takes it as its own.
- Prerequisites: a device that answers late did not take the
  characters, or a repeat is harmless on its line; its late answer
  accepts what the try it answers carried.
- If your device differs: characters it took before a late answer go
  out twice on the line; the next try's own answer is then taken by
  the request after it, so characters the device refused there count
  as sent and are lost.
- Closes when: the host holds the next request after a timeout, the
  specification says what a device does with a late request, or
  requests get a tag.

## AS-49: a device that answers a serial write busy did not take the characters

- Specification: OBMF-ICP 1.0.0, Table 4 (p26), lists 0x03 "Channel
  not ready / busy" with no count, and LSTP 1.0, 3.1.1 (p8), says
  "retry later"; neither says whether a serial device took any of the
  request's characters before answering busy.
- Options: busy means none were taken, keep them and send again; busy
  is an error, drop them with a warning (drop 1).
- This driver: keeps the characters and sends them again 10 ms later,
  with no limit; after 5 s, one "transmit stalled: busy" line.
- Prerequisites: answer busy only when you took none of the request's
  characters, and take them on a later try.
- If your device differs: characters it took before answering busy go
  out twice.
- Closes when: the specification says what busy means on a serial
  channel.
