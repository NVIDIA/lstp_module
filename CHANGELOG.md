# Changelog

One section per evaluation drop, newest first. Each lists what was
added, changed and fixed, which known issues were closed, opened or
changed (numbers from `KNOWN-ISSUES.md`), and what the drop was tested
on. `README.md` has the full description of the latest drop's changes;
`KNOWN-ISSUES.md` lists only the issues still open.

The drops are commits on the `obmf-drops` branch, each the child of
the one before. Issue numbers that no drop lists: KI-10, KI-12 and
KI-13 were closed before drop 1; KI-16 was found after drop 1 and
fixed in drop 2 (drop 2, Fixed); KI-18, the missing GPIO interrupts,
was closed by drop 4, the first drop with the GPIO driver.

| drop | date | commit |
|---|---|---|
| 4 | 2026-11-13 | `38b3dca` |
| 3 | 2026-11-12 | `5393048` |
| 2 | 2026-11-12 | `4fd340c` |
| 1 | 2026-11-06 | `7e2fc98` |

## Drop 4 (2026-11-13)

Added:

- The GPIO driver, `gpio-obmf.ko`, on OBMF-ICP and LSTP devices: one
  GPIO chip per GPIO channel whose device owns the lines, numbered by
  GPIO_INDEX (an unlisted index is a hole) and named by GPIO_NAME.
  Each line keeps the direction the device gives it; the other
  direction fails with `EINVAL`, setting an input with `EPERM`.
  Reading an output gives the value last set. Get and Set of several
  lines go in one request. A Set the device refuses keeps the earlier
  values and is reported (`events/gpio_set_failed`).
- GPIO interrupts with edge triggers only (rising, falling, both).
  A configuration the device refuses leaves the interrupt off and is
  reported (`events/gpio_irq_config_failed`); an event for a line
  with no interrupt is counted (`events/gpio_irq_unused`). **Tested
  with unit-test fakes only: no device we have raises GPIO
  interrupts.**
- LSTP: the GPIO table, Get, Set and the interrupt configuration and
  events are translated; outputs with LED support are left out of
  the chip.
- The core: a function driver can take its channel's device requests
  (the optional `device_request` operation), count an event of its
  own (`obmf_fdev_count()`) and report a device request it cannot
  decode (`obmf_fdev_bad_request()`). A GPIO Interrupt Notify on a
  channel without a driver is answered success; GPIO commands a
  device must not send are answered invalid.
- `CHANGELOG.md`, this file (added after the drop, with no change to
  the code).

Changed:

- Serial: the wait for an answer doubles only after a timeout on the
  host. A device answer with a timeout status is an answer in time;
  the characters are sent again and the wait stays.

Known issues:

- Closed: KI-22 (a device timeout status on a serial write also
  lengthened the wait).
- New: KI-17 (a GPIO consumer that asks for open drain or open source
  cannot set the line high), KI-19 (LSTP LED pins are not exposed),
  KI-23 (GPIO interrupts take edge triggers only), and the limit "no
  GPIO drive or bias can be set". They come with the GPIO driver.
- Changed: KI-1 now also covers GPIO requests; KI-9 now reads "only
  the I2C, serial and GPIO function drivers exist".

Tested on Linux 6.12 (arm64) with a development microcontroller board
running OBMF-ICP firmware and then LSTP firmware (I2C, serial, and
GPIO with one output looped to one input; two cable pulls on
OBMF-ICP), on Linux 6.18 (arm64) with a USB4715-based LSTP device (I2C
and GPIO), with device tree and ACPI descriptions of an I2C and
a serial channel in an arm64 guest at v7.3-rc6, and with the KUnit
suites on x86_64 at v7.3-rc6 (147 cases, 218 with LSTP).

## Drop 3 (2026-11-12)

Added:

- Reports: what the device does wrong is counted per event in
  `usb/obmf/<interface>/events/` and logged through one rate-limited
  helper (10 lines per 5 s from each place in the code), with errors
  printed by name. Response timeouts, device errors, bad responses,
  malformed messages, unsent answers, bulk endpoint stalls and failed
  halt clears, unhealthy channels at bind, and full device request
  queues are logged; each channel keeps `dreq_max_depth`.
- After a transport failure the core tells each function driver (the
  optional `failed` operation, `obmf_fdev_errno()`); the serial port
  hangs up and an open fails with `EIO` until the interface is bound
  again.
