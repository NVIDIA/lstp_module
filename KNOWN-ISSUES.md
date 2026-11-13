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

## KI-9: only the I2C, serial and GPIO function drivers exist

- Effect: SPI, IPMI, memory device and the other channel types are
  discovered and logged, but get no device. The GPIO driver is new in
  this drop.
- Why: I2C, serial and GPIO come first; the other types follow.
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
  read, an OBMF-ICP device fails to bind ("discovery failed: -16"),
  and an LSTP device binds without that channel, which is logged as
  "channel <n>: status 0x3, ignored". Unbinding and binding the
  interface reads it again.
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

## Known limits without a number yet

- There are no allocation-failure tests yet.
- No GPIO drive or bias can be set; each line works as the device
  configures it (see KI-17).
