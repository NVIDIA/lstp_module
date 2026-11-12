# Known issues

What is wrong or missing in this version of the driver, why, and what
closes each item. The numbers are stable: please quote them in
reports. Gaps in the numbering are items already closed, or items
about code that is not in this drop.

## KI-1: a late I2C response after a timeout can answer the next request

- Effect: after an I2C request times out, its late response can
  complete the next request with the same command: a false ACK, or
  data read from the earlier address. It can only happen after a
  timeout.
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

## KI-5: dropped serial characters are not counted readably

- Effect: characters whose write the device answers with an error
  status (other than busy or a timeout) are dropped with a "dropped
  <n> bytes" warning, and characters a close discards with "<n>
  characters not sent at close, discarded"; each stall of the
  transmit path gets one "transmit stalled" line. The totals of
  dropped characters and of stalls cannot be read. (Characters the
  device refuses are no longer dropped: they are sent again until it
  takes them. Device writes that arrive while the port is closed are
  not dropped either: the host refuses them, see KI-20.)
- Why: not built yet.
- Closes when: a readable count, with a test.

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

## KI-9: only the I2C and serial function drivers exist

- Effect: GPIO, SPI, IPMI, memory device and the other channel types
  are discovered and logged, but get no device. A GPIO driver is being
  written; it is not in this drop.
- Why: I2C and serial come first; the other types follow.
- Closes when: each driver is added.

## KI-11: after a real stall, some devices stay stopped until replugged

- Effect: when an endpoint stalls, the driver clears the halt. A
  device whose firmware stops all OUT transfers after a clear-halt
  then needs a replug.
- Why: the driver follows OBMF-ICP over USB 1.0, 8.2 and leaves port
  and device resets to the USB core.
- Closes when: the device firmware keeps working after a clear-halt.
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

## KI-20: serial input can be lost with no kernel message

- Effect: input is lost in two ways, and neither is logged.
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
    only in `buf_overrun`, and refusals when the channel's queue of
    device requests is full count only in debugfs (`rx_dropped`,
    `default_answers`).
- Why: overruns are counted and marked only, refusals are counted
  only, and the driver relies on the device to send a refused write
  again (AS-25).
- Closes when: both cases log a ratelimited warning, with tests, and
  why the device request queue fills at the line rate is explained.
  The overrun itself, and a refused write that is not sent again, are
  the device firmware's to fix.

## KI-21: an I2C `clock-frequency` that differs from the channel's speed fails the probe

- Effect: an I2C channel whose firmware node sets `clock-frequency` to
  a value other than the bus speed the device reports gets no
  adapter; the log shows "clock-frequency <n>, channel runs at <m>".
- Why: the host cannot set the bus speed, and the driver treats the
  property as a check.
- Closes when: the property is a limit: the adapter runs at the
  device's speed at or below it and warns above it, with tests.

## Known limits without a number yet

- There are no allocation-failure tests yet.