- Function drivers can report events of their own
  (`obmf_fdev_report()`).
- Serial: dropped characters are an event (`events/tx_dropped`);
  each port has `obmf_tty/<tty>/tx_dropped` and `tx_stalls` in
  debugfs.

Changed:

- After a response timeout, the channel's next request waits for the
  late response, which is dropped, or for one default timeout (1 s).
- Serial: each timeout doubles the wait for the answers after it, up
  to 10 s, until the port is opened again.

Known issues:

- Closed: KI-5 (dropped serial characters were not counted readably).
- New: KI-22 (a device timeout status on a serial write also
  lengthens the wait; closed in drop 4).
- Changed: KI-1 is narrower (a late response answers the next request
  only if it comes more than one default timeout late); KI-20 is
  narrower (a full device request queue is now reported).

Tested on Linux 6.12 (arm64) with a microcontroller device on
OBMF-ICP, on Linux 6.18 (arm64) with a USB hub controller with LSTP
management, with device tree and ACPI descriptions of an I2C and a
serial channel in an arm64 guest at v7.3-rc6, and with the KUnit
suites on x86_64 at v7.3-rc6 (127 cases, 178 with LSTP).

## Drop 2 (2026-11-12)

Added:

- Serial channels take firmware nodes, as I2C channels do: a child
  node named `serial` on device tree, or a channel object on ACPI.
- SMBus clients with PEC enabled work.
- The KUnit suites also run the I2C and serial drivers on the fake
  transport.

Changed:

- Serial: characters the device refuses, answers busy or does not
  answer stay queued and are sent again every 10 ms until the device
  takes them, with one "transmit stalled" line after 5 s; they are
  discarded only on a flush, a hangup, an unbind or a close after
  `closing_wait`. Device writes on a closed port are refused, so the
  device keeps them. Carrier Down hangs the tty up unless `CLOCAL` is
  set. A refused break is sent again up to 20 times. Out-of-range
  line settings give a port at 8N1.
- I2C: no bus recovery is registered; a failed transfer returns its
  error and the client decides.
- The core answers a device request that a driver leaves unanswered
  with the default answer, and one that cannot be decoded with the
  invalid status.
- LSTP: shorter I2C and UART configurations are accepted; a GPIO
  channel's configuration is read to an empty slice.

Fixed:

- Serial characters were dropped after 20 refusals in a row or at the
  first busy answer or timeout (found after drop 1).
- An open that raced an unbind; a "dropped" warning after an unplug;
  the channel list at discovery cut "I2C-target" to "I2C-targe";
  unloading `obmf.ko` did not wait for queued device-link work.

Known issues:

- Closed: KI-3 (an I2C timeout status and no response gave the same
  error and both started a bus recovery: no recovery is started now).
- New: KI-20 (serial input can be lost with no kernel message), KI-21
  (an I2C `clock-frequency` that differs from the channel's speed
  fails the probe).
- Removed limit: the I2C adapter quirks and bus recovery path had no
  unit tests.

Tested on Linux 6.12 (arm64) with a microcontroller device on
OBMF-ICP, on Linux 6.18 (arm64) with a USB hub controller with LSTP
management, with device tree and ACPI descriptions of an I2C and a
serial channel in an arm64 guest at v7.3-rc6, and with the KUnit
suites on x86_64 at v7.3-rc6.

## Drop 1 (2026-11-06)

The first drop: the USB core driver (`obmf`) with channel discovery
and one auxiliary device per I2C or serial channel, the I2C adapter
driver (`i2c-obmf`), the tty driver (`obmf_tty`), and the LSTP dialect
(`OBMF_LSTP=y`). I2C channels can be described in device tree, ACPI
or software nodes. Built against Linux 6.12, 6.18 and v7.3-rc6.

Known issues: KI-1, KI-2, KI-3, KI-4, KI-5, KI-6, KI-7, KI-8, KI-9,
KI-11, KI-14, KI-15, and two limits (no unit tests for the I2C adapter
quirks and bus recovery path; no allocation-failure tests).

Tested on Linux 6.12 (arm64) with a microcontroller device on OBMF-ICP
and on LSTP firmware, on Linux 6.18 (arm64) with a USB hub controller
with LSTP management, with device tree and ACPI descriptions in an
arm64 guest at v7.3-rc6, and with the KUnit suites on x86_64 at
v7.3-rc6.
