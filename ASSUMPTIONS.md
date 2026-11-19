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
  discovery with "discovery failed: -EBUSY", and the driver releases
  the interface. On LSTP, a busy device header is retried for a few
  discovery passes and then fails discovery the same way; a busy
  answer to any read of a channel's
  configuration leaves that channel out with "channel <n>: status 0x3,
  ignored".
- Prerequisites: answer channel 0 with status 0x00 from the first
  request after the interface is configured; if start-up takes time,
  connect to the bus later rather than answer busy.
- If your device differs: it gets no function device and the
  interface is released, or on LSTP it binds without the busy
  channel, until the interface is bound again or the device is
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
  `-ETIMEDOUT`, is counted in `timeouts` and logs "channel <n>: no
  response in <ms> ms"; the channel is not reset. The default response
  timeout is 1000 ms; a serial write adds twice its time on the line.
  The channel's next request waits until the late response arrives or
  one default timeout (1000 ms) passes. A response that arrives with no
  request waiting is counted in `rx_late` and dropped; one that arrives
  later, while the next request waits, is taken as its answer. During a software unbind, the
  first timeout stops the device instead.
- Prerequisites: answer every request within the timeout; if a
  request cannot finish in time, answer it with an error status (for
  example 0x02) before the timeout rather than late; send no response
  to a request after the timeout.
- If your device differs: an answer more than one default timeout
  late is taken by the next request on that channel: on I2C a false
  ACK, or data read from the earlier address (KI-1, AS-52).
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
- This driver: on a bulk OUT stall it logs "bulk out stalled" and
  fails the message with `-EPIPE` (`-EIO` on I2C and SPI); a message is never
  sent twice. Its recovery work then cancels every message queued on
  the endpoint behind the stalled one (they fail with `-EPIPE` too)
  and clears the halt, once per burst of stalls; until the clear
  succeeds, every send fails with `-EPIPE` and sends nothing. A serial
  port drops the characters of a write caught this way (KI-33). On a
  bulk IN stall it logs "bulk in stalled", clears the halt and goes on
  reading. A halt clear that fails logs "bulk in clear halt failed:
  <errno>" or "bulk out clear halt failed: <errno>" and is tried again
  after 250 ms; ten failures in a row stop the device. It resets
  neither the device nor the port; only the USB core does that, on its
  own errors.
- Prerequisites: after `CLEAR_FEATURE(ENDPOINT_HALT)` on either bulk
  endpoint, take and answer new messages with no reset (8.2 step 1);
  treat the stalled message as gone.
- If your device differs: a device that expects a resend loses the
  request, and the requests of other channels caught by the same
  stall; a device that needs a reset after a stall stays stopped
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
  2, and an I2C adapter to I2C controller channels and a GPIO chip to
  GPIO channels with role 0.
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
- This driver: answers an Interrupt Notify (command 4) on bulk OUT,
  with status 0x00, or with the invalid status for one with a bad
  entry, and sends nothing for a Datagram (command 5). The GPIO driver
  answers a Notify before it runs the handlers; with no GPIO driver
  bound, the core answers it with status 0x00. An LSTP IRQ_EVENT,
  which has no answer, is taken as a Datagram.
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
  CONFIGURATION_VALID set. HEALTH_STATUS sums up several conditions,
  some of which a host cannot act on at discovery, so it is not used
  to leave a channel out. A channel with HEALTH_STATUS 3 or 15, or
  with CHANNEL_ERROR_INDICATOR set, binds with one warning per bind:
  "channel <n>: health <h>, error indicator <i>".
- Prerequisites: clear CHANNEL_ENABLE or CONFIGURATION_VALID on a
  channel the host must not use; while an enabled, valid channel is
  not ready, answer its requests with the channel type's status codes.
- If your device differs: the host binds a channel the device reports
  unhealthy; its requests fail with the device's status codes or time
  out, after the warning at bind.
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
  stalled" line after 5 s. Each timeout with no answer doubles the
  default part of the wait for the answers after it, up to 10 s in
  all, until the port is opened again (AS-56); a timeout status does
  not. On LSTP, a device write the host cannot
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

## AS-31: a device need not answer a read of an output line with its level

- Specification: OBMF-ICP 1.0.0, Table 17, lists a read of an output
  ("Read on OUT") as an invalid operation; whether a device may still
  return the level is not said. LSTP 1.0 does not say either.
- Options: outputs not readable: the host keeps the value last set;
  outputs readable; one read of the outputs at bind to find out.
- This driver: never reads an output line from the device, on either
  protocol: a read of an output gives the value last set, from the
  line's default output at bind.
- Prerequisites: drive each output only as the host sets it, from its
  default output on.
- If your device differs: a persisted output (GPIO_OUTPUT_PERSIST_STATE
  1) reads its default, not its level, until the first Set after the
  bind; an output the device changes on its own reads the value last
  set.
- Closes when: the specification says whether outputs are readable.

## AS-32: a device answers a GPIO read with one entry per requested index

- Specification: OBMF-ICP 1.0.0, Table 19, gives no order for the
  entries of a Get Values response.
- Options: match each entry by its index, each index once; take the
  entries in the order of the request.
- This driver: matches each entry of an OBMF-ICP response by its
  index; on LSTP, GET_VALUE answers in the order of the request.
- Prerequisites: return exactly one entry for each line asked, with
  data 0 or 1.
