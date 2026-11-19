# Known issues

What is wrong or missing in this version of the driver, why, and what
closes each item. The numbers are stable: please quote them in
reports. Gaps in the numbering are items already closed, or items
about code that is not in this drop.

## KI-1: a late response after a timeout can answer the next I2C or GPIO request

- Effect: after an I2C request times out, its late response can
  complete the next request with the same command: a false ACK, or
  data read from the earlier address. On GPIO, a late Set response
  can complete the next Set and hide its refusal, and a late Get
  response can give stale levels. It can only happen after a
  timeout. Since drop 3 the next request after a timeout waits for the
  late response, which is dropped, or for one default timeout (1 s);
  only a response later than that can still answer it.
- Why: OBMF-ICP responses carry no tag, so a late response that
  passes the echo and length checks looks like the right one.
- Closes when: the specification gives a way to flush or tag a
  pending response, and the driver uses it.
- Until then: if the `timeouts` counter of an I2C channel is above 0,
  treat later I2C results on it as suspect, and unbind and bind the
  interface.
- See AS-5.

## KI-2: an open `/dev/i2c-N` delays an unplug or module unload

- Effect: the unplug or unload waits until the file is closed, then
  completes.
- Why: the I2C core waits for open files when an adapter is removed;
  this holds for every I2C bus driver.
- Closes when: not planned. Close I2C tools first.

## KI-4: a device that omits the zero-length packet can merge messages

- Effect: a message that fills its last packet, from a device that
  sends no zero-length packet after it, merges with the next message;
  both are dropped and counted in `rx_malformed`.
- Why: each IN buffer is the descriptor's read size plus one header,
  because the specification leaves open whether that size counts the
  header.
- Closes when: the specification settles it, and the buffer is sized
  to match.
- See AS-2.

## KI-6: serial line settings cannot be changed

- Effect: after `stty` changes the speed or framing, the port shows
  the device's settings again.
- Why: whether a host may write a channel's configuration is open in
  the specification, and a device that refuses channel 0 writes could
  not take the change.
- Closes when: the specification answers it; then the write path.
- See AS-9.

## KI-7: no SMBus Host Notify

- Effect: SMBus devices that signal the host with Host Notify are not
  notified.
- Why: left out of this version until a device can enable it.
- Closes when: such a device is available to test with.

## KI-8: no SMBus block read or block process call

- Effect: client drivers that need block reads cannot use them on
  this adapter; byte, word and plain I2C transfers work.
- Why: the specification is ambiguous on the command for a block read
  and on whether the response starts with the byte count.
- Closes when: the specification settles both; then block read, with
  tests.

## KI-9: only the I2C, serial, GPIO and SPI function drivers exist

- Effect: IPMI, memory device and the other channel types are
  discovered and logged, but get no device. The
  SPI driver is new in this drop.
- Why: I2C, serial, GPIO and SPI come first; the other types follow.
- Closes when: each driver is added.

## KI-11: after a real stall, some devices stay stopped until replugged

- Effect: when an endpoint stalls, the driver clears the halt. A
  device whose firmware stops all OUT transfers after a clear-halt
  then needs a replug.
- Why: the driver follows OBMF-ICP over USB 1.0, 8.2 and leaves port
  and device resets to the USB core.
- Closes when: the device firmware keeps working after a clear-halt.
- Since drop 3 the stall is visible: "bulk in stalled", "bulk out
  stalled", "bulk ... clear halt failed: <errno>", and the `events/`
  counters in debugfs. The behaviour is unchanged.
- See AS-6.

## KI-14: a channel that answers busy during discovery