- If your device differs: a response with a missing, repeated or
  unknown index, or data above 1, fails the read with `EPROTO` and is
  counted as a protocol error.
- Closes when: the specification gives the order.

## AS-33: a device sends no GPIO interrupt events while no interrupt is configured

- Specification: neither OBMF-ICP 1.0.0 nor LSTP 1.0 says which
  interrupt configuration a line has before the host sets one.
- Options: none, so no events until the host configures a line; some.
- This driver: sets a line's interrupt only while a consumer holds it
  enabled, and runs the line's handler only then. An event for any
  other line is counted in `events/gpio_irq_unused`, with no log line,
  and dropped; an Interrupt Notify is still answered.
- Prerequisites: send no interrupt event for a line whose interrupt
  was never configured.
- If your device differs: the counter grows; no line is affected.
- Closes when: the specification says what a line's interrupt
  configuration is before the host sets one.

## AS-34: a failed multi-line GPIO Set may have applied some entries

- Specification: neither OBMF-ICP 1.0.0 nor LSTP 1.0 says what a
  failed Set Values applied.
- Options: none of its entries; any prefix of them.
- This driver: cannot tell, so it keeps the values set before for
  every line of a Set message the device refuses, reads nothing back,
  and reports the failure ("cannot set line <n>: <errno>" or "cannot
  set lines: <errno>", counted in `events/gpio_set_failed`). A Set
  split over several messages stops at the first refused one; the
  messages before it stay set.
- Prerequisites: apply all entries of a Set Values message or none
  (AS-104).
- If your device differs: after a refused Set an output can be at a
  level the driver does not know: reads give the value set before,
  until the line is set again.
- Closes when: the specification says what a failed Set applies.

## AS-36: an LSTP line with a Z default starts at its released level

- Specification: LSTP 1.0, 4.3.1.2, allows a Z default output, which
  OBMF-ICP does not have; its level depends on the board.
- Options: the released level; leave the line out; refuse the channel.
- This driver: an open-drain output with a Z default starts high, an
  open-source one low; a push-pull output with Z stays an output and
  starts low.
- Prerequisites: use Z only on lines whose released level is the
  board's pull.
- If your device differs: the line reads a wrong level until it is
  set; nothing is driven because of it.
- Closes when: the LSTP specification gives the level of Z.

## AS-37: a cut LSTP GPIO table keeps its whole entries

- Specification: discovery can end an LSTP GPIO table early, at the
  READ_CONFIG range or on an error status after the first slice
  (`REQUIREMENTS.md`, "Device requirements: LSTP").
- Options: keep the whole entries; refuse the channel.
- This driver: keeps the whole lines and leaves a partial one out,
  with "channel <n>: GPIO table cut in line <m>".
- Prerequisites: keep the GPIO table within the READ_CONFIG range and
  answer every slice of it.
- If your device differs: the lines past the cut are missing; the log
  has one warning.
- Closes when: every device's GPIO table is shown to fit and to be read
  whole.

## AS-38: an LSTP GPIO table ends after 255 blocks or with its data

- Specification: LSTP 1.0, 4.3.1.4, gives the limit of 255 blocks but
  no end marker, and nothing on what may follow.
- Options: read up to 255 blocks or to the end of the data; a block
  with count 0 ends the table.
- This driver: reads up to 255 blocks; a block with count 0 adds no
  line; bytes after the 255th block are ignored with "channel <n>: <k>
  bytes after 255 GPIO blocks ignored".
- Prerequisites: send no data after the GPIO table.
- If your device differs: a warning; no line is added.
- Closes when: the LSTP specification says how a table ends.

## AS-39: an LSTP GPIO channel always sends its first block count

- Specification: LSTP 1.0 does not say whether a GPIO channel may have
  an empty configuration.
- Options: leave the channel out; keep a channel with no lines.
- This driver: leaves a GPIO channel with an empty configuration out.
- Prerequisites: send at least the first block count for every GPIO
  channel.
- If your device differs: a GPIO channel with no lines gets no device;
  nothing else changes.
- Closes when: the LSTP specification says how a channel with no lines
  is described.

## AS-40: an LSTP GPIO error response carries no data the host needs

- Specification: LSTP 1.0 does not say whether an error response may
  carry data.
- Options: ignore the data; take the response as malformed.
- This driver: ignores data after an error status and reports the
  status.
- Prerequisites: put nothing the host needs after an error status.
- If your device differs: what a device puts after an error status is
  lost; the error itself is reported.
- Closes when: the LSTP specification says what an error response
  holds.

## AS-41: LSTP GPIO names are UTF-8 and persist states are 0 or 1

- Specification: LSTP 1.0 gives no encoding for GPIO names and no range
  for the persist state.
- Options: copy both as sent; check them and leave the line out.
- This driver: copies both as sent; the GPIO driver gives no name to a
  line whose name holds a control character.
- Prerequisites: send UTF-8 names, NUL-padded, and a persist state of
  0 or 1.
- If your device differs: a line's name shows odd characters, is cut
  at 32 bytes or is missing; a persist state above 1 is not used by
  the host.
- Closes when: the LSTP specification gives the name encoding and the
  persist range.

## AS-42: the host sends no GPIO request with an interrupt or reserved command other than Set Interrupt Config

- Specification: neither OBMF-ICP 1.0.0 nor LSTP 1.0 says which error
  a host request with a GPIO interrupt or reserved command gets when
  the dialect cannot carry it.
- Options: not supported for commands 2 to 5 and invalid for 6 and up;
  invalid for 4 and up.
- This driver: the GPIO driver sends Get Values, Set Values and Set
  Interrupt Config only. On LSTP, Set Interrupt Config becomes
  SET_IRQ_CONFIG; Get Interrupt Config and commands 4 and 5 fail with
  `EOPNOTSUPP`, and commands 6 and up with `EINVAL`, before anything is
  sent.
- Prerequisites: none (host side only).
- If your device differs: no request reaches the device either way.
- Closes when: the specification sets the errors.

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
  10 ms later. Since drop 3 the next try waits up to one default
  timeout (1000 ms) for the late answer, which is dropped, and each
  timeout with no answer doubles the wait of the tries after it, up to
  10 s, until the port is opened again: a device slower than the
  default timeout gets one repeat per open, not one per write. Since
  drop 4 a timeout status does not double the wait. An answer later
  than that is taken by the next try as its own.
- Prerequisites: a device that answers late did not take the
  characters, or a repeat is harmless on its line; its late answer
  accepts what the try it answers carried.
- If your device differs: characters it took before an answer later
  than that go out twice on the line; the next try's own answer is then taken by
  the request after it, so characters the device refused there count
  as sent and are lost.
- Closes when: the specification says what a device does with a late
  request, or requests get a tag.

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

## AS-50: ten lines per five seconds per place in the code are enough to diagnose a device

- Specification: none; OBMF-ICP does not say how a host reports a
  device's faults.
- Options: one rate limit per place in the code, shared by all
  devices, with a counter per event; a rate limit per device; none.
- This driver: counts every event in the interface's `events/`
  directory in debugfs and logs it at most 10 times per 5 s from each
  place in the code, for all devices together. The configuration
  warnings of a bind are not part of this: each bind logs its first
  ten and then one line with the number not logged.
- Prerequisites: a report carries the first lines and the `events/`
  counters (`TESTING.md`, "What to send with a report").
- If your setup differs: with several devices failing at once, the
  lines of all but the first few are suppressed; the counters still
  count each device.
- Closes when: reports show the lines are enough, or need more.

## AS-51: a request whose send timed out has no answer worth waiting for

- Specification: OBMF-ICP does not say what a device does with a
  message it received only in part.
- Options: wait for a late answer only after a response timeout; wait
  after every timeout, of the send too.
- This driver: holds the channel's next request only after a response
  timeout; a send that times out fails at once and holds nothing.
- Prerequisites: do not answer a message whose transfer did not
  complete.
- If your device differs: its late answer to such a message is taken
  by the next request on the channel (KI-1).
- Closes when: the specification says what a partly received message
  gets.

## AS-52: a device answers a timed-out request, if at all, within one second after the timeout

- Specification: OBMF-ICP 1.0.0 gives no time limit for a late answer,
  and no request carries a tag (6.6).
- Options: hold the next request for one default timeout; hold it as
  long as the timed-out request waited, up to 10 s.
- This driver: after a response timeout the channel's next request
  waits for the late answer, which is dropped, or for 1000 ms.
- Prerequisites: answer a request you could not answer in time within
  1000 ms after the host's timeout, or not at all.
- If your device differs: a later answer is taken by the next request
  on the channel (KI-1, AS-48).
- Closes when: the specification bounds a late answer or adds a tag.

## AS-53: a function driver's `failed` operation does not unbind, publish or wait for its own device requests

- Specification: none; a rule of this driver's interface for function
  drivers.
- Options: call the operation under the lock that delivers device
  requests, with the rule documented; call it without the lock.
- This driver: calls each published function driver's `failed`
  operation once after a transport failure, under that lock; the
  serial driver only marks its port failed and hangs it up.
- Prerequisites: a function driver's `failed` operation only marks
  its state and wakes or hangs up its users.
- If a driver differs: an operation that unbinds or waits for one of
  its own device requests deadlocks the channel when the device fails.
- Closes when: the operation is called without the lock.

## AS-54: every response on a channel held after a timeout is the late one

- Specification: responses carry no tag (OBMF-ICP 1.0.0, Table 3,
  p26).
- Options: take any response as the late one; match it to the
  timed-out request first.
- This driver: a response that comes while the channel is held and no
  request waits is counted in `rx_late`, dropped, and ends the hold.
- Prerequisites: send a response only to a request.
- If your device differs: an unsolicited response ends the hold early,
  and a late answer after it is taken by the next request (KI-1).
- Closes when: the specification adds a tag.

## AS-55: a device does not answer, after a suspend and resume, a request that timed out before it

- Specification: OBMF-ICP does not say what a device does with an
  answer it still owes when the host suspends it.
- Options: end the hold after a timeout when the device is suspended;
  keep it over the resume.
- This driver: a system suspend ends a hold it finds, so the first
  request after resume is sent at once. The bulk IN transfers stop at
  suspend, so an answer the device has not sent yet stays with it and
  can come after resume.
- Prerequisites: drop, or send before the suspend, an answer the host
  stopped waiting for.
- If your device differs: its late answer is taken by the first
  request after resume (KI-1).
- Closes when: the specification says what a suspend does to a pending
  answer.

## AS-56: a device answers a serial write within 10 s, wire time included

- Specification: OBMF-ICP 1.0.0, 5.5.2, gives no time for a serial
  answer.
- Options: cap the growing serial wait at the core's 10 s limit; a
  longer cap for serial channels.
- This driver: each timeout with no answer doubles the serial wait,
  up to 10 s for the wait and the wire time together, until the port
  is opened again; a timeout status leaves it as it is.
- Prerequisites: answer every serial write within 10 s.
- If your device differs: every write times out at the cap and is
  sent again, and the port logs a "transmit stalled" line for each
  stall (AS-48).
- Closes when: the specification bounds the answer time.

## AS-57: a device sends serial writes to the host without waiting for each answer

- Specification: OBMF-ICP 1.0.0, 5.5, does not say whether a device
  waits for the host's answer to a device write before it sends the
  next.
- Options: a queue per channel with a reported limit; one device
  request in flight per channel.
- This driver: queues up to 16 device requests per channel beyond 3
  reserved; a request that finds no room is refused with the default
  answer and logs "channel <n>: device request queue full, request
  refused"; `dreq_max_depth` in debugfs shows the most it held.
- Prerequisites: send a refused write again later.
- If your device differs: a device that waits for each answer never
  fills the queue, and a queue-full line then points to another cause
  (KI-20).
- Closes when: the specification says how many device requests may be
  in flight.

## AS-58: a device that times out once is slow, not losing answers

- Specification: a timeout does not tell a slow device from one that
  lost the request.
- Options: double the serial wait after each timeout until the port
  is opened again; keep a fixed wait.
- This driver: doubles the wait for the next answers after each
  timeout with no answer, up to 10 s; an answer with a timeout status
  came in time and leaves the wait as it is.
- Prerequisites: a device that missed one answer is slow on the next
  as well, or the longer wait costs little on its line.
- If your device differs: after lost answers the host tries less and
  less often, up to 10 s apart, until the port is opened again; a
  silent device's "transmit stalled" line comes at about 9 s, not 7 s.
- Closes when: the specification bounds the answer time.

## AS-59: a function driver never needs to report one of the core's events itself

- Specification: none; a rule of this driver's interface for function
  drivers.
- Options: function drivers report their own events, and a malformed
  response through `obmf_fdev_protocol_error()`; export the core's
  report for any event.
- This driver: `obmf_fdev_report()` takes only the function drivers'
  events and warns once for one of the core's; a malformed response
  is reported as the core's bad response.
- Prerequisites: a function driver sees no core-level fault other
  than a malformed response.
- If a driver differs: that fault is not counted as the core's event
  until the interface gains a way to report it.
- Closes when: a function driver needs another core event.

## AS-60: an LSTP I2C channel has no target mode

- Specification: LSTP 1.0 I2C (4.4.2, pages 23-25) describes the
  channel as a controller only.
- Options: no target mode; a target mode over device requests, which
  needs a specification.
- This driver: the I2C adapter does not offer target registration.
- Prerequisites: a board does not use an LSTP I2C channel as a target.
- If your board differs: registering an I2C target on the channel
  fails with `-EOPNOTSUPP`.
- Closes when: LSTP or OBMF-ICP defines a target mode.

## AS-62: a Set Interrupt Config the device refuses leaves the interrupt off until the next change

- Specification: OBMF-ICP 1.0.0, Table 17, lets a device refuse a Set
  Interrupt Config; the kernel sends the configuration after the
  consumer's call has returned, so the refusal cannot reach the
  consumer.
- Options: report it, count it, leave the interrupt off and try again
  at the next change; refuse at request time, from a per-line
  capability the device does not describe.
- This driver: logs "cannot configure the interrupt of line <n>:
  <errno>", counts `events/gpio_irq_config_failed`, and leaves the
  interrupt off: the line's events are counted in
  `events/gpio_irq_unused`. The next change of the line's interrupt
  sends the configuration again.
- Prerequisites: answer success only for a line whose edges you will
  report.
- If your device differs: a consumer waits for edges that never come;
  the log line and the counter show it.
- Closes when: the specification says how a device tells which lines
  can interrupt.

## AS-63: a device sends one interrupt entry per edge and none after a disable

- Specification: OBMF-ICP 1.0.0, Table 18, does not say whether two
  edges between notifications give one entry or two, or whether a
  notification may follow a disable.
- Options: one handler run per entry, entries for lines without an
  interrupt counted and dropped; a read of the line on every entry.
- This driver: runs the handler of a line with its interrupt enabled
  once for each entry; counts the other entries in
  `events/gpio_irq_unused`.
- Prerequisites: send one entry per edge, or merge close edges into
  one; send nothing for a line after a disable is answered.
- If your device differs: a handler runs more or less often than edges
  came; a consumer that reads the line still sees its level.
- Closes when: the specification answers both points.

## AS-64: a device keeps a line's interrupt configuration until the host changes it

- Specification: OBMF-ICP 1.0.0 does not say whether a device keeps a
  GPIO line's interrupt configuration across a reset.
- Options: send it on change only; send every enabled line's
  configuration again after the device comes back.
- This driver: sends Set Interrupt Config only when the trigger differs
  from the last one the device took.
- Prerequisites: keep each line's interrupt configuration while the
  interface stays bound.
- If your device differs: after the device loses the configuration,
  the line gives no more interrupts, with no log line, until a
  consumer changes the line's interrupt.
- Closes when: the specification says what a reset clears.

## AS-65: the state of an LSTP interrupt event is not used

- Specification: LSTP 1.0, 4.3.2, gives a state with each line of an
  IRQ_EVENT and does not say what it means.
- Options: check it and drop it; pass it on as a level, which no
  OBMF-ICP field holds.
- This driver: an event with a state above 1 is malformed and dropped;
  otherwise it becomes an Interrupt Notify Datagram of the lines,
  without the states.
- Prerequisites: none on the device; a consumer that needs the level
  reads the line.
- If your device differs: a consumer that reads the line after a short
  pulse sees the level after it; the edge is still delivered.
- Closes when: the LSTP specification says what the state means.

## AS-66: a device may send the next GPIO notification before the handlers finish

- Specification: OBMF-ICP 1.0.0, Table 18, does not say whether a
  device waits for the host's handlers before its next notification.
- Options: answer first and queue the next; answer after the handlers,
  which would block a handler that reads a line.
- This driver: answers an Interrupt Notify before the handlers run;
  the core holds up to 16 device requests per channel and refuses one
  more, with "channel <n>: device request queue full, request refused"
  (or "request dropped" for a Datagram).
- Prerequisites: send no more notifications than the queue holds while
  the handlers run, or merge edges.
- If your device differs: later notifications of a burst are lost,
  with that line and the channel's `dreq_max_depth` in debugfs.
- Closes when: a device with GPIO interrupts is run under a burst of
  edges.

## AS-68: a request the device does not take off the bus within the response wait would not have been answered

- Specification: OBMF-ICP over USB 1.0 gives no time within which a
  device must accept a request on its bulk OUT endpoint.
- Options: one wait for sending and answering, both `ETIMEDOUT`; a
  shorter send wait with its own errno.
- This driver: waits for the bulk OUT transfer for the whole response
  wait; a send that does not finish in that time fails with
  `ETIMEDOUT`, and on a serial channel the wait doubles after it as
  after no answer. Each request has its own bulk OUT transfer,
  cancelled when its wait runs out, whatever the other channels send;
  the time it spends queued on the endpoint behind other channels'
  transfers counts against its wait.
- Prerequisites: take a request off the bus within the response wait,
  or do not answer it either.
- If your device differs: a device that takes requests slowly but
  answers at once costs the whole wait per request; on a serial
  channel the wait doubles up to 10 s until the port is opened again,
  also when the time went on other channels' transfers queued ahead of
  it on the endpoint (the characters are kept).
- Closes when: a device is seen whose bulk OUT transfer does not
  finish within the response wait.

## AS-69: a device sends a timeout status only after its own timeout ran

- Specification: OBMF-ICP 1.0.0 Table 4 (status 0x02) and Table 21
  (0x40) do not say how long the device waits before it answers with
  them.
- Options: keep the serial wait on a timeout status; double it as
  after no answer.
- This driver: a timeout status is an answer in time; the serial
  driver keeps its wait and sends the characters again 10 ms later.
- Prerequisites: answer 0x02 or 0x40 only after the request's own time
  on the line ran out.
- If your device differs: a device that answers 0x40 at once gets the
  same characters every 10 ms plus the request's time, with "transmit
  stalled" lines in the log.
- Closes when: the time a device waits before 0x02 or 0x40 is known.

## AS-70: a timeout status on channel 0 during discovery means the device is still starting

- Specification: OBMF-ICP 1.0.0 Table 4 gives status 0x02 but not when
  channel 0 sends it.
- Options: run the discovery pass again until its deadline; fail the
  bind at once.
- This driver: discards a discovery pass that ends in a timeout
  status, no answer or a malformed response, and runs it again after a
  delay, until the discovery deadline (30 s).
- Prerequisites: answer a channel 0 read with 0x02 only while starting
  up.
- If your device differs: a device whose channel 0 always answers
  0x02 keeps discovery running until its deadline; discovery then
  fails and the interface is released.
- Closes when: the use of 0x02 on channel 0 is known.

## AS-72: a request in flight at an unplug can time out before the disconnect

- Specification: none; the USB core calls the driver's disconnect some
  time after the cable is pulled, with no bound against the response
  wait.
- Options: return what happens first; turn a timeout followed by a
  disconnect into `ENODEV`, which would delay every timeout.
- This driver: a request whose wait runs out before the disconnect
  fails with `ETIMEDOUT`; the disconnect ends the other waiting
  requests with `ENODEV`, and cancels a send in flight at once, which
  then fails with `ENODEV` too.
- Prerequisites: none from the device. Callers do not read
  `ETIMEDOUT` as "the device is still there".
- If your setup differs: a caller that retries after `ETIMEDOUT`
  sends one more request, which fails with `ENODEV`.
- Closes when: unplug runs show which errnos occur.

## AS-73: a device acknowledges a Generate Break, whether or not it can make one

- Specification: LSTP 1.0, 4.5.2, asks a device to ignore an event
  flag it does not support; OBMF-ICP 1.0.0 Table 22 says nothing.
- Options: a break request succeeds when the device acknowledges it;
  also accept a refusal as "no break support".
- This driver: `tcsendbreak()` (TCSBRK) sends one Generate Break; a
  refusal (NACK) is sent again up to 20 times, 10 ms apart, then fails
  with `EIO`.
- Prerequisites: a device without break support acknowledges the
  request.
- If your device differs: after 20 refusals `tcsendbreak()` fails with
  `EIO` after at least 190 ms; an error status fails it with `EIO` at
  once. On LSTP, "not supported" (0x06) gives "Operation not
  supported", which the kernel's tty layer turns into success: no
  break is sent, and the channel's `status_errors` counts it (seen on
  the USB4715 board's LSTP UART, KI-35). A success shows only that the
  device took the request, not that a break reached the line.
- Seen in this drop's runs: the development microcontroller board's
  OBMF-ICP and LSTP firmware acknowledge it; the USB4715 board's LSTP
  UART refuses it (KI-35).
- Closes when: each device's break support is known.

## AS-74: an LSTP device does not send a lost arbitration on a serial channel

- Specification: LSTP 1.0 defines ARB_LOST (0x05) for any request; a
  UART has no arbitration.
- Options: treat it as a permanent error; give it a code of its own.
- This driver: on a channel other than I2C, ARB_LOST is reported as
  the OBMF-ICP permanent error, the same status a serial NACK has
  (AS-10), with `EIO`. A debug line gives the LSTP status next to the
  OBMF-ICP one.
- Prerequisites: send ARB_LOST only on I2C channels.
- If your device differs: a serial lost arbitration and a serial NACK
  give the same status and errno; only the debug line tells them
  apart.
- Closes when: LSTP device makers confirm it, or a run with the debug
  line shows which status a device sends on a serial channel.

## AS-76: an SPI Write without data and with de-assert only releases the chip select

- Specification: Table 50 (p51) gives Write Data Size and the CS Control
  bits with no minimum size. A device may refuse a zero-length Write, or
  clock nothing and still not move the chip select.
- Options: (a) rely on it (chosen); (b) no release on native devices:
  the chip select waits for the next message's assert; (c) ask the
  authors first.
- This driver: On every error but -ENODEV the controller sends the Write
  with CS Control 0b01 and sizes 0 (`spi-obmf.c:obmf_spi_release`); a
  failure is logged once per ratelimit window and the next message
  asserts afresh.
- Prerequisites: A device takes an SPI Write with Write Data Size 0 and
  moves the chip select by the CS Control bits alone.
- If your device differs: The release fails ("chip select N not
  released") or does nothing, and the chip select stays asserted until
  the next message asserts it again; a device on another chip select may
  see two selected.
- Seen in this drop's runs: the development microcontroller board's
  LSTP firmware answers this release with status 0x01 (KI-34).
- Closes when: The firmware authors confirm a zero-length Write, or a run
  shows the chip select released after an error.

## AS-77: an OBMF-ICP SPI Write/Read is full duplex

- Specification: Table 50 has separate Write and Read Data Sizes for
  Write/Read and does not say whether the read happens during or after
  the write.
- Options: (a) full duplex (chosen; LSTP's reading); (b) write then
  read: the controller would set SPI_CONTROLLER_HALF_DUPLEX and the SPI
  core would refuse tx+rx transfers.
- This driver: tx and rx in one transfer are one Write/Read with Write
  Data Size = Read Data Size = len.
- Prerequisites: A native device clocks both directions at once for
  Write/Read.
- If your device differs: Read data shifted by the write length; flash
  ID reads return the wrong bytes. The fix is one flag.
- Closes when: The OBMF-ICP authors say how a Write/Read clocks.

## AS-78: on OBMF-ICP, a command with neither CS Control bit keeps the chip select as it was

- Specification: Table 50 defines bit 5 (assert before) and bit 4
  (de-assert after) but not what a command with neither does.
- Options: (a) held to the next command (chosen); (b) released after
  each command: no chip select held across transfers or the core's cut;
  (c) ask.
- This driver: The first transfer asserts, the middle ones keep, the
  last de-asserts.
- Prerequisites: A device leaves the chip select as it was when a
  command has neither bit set.
- If your device differs: Multi-transfer flash commands (command,
  address, then data) lose the chip select between transfers and fail.
- Closes when: The OBMF-ICP authors say what a command with neither
  bit does.

## AS-79: the LSTP SPI command's chip select bits are those of its bit table

- Specification: LSTP 1.0 4.2.2 says "The upper 3 indicate what CS pins
  to use" (p13), which could read as a mask of three pins, while the
  table on p15 gives bits 4-5 as CS state flags and bit 6 as the CS
  select.
- Options: (a) the bit table (chosen; what existing LSTP devices use);
  (b) the "upper 3 bits" mask: three chip selects, no CS state flags.
- This driver: CS0 0x00 and CS1 0x40 with assert 0x20 and de-assert
  0x10; CS2 and CS3 refused.
- Prerequisites: An LSTP device reads bits 4-6 as the bit table on p15
  says.
- If your device differs: The wrong chip select, or none, is driven; a
  flash ID read returns 0x00 or 0xff.
- Closes when: The LSTP authors confirm the bit table, or a hardware
  run reads the right ID on each chip select.

## AS-80: an LSTP SPI channel runs mode 0 with 8-bit words, most significant bit first

- Specification: LSTP 1.0 4.2.1 lists no mode, polarity, bit order or
  word size, and the device cannot be asked.
- Options: (a) mode 0, 8 bits, MSB first (chosen; what SPI NOR flash
  uses); (b) refuse clients until the mode is known.
- This driver: Table 84 flags 0: the controller offers mode 0 only and
  8-bit words; a client asking another mode is refused at setup.
- Prerequisites: An LSTP device clocks mode 0, 8 bits, MSB first.
- If your device differs: Garbage on every transfer; the setup check
  cannot catch it because the device's mode is unknown.
- Closes when: The LSTP authors say which mode and word size a device
  runs.

## AS-81: only 8-bit SPI words are served

- Specification: Table 84 (p76) defines the word-size fields; Tables 50
  and 51 carry bytes only.
- Options: (a) 8 bits only, another width refused at probe (chosen); (b)
  a byte layout guessed (little endian words padded to bytes).
- This driver: A channel configured for another width gets no controller
  and the probe message names the widths.
- Prerequisites: A device that wants other widths is reconfigured to 8
  bits, or the authors define the layout.
- If your device differs: The channel is not served; visible at probe,
  never wrong data.
- Closes when: The spec defines the layout of wider words.

## AS-82: the higher of the read and write clocks is the channel's clock

- Specification: Table 84 (p77) names both as "Requested maximum SPI
  clock frequency" for each direction, not how one transfer in both
  directions is clocked.
- Options: (a) the higher (chosen): no client is overclocked; (b)
  the lower: a client rated between the two runs
  above its rating in one direction.
- This driver: min_speed_hz = max_speed_hz = max(RD, WR); setup refuses
  a client slower than that; the length cut uses min(RD, WR).
- Prerequisites: A device clocks each direction at or below its own
  maximum.
- If your device differs: A client rated below the higher clock is
  refused at setup though it would work in one direction.
- Closes when: Device behaviour is known or the authors say which clock
  a Write/Read uses.

## AS-83: an SPI transfer of at most 400 ms on the wire is answered within the default timeout

- Specification: Neither specification bounds the device's time to
  answer a long SPI transfer.
- Options: (a) the default wait and the clock / 20 cut (chosen); (b) a
  wait scaled by the length (up to 10 s, longer than the SPI core waits at unbind); (c)
  a smaller cut.
- This driver: Requests pass timeout 0 (the default); max_transfer_size
  includes clock / 20.
- Prerequisites: A device answers an SPI request within 1000 ms when its
  wire time is at most 400 ms.
- If your device differs: -ETIMEDOUT, then the release after the core's
  hold; the message fails.
- Closes when: A hardware run of long reads at the slowest clock, or a
  device timing statement.

## AS-90: an LSTP NOT_SUPP means the request is not valid for that channel

- Specification: LSTP 1.0 3.1.1 has NOT_SUPP (0x06), "Device does not
  support this command or option"; OBMF-ICP 1.0.0 Table 4 has no such
  code, and its own 0x06 means "Request size not supported".
- Options: INVALID with `-EOPNOTSUPP` (chosen); INVALID with `-EIO`, as
  LSTP_ERROR and a native INVALID; the raw 0x06 passed through (read as
  a size error; the old behaviour).
- This driver: NOT_SUPP becomes OBMF-ICP INVALID on every channel type;
  the caller gets `-EOPNOTSUPP`; the channel's status errors count it;
  no device-error line is logged.
- Prerequisites: a device sends NOT_SUPP only for a command or option
  the channel never supports, and LSTP_ERROR for a bad parameter.
- If your device differs: a device fault answered NOT_SUPP reaches the
  user as `-EOPNOTSUPP`, the same as a host refusal, with no warning;
  only the counter and the debug line show it.
- Closes when: the firmware authors say when a device sends NOT_SUPP, or
  a run with the debug line shows which requests get it.

## AS-91: ten configuration warnings a bind, and a count of the rest, are enough to diagnose a device's channel table

- Specification: neither specification bounds how many channels a device
  lists with faults or says what a host logs about them.
- Options: ten lines a bind and a count (chosen); one line per bad
  channel; a ratelimit per call site; the count or the list in debugfs
  as well.
- This driver: the first ten channel warnings of each bind are logged,
  then "N more configuration warnings not logged".
- Prerequisites: a device's channel faults are few or repeat one cause.
- If your device differs: with more than ten different faults, the log
  says how many are missing, not which.
- Closes when: field use shows ten lines are enough, or a report needs
  the rest.

## AS-98: a 5-byte LSTP SPI configuration (count and clock) is complete

- Specification: LSTP 1.0 4.2.1 lists 13 bytes and does not say whether
  a device may send only the fields a host uses.
- Options: 5 bytes complete (chosen, as for the UART and I2C
  translations); all 13 bytes required.
- This driver: reads the device count and the clock from the first 5
  bytes; a shorter configuration is the "short config (N), ignored" bind
  warning and drops the channel.
- Prerequisites: a device's SPI configuration carries the device count
  and the clock in its first 5 bytes.
- If your device differs: nothing is lost (the later fields are not
  used); under 5 bytes the SPI channel is dropped with a bind warning.
- Closes when: the LSTP firmware authors say which fields a device always
  sends, or a hardware run shows each SPI device's configuration length.

## AS-99: an OBMF-ICP SPI Write's response may end after the echo

- Specification: Table 51 shows a Read Data Size in every SPI response
  and does not say whether a write-only response may leave it out.
- Options: both forms accepted (chosen, as the I2C driver's write-only
  response); Read Data Size 0 required; only the echo.
- This driver: a write-only request answered with no payload, or with a
  Read Data Size of 0, succeeds; any other payload is -EPROTO, counted
  as a protocol error.
- Prerequisites: a device answers an SPI Write with no payload or with a
  Read Data Size of 0.
- If your device differs: that write fails with -EPROTO and a
  protocol-error count; nothing is lost silently.
- Closes when: the OBMF-ICP authors say which form a write-only response
  takes, or a hardware run shows the device's form.

## AS-102: a device whose firmware has stopped NAKs bulk OUT, so host sends time out

- Specification: OBMF-ICP over USB 1.0 names a NAK only for bulk IN
  with no data, and a STALL as the failure signal (6, p14; 8.1, p17);
  it does not say what a device that stops taking OUT data does.
- Options: it NAKs, and the host's sends time out; it keeps taking
  data, and the responses time out; it stalls or does not answer the
  transfer, and sends fail at once.
- This driver: sends each request in its own bulk OUT transfer, waits
  for it at most the request's timeout and then cancels it, whatever
  the other channels send; so every option is bounded the same way.
- Prerequisites: none.
- If your device differs: a stuck device shows response timeouts
  ("no response") instead of send timeouts, or `EPIPE` or `EIO` at
  once; nothing else changes.
- Closes when: a run with the firmware halted shows which, or the
  device controller's documentation says it.

## AS-103: the host cancels a timed-out send within about 0.5 s, so an SPI unbind with a silent device ends inside the SPI core's wait

- Specification: none; this is host behaviour. With a device that does
  not answer, an SPI unbind waits for the message in flight and one
  request that releases its chip select, each up to a hold, a wait and
  the cancel of its transfer: about 4 s plus twice the cancel time,
  against the SPI core's wait for its queue of at least 5 s.
- Options: rely on a cancel within about 0.5 s; skip the release for a
  removed client, which would halve the bound.
- This driver: sends one release after the request in flight fails,
  also when its client is gone; every later message of a removed
  client fails at once with `ESHUTDOWN`.
- Prerequisites: none from the device; the host's USB controller
  cancels a bulk OUT transfer, and a small buffer is allocated, within
  about 0.5 s each.
- If your setup differs: on a host that is much slower, the SPI core
  gives up waiting for its queue ("problem destroying queue") while a
  message is still running; that message can then use memory the
  unbind has freed and crash the host (the use after free this drop
  fixed, KI-31 in `CHANGELOG.md`).
- Closes when: the cancel time is measured on the target hosts.

## AS-104: a GPIO Set Values message with an invalid entry changes no line and returns one status

- Specification: a Set Values response has one status for the whole
  message (OBMF-ICP 1.0.0 Table 19, p33; Table 3, p26); neither
  OBMF-ICP 1.0.0, 5.4 (p30), nor LSTP 1.0, 4.3.2 (p19), says whether a
  message with one invalid entry changes the other lines, or which
  status the device returns.
- Options: all or none per message; the valid entries applied (the
  host would then have to read the lines back).
- This driver: never sends an invalid entry itself; after a refused
  Set it keeps the values set before for that message's lines and
  reads nothing back (AS-34); a Set of more lines than one message
  holds goes out as several messages.
- Prerequisites: on a Set with any invalid entry, change no line, and
  answer 0x40 for an unknown index, 0x42 for a write to an input, 0x01
  for reserved data or a bad payload, the first failing entry in the
  message deciding.
- If your device differs: lines it did change read back their old
  values, without an error, until they are set again.
- Closes when: the specification or the device's documentation says
  it.

## AS-105: a device answers a second request on a busy channel with busy (0x03) and never sends the first one's late response after it

- Specification: one outstanding request per channel (OBMF-ICP 1.0.0
  Table 1 #15, p12; OBMF-ICP over USB 1.0, 6, p14), with no status for
  a second one and no tag that matches a response to its request
  (Table 3, p26).
- Options: busy (0x03), not run, counted per channel; 0x01; queue it
  or run it.
- This driver: sends a channel's next request only after the one
  before has been answered, or has timed out and a hold of up to 1 s
  has passed; drops a response that comes with no request waiting;
  gives the caller `EBUSY` for 0x03.
- Prerequisites: answer within the host's timeout (0x02 if needed);
  answer a second request 0x03 at once, without running it; send the
  first one's late response before that or not at all.
- If your device differs: 0x01 gives `EIO` and a "device error"
  warning; a late response sent after the busy answer can answer the
  host's next request (KI-1).
- Closes when: the specification says it.

## AS-106: an LSTP SPI WRITE answer may carry data; it is ignored

- Specification: LSTP 1.0, 4.2.2 (p14) gives the response to a WRITE
  as the 4-byte header alone; whether a longer one is malformed is not
  said. LSTP firmware has been seen to answer a WRITE with status
  0 and one data byte. Not stated in the specification.
- Options: the status decides and the data is ignored (chosen); the
  header only, data an error (every write-only SPI transfer then fails
  on such firmware); exactly one extra byte.
- This driver: a WRITE answered with status 0 and any data succeeds
  with nothing read; the data is not passed on, and a rate-limited
  debug line gives the channel and the length. An error status decides
  the outcome whatever data follows; an answer longer than the
  request's buffer is still a protocol error (`EPROTO`).
- Prerequisites: status 0 on a WRITE means the data was written; data
  after the header carries nothing the host needs.
- If your device differs: if the data reports a partial write, the
  host reports success for a write the device did not finish.
- Closes when: the firmware or specification authors say what the data
  is.