- Effect: if the device answers BUSY when a channel's configuration is
  read, discovery on an OBMF-ICP device fails ("discovery failed:
  -EBUSY") and the driver releases the interface, and an LSTP device
  binds without that channel, which is logged as "channel <n>: status
  0x3, ignored". A replug or a bind of the interface reads it again.
- Why: discovery does not retry a busy channel read on either
  protocol, and each keeps its own rule for a failed read.
- Closes when: a decision on whether discovery retries a busy channel.
- See AS-4.

## KI-15: on a hub, an interface object can collide with a port object

- Effect: under ACPI, if a USB hub carries OBMF-ICP on an interface
  whose number is also one of the hub's port numbers, the interface
  object and that port object have the same `_ADR`, and the channels
  may not be described. Interface 0 is not affected, since hub ports
  are numbered from 1.
- Why: the USB core gives a USB interface no ACPI object of its own,
  so the interface objects sit beside the hub's port objects
  (`REQUIREMENTS.md`, "ACPI example").
- Closes when: the USB core gives interfaces their own ACPI
  companions, or such a device appears and needs another rule.

## KI-17: a GPIO consumer that asks for open drain or open source cannot set the line high

- Effect: a consumer that asks for a drive mode (a firmware flag such
  as `GPIO_OPEN_DRAIN`, or `gpioset --drive`) gets `EINVAL` when it
  sets the line high (open drain) or low (open source). Push-pull use,
  the default, works.
- Why: the driver has no `set_config`, so the GPIO core emulates the
  mode by switching the line to input, and an OBMF-ICP line's
  direction is fixed by the device.
- Closes when: a `set_config` accepts a drive mode that matches the
  line's own, or the limit is accepted.

## KI-19: LSTP LED pins are not exposed

- Effect: on an LSTP device, an output with LED support is not a GPIO
  line, and a channel of LED pins only gives no GPIO chip. Earlier
  LSTP drivers expose these pins as LEDs.
- Why: LED support is not built yet; the count of pins left out is a
  debug message only.
- Closes when: LED support is built for LSTP with tests, or the limit
  is accepted.

## KI-20: serial input can be lost with no kernel message

- Effect: input is lost in two ways; only the second is logged, since
  drop 3.
  - When the device reports an overrun, the characters are already
    lost on the device. The driver counts the event in `TIOCGICOUNT`
    and inserts a `TTY_OVERRUN` mark, but a reader in raw mode never
    sees the mark. On one USB hub controller with LSTP management, its
    UART looped back and written at the line rate (115200 baud), about
    0.2 to 0.4% of the data was lost this way; paced writes came back
    whole.
  - On the same controller, device writes that the host refuses are
    not sent again. With the reader stopped, input past the host's
    buffer was lost (seen once, in one session). The refusals count
    only in `buf_overrun`. Since drop 3 a refusal when the channel's
    queue of device requests is full logs "channel <n>: device request
    queue full, request refused", rate limited, and the channel's
    `dreq_max_depth` in debugfs shows the queue's highest depth.
- Why: overruns are counted and marked only, and the driver relies on
  the device to send a refused write again (AS-25, AS-57).
- Closes when: why the device request queue fills at the line rate is
  explained. The overrun itself, and a refused write that is not sent
  again, are the device firmware's to fix.

## KI-21: an I2C `clock-frequency` that differs from the channel's speed fails the probe

- Effect: an I2C channel whose firmware node sets `clock-frequency` to
  a value other than the bus speed the device reports gets no
  adapter; the log shows "clock-frequency <n>, channel runs at <m>".
- Why: the host cannot set the bus speed, and the driver treats the
  property as a check.
- Closes when: the property is a limit: the adapter runs at the
  device's speed at or below it and warns above it, with tests.

## KI-23: GPIO interrupts take edge triggers only; a level trigger is refused

- Effect: a GPIO input can be an interrupt with a rising, falling or
  both-edge trigger. A level trigger is refused with `EINVAL`, so a
  client whose firmware node asks for a level-triggered interrupt from
  a GPIO line fails to request it and does not probe. A client
  described with an edge trigger works.
- Why: the specification does not say how a device notifies a level
  that stays asserted after the host handled it.
- Closes when: level triggers are served, with that notification rule
  settled and with tests.
- The interrupts were tested with unit-test fakes only: no device we
  have raises GPIO interrupts.

## KI-24: a firmware node whose children match no channel gives no warning

- Effect: an interface node (device tree) or object (ACPI) whose
  children name no channel the device has, for example a board
  description in the layout of the older LSTP driver or with wrong
  channel numbers, gets no warning: the function devices bind without
  the board's settings and without their clients.
- Why: the driver does not yet check that each child of the interface
  node was used.
- Closes when: the warning is added (planned with the matching of
  channel nodes by channel name).

## KI-25: on LSTP, vendor, reserved and OEM channel types are dropped

- Effect: an LSTP device's vendor MMIO (0x06), reserved (0x07-0x7E) or
  OEM (0x7F-0xFF) channel gets no device, is missing from the bind
  line, and is logged once as "channel <n>: LSTP type <t> not
  supported, ignored"; the other channels bind as usual. On an
  OBMF-ICP device such a channel is listed "(no driver)".
- Why: the LSTP translation keeps only the channel types that have an
  OBMF-ICP type.
- Closes when: LSTP keeps those channels and lists them "(no driver)",
  as for OBMF-ICP devices.

## KI-26: the sysfs attributes have no ABI documentation

- Effect: `obmf/name` on the interface and `name` and `channel` on
  each function device work, but no `Documentation/ABI/` file states
  their paths, contents and stability.
- Why: the tree has no `Documentation/` yet.
- Closes when: the ABI entries are written with the documentation.

## KI-27: a discovery running at system suspend can add function devices after their parent is prepared

- Effect: if a system suspend starts while discovery runs (up to 30 s
  after a plug, a module load or a resume), the kernel can warn
  "parent ... should not be sleeping", and the new function devices
  bind only at resume.
- Why: discovery runs in a work item after probe; the kernel's
  suspend preparation waits for probes, not for that work, which the
  interface's own suspend stops later.
- Closes when: discovery is stopped before the suspend preparation, or
  a suspend during discovery on a device shows no warning. Not seen on
  a device so far.

## KI-28: on LSTP, an I2C bus recovery through the channel interface holds the channel 0 lock for a whole round trip

- Effect: none in this drop: no function driver can start a recovery
  (the core does not export that interface, and `i2c-obmf` offers no
  bus recovery). Once one can, a recovery on an LSTP I2C channel makes every other
  channel's status read or control update on that device wait for its
  reply, up to the default 1 s.
- Why: the lock covers the channel header's read-modify-write, and on
  LSTP the recovery is the one such update that talks to the device.
- Closes when: the recovery runs outside the lock, with a case that
  shows a second channel does not wait.

## KI-29: native message buffers follow the device's transfer sizes uncapped, for every channel, bound or not

- Effect: a native OBMF-ICP device that declares large transfer sizes
  (up to 64 KiB) and many channels makes the host allocate up to five
  buffers of that size per channel at bind, also for channels no
  function driver uses; with 255 such channels about 80 MiB, and the
  bind can fail for lack of memory. LSTP devices are not affected.
- Why: the transfer sizes of the functional descriptor are taken as
  the limits with no upper cap, and buffers are allocated at discovery.
- Closes when: the sizes are capped and unbound channels get no buffers.

## KI-30: a KUnit build runs every suite at each load of obmf.ko, and the function-driver suites show fake devices to user space while they run

- Effect: on a kernel built with the obmf KUnit option, every load of
  `obmf.ko` runs the unit tests, and while the I2C, serial, GPIO and
  SPI suites run, user space sees a fake I2C adapter, a
  `/dev/ttyOBMF<n>`, a GPIO chip and an SPI controller for a few
  seconds. No real device is touched.
- Why: the tests are still inside `obmf.ko`; moving them into their own
  test module is planned.
- Closes when: the tests move to a test module, so loading `obmf.ko`
  runs none.

## KI-32: a system suspend while the device does not answer can leave SPI transfers failing until a rebind

- Effect: if the device stops answering while SPI messages are queued
  and the system suspends, the SPI controller's suspend can fail with
  `EBUSY` ("queue stop failed"); the suspend is aborted, and every
  synchronous SPI transfer on that channel then fails with `ESHUTDOWN`
  until an unbind and bind of `obmf.spi.<n>`, or a later suspend and
  resume of the system.
- Why: the SPI core marks the controller suspended even when its queue
  did not stop, and the device whose suspend failed is not resumed.
  Other USB SPI bridges in the kernel behave the same way.
- Also: a device that still answers, but slowly, with a long queue of
  SPI messages can make the suspend fail the same way (found by
  reading the code, not tested).
- Closes when: the queued messages are ended before the controller
  suspends, with a test that suspends with a silent device.

## KI-33: a bulk OUT stall drops the pending characters of every serial port whose send it catches

- Effect: when the device stalls its bulk OUT endpoint, the message
  that met the stall fails, and so does every message queued on the
  endpoint behind it and every message sent until the halt is
  cleared, with `EPIPE`, although none of them left the host. A serial
  port whose write fails this way drops those characters: "dropped
  <n> bytes: -EPIPE", counted in `events/tx_dropped` and the port's
  `tx_dropped`. With several busy serial ports, one stall drops the
  pending characters of each. Requests on other channels fail too (an
  I2C or SPI transfer with `EIO`); their callers see the error. Before drop
  5 only the message that met the stall failed, and the others waited
  behind it. Found by reading the code; not seen in a test.
- Why: a message is never sent twice (AS-6), and the driver does not
  tell the messages that never left the host from the one that
  stalled; whether the host sends a message again after a stall is
  open in the specification.
- Until then: a bulk OUT stall is a finding ("bulk out stalled" in the
  log, `events/out_stalled` above 0); please report it with the
  device's firmware version.
- Closes when: the specification says whether the host resends after
  a stall, and the serial driver sends again the characters that never
  left the host.
- See AS-6.

## KI-34: on some LSTP firmware the chip select release after a failed SPI transfer is refused

- Effect: after an SPI transfer fails (a timeout, an error status, a
  malformed answer), or when a removed client's message finds its chip
  select still held, the driver sends one SPI WRITE with no data and
  only the de-assert bit to release the chip select. The LSTP firmware
  of the development microcontroller board answers it with status 0x01
  (invalid), so the log has "channel <n>: device error 0x1" and
  `chip select <n> not released: -EIO`, and the channel's
  `status_errors` counts it. The client gets the error of its own
  transfer. Whether the chip select was released is not known; the
  next message asserts it again. Not seen after a successful transfer.
- Why: LSTP 1.0 (4.2.2) gives no minimum size for a WRITE and no other
  request to release a chip select; this firmware does not take a
  WRITE of 0 bytes. See AS-76.
- Until then: the warning after a failed transfer on such firmware is
  expected; a release failure after a successful transfer is a finding.
- Closes when: the specification or the firmware authors say how a
  host releases a chip select, and the firmware takes it.

## KI-35: a serial break on the USB4715 board's LSTP UART succeeds without a break

- Effect: `tcsendbreak()` (TCSBRK) on that serial port returns 0, but
  the device answers the Generate Break with "not supported" (LSTP
  status 0x06) and sends no break. The channel's `status_errors`
  counts one for each break; a debug line gives the status. No data is
  lost.
- Why: LSTP 1.0 (4.5.2) asks a device to ignore an event flag it does
  not support, without an error; this firmware refuses it. The driver
  returns "Operation not supported" for the refusal, which the kernel's
  tty layer turns into success for a port that cannot send a break.
  See AS-73.
- Closes when: the firmware ignores the flag (or sends a break), or
  the tty layer gets a way to report that a port has no break.

## Known limits without a number yet

- There are no allocation-failure tests yet.
- The USB transport's bulk OUT path (one transfer per message, its
  cancel at a timeout, the halt clear) has no test that runs it on a
  device that takes no data or stalls. Unit tests without a device
  check the order in which a send is refused, the stall mark, the
  errno of a cancelled transfer, the suspend wait and the stop; the
  transfers themselves, a real stall and its clear are not tested.
- No GPIO drive or bias can be set; each line works as the device
  configures it (see KI-17).
