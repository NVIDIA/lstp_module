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
From drop 5 on, `README.md` describes only the latest drop's changes;
the earlier ones are here.

| drop | date | commit |
|---|---|---|
| 5 | 2026-11-19 | this commit |
| 4 | 2026-11-13 | `38b3dca` |
| 3 | 2026-11-12 | `5393048` |
| 2 | 2026-11-12 | `4fd340c` |
| 1 | 2026-11-06 | `7e2fc98` |

## Drop 5 (2026-11-19)

Added:

- The SPI controller driver, `spi-obmf.ko`, on OBMF-ICP and LSTP
  devices: one SPI controller per SPI channel, with the chip select
  carried in each request, so the device asserts and releases it.
  The mode, bits per word and clock are the channel's configuration,
  read at bind; a client that asks for another mode or a slower clock
  is refused at setup (a faster clock is lowered to the channel's by
  the SPI core). A transfer longer than the channel's limit is split
  with the chip select held. Transfers the protocol cannot carry are
  refused with `EINVAL`, with a debug line only, before anything is
  sent (a message that starts or ends with a transfer without data, a
  transfer without data that has `cs_change`, a delay between words, a
  delay on a transfer that releases the chip select or ends the
  message, a transfer without data with a delay right after one that
  releases the chip select, `cs_off`). After a
  failed transfer the chip select is released with a request that
  carries no data; the failed data is never sent again. At an unbind
  the SPI core removes the clients first; a message a removed client
  left queued fails with `ESHUTDOWN` without being sent (the SPI core
  logs "SPI transfer failed: -108" for it), so the unbind ends within
  the SPI core's wait for its queue whatever the device does, as long
  as the host cancels a timed-out transfer in under about 0.5 s
  (`ASSUMPTIONS.md`). A first
  version: see `README.md` for what it supports and refuses.
- `auto_bind_spidev`, a module parameter for development and
  debugging only, off by default: with it, each chip select of a
  channel that has no firmware description of its clients gets a
  spidev device. A board's firmware description (device tree, ACPI)
  is the supported way to bind SPI clients. Built out of tree only;
  `make OBMF_SPI_SPIDEV=n` leaves it out.
- Kconfig entries for the function drivers (`I2C_OBMF`, `OBMF_TTY`,
  `GPIO_OBMF`, `SPI_OBMF`) and a `.kunitconfig` that builds every
  suite, with the LSTP dialect.
- A debug line with the status an LSTP device sent next to the
  OBMF-ICP status it became, on every response that carries an error
  (dynamic debug), so two LSTP statuses that map to one OBMF-ICP
  status can be told apart.
- Unit tests: unbind and system suspend during discovery; a failed
  discovery and a failed resume; a device request answered while the
  core stops; a dialect result past its buffer; discovery warnings
  of a broken device; every waiting request woken at an unplug; I2C
  transfers refused while suspended and zero-length I2C messages
  refused; a GPIO interrupt armed again after a rebind; the sysfs
  names.
- Unit tests no longer bind real devices: the test drivers match
  only devices the tests create, and the function-driver tests use
  their own fake devices. New cases: an unbind while a request is
  kept, the serial device request length bounds, and suspend and
  resume of a failed or unbinding interface. The SPI driver's cases:
  probe refusals, setup, transfers, chip select handling, refused
  messages, the split at the limit, the errno per status, the release
  after an error, an unbind while the device does not answer, unplug
  and suspend.
- Unit tests of the unbind: a function driver unbound while the
  device does not answer, on the core and I2C, each request ending on
  its own timeout; a reply lost during the unbind, which fails no later
  request; an answer whose send times out, the next one still sent;
  an unbind of the whole interface, which stops at its first timeout.
  Of the SPI unbind: while the device does not answer, while other
  channels' sends take their whole timeout (one of them at the longest
  timeout, as a serial port's after timeouts), with a slow device and
  a long queue on the SPI function and on the whole interface, and a
  removed client's messages failing while another client's go on. Of
  the send bound: a function driver's unbind while other channels'
  sends take their whole timeout; a send that takes its whole wait; a
  serial port's wait while other channels' sends time out. Of the USB
  transport, without a device: the order in which a bulk OUT
  submission is refused (stopped, suspended, stalled), the stall mark,
  the errno of a cancelled transfer, a suspend that waits for the send
  in flight, and the stop.

Changed:

- A function driver's unbind while the device does not answer: each
  request of its teardown still ends on its own timeout (a response
  wait, by default 1 s, after at most the hold an earlier timeout
  leaves), and now whatever the other channels send (below). A
  timeout fails no later request, the device's own requests are still
  answered, and the other channels go on. A request waits only for the
  one in flight on its own channel. An unbind of the whole USB
  interface still stops the device at its first timeout ("no response
  during unbind, device stopped").
- USB transport: each message is sent with its own bulk OUT transfer,
  and no lock is held across a transfer. A send waits for its
  transfer at most its own timeout and then cancels it, whatever the
  other channels send, so a channel's request is bounded by its own
  waits. Before, one transfer was in flight at a time, and on a device
  that took no data every send held the others for its whole timeout:
  a request could wait for several other channels' timeouts before its
  own started. Messages of one channel keep their order; messages of
  different channels may now be queued on the endpoint together, as
  OBMF-ICP over USB 1.0, 6 allows.
- USB transport: a bulk OUT stall is cleared from the transport's
  recovery work, as a bulk IN stall is, instead of inside the send
  that met it; the send that met the stall fails with `EPIPE` at once.
  Before the clear, every message queued on the endpoint is cancelled,
  and until the clear succeeds every send fails with `EPIPE`, sending
  nothing; on I2C and SPI that is `EIO`. A failed clear is tried again every
  250 ms; ten failures in a row stop the device ("transport failed").
  Before, the send cleared the halt itself, which could take up to
  5 s, with every other send waiting.
- USB transport: a request can fail with `ENOMEM`: the transfer of
  each send is allocated for it, and a submission the USB core refuses
  for lack of memory is `ENOMEM` too (other refused submissions stay
  `EIO`). A system suspend waits for every send in flight, each within
  its own timeout.

- Discovery runs after probe, in a work item: a device that does not
  answer no longer holds probe, the module load or the parent's
  device lock. `obmf/name` reads empty until discovery has read the
  device's name. An unbind ends a running discovery at its next
  request or in the wait between passes; a system suspend ends it the
  same way, and resume runs it again.
- A failed discovery releases the interface (the driver unbinds from
  it), so a replug or a manual bind tries again; before, the
  interface stayed bound with no function device. The same happens
  when a resume that has to run discovery again fails. The log line
  "discovery failed: <errno>" is then the only record, since the
  interface's counters go with the binding.
- A timeout status from the device (0x02, or 0x40 on I2C, I3C and
  serial channels) gives function drivers `ETIME`; no answer in time
  stays `ETIMEDOUT`. I2C clients, GPIO users and a serial break see
  `ETIMEDOUT` for both, as before. The serial "transmit stalled" line
  names the reason "timeout status" or "no answer" (before: "timed
  out").
- The configuration warnings of one bind are capped: the first ten
  are logged, then one line "<n> more configuration warnings not
  logged". Before, they shared the rate limit of 10 lines per 5 s per
  place in the code with every other device.
- LSTP: an I2C write-read whose write part is at the adapter's write
  limit now fits in one message; a plain write's limit is 2 bytes
  lower (57 instead of 59 bytes at 64-byte packets).
- LSTP: the status "not supported" is reported as the OBMF-ICP
  status "invalid", and a lost arbitration on a channel other than I2C
  as a permanent error; the errnos do not change (`EOPNOTSUPP`,
  `EIO`).
- LSTP: a channel's configuration is no longer kept as read (up to
  32 KiB per channel) beside its translation.
- A control-register update with an empty mask sends nothing.
- The serial driver's SPDX identifier is `GPL-2.0-only`, as in the
  other files (the same licence, written the same way).

Fixed:

- The core and the serial driver did not build on Linux v6.12.0
  itself (later 6.12 stable releases were not affected).
- The driver did not build on Linux 6.18 stable releases whose
  `kzalloc_obj()` takes the allocation flags as a required argument
  (seen on v6.18.34); drop 4 had the same problem. Every call now
  passes the flags. Kernels whose `kzalloc_obj()` makes the flags
  optional, and kernels without it, were not affected.
- On a device that took no data, a request waited for the sends of
  other channels before its own started (see Changed): a function
  driver's unbind could last several timeouts.
- KI-31, found and fixed during this drop's development (no earlier
  drop had the SPI driver): an SPI unbind (an unbind of `spi_obmf`,
  `rmmod spi_obmf`, or an unbind of the USB interface) while a client
  had messages left queued and the device answered slowly or not at
  all could outlast the SPI core's wait for its queue; the unbind then
  freed the driver's buffers while a message was still running, a use
  after free that can crash the host (seen in a unit test with KASAN).
  Now the messages a removed client left fail at once (Added, the SPI
  driver).
- On LSTP, an SPI write answered with success and data failed with
  `EPROTO`, found in this drop's hardware runs (no earlier drop had
  the SPI driver): LSTP 1.0 (4.2.2) gives the answer to a write as the
  header alone, and LSTP firmware has been seen to add one data byte,
  so every write-only transfer failed on it, a half-duplex message
  (a command written, then a read) among them. Now the status decides:
  such a write succeeds with nothing read, and the data is dropped
  with a rate-limited debug line. The rule for this answer: within
  LSTP 1.0 a longer answer is taken and its extra bytes ignored; an
  error status, a wrong channel or an answer longer than the request's
  buffer still fails.
- A control-register update with an empty mask dereferenced a NULL
  pointer on an LSTP device, and on an OBMF-ICP device wrote the
  register back unchanged, which can trigger a write-1 action bit. No
  function driver in the drops makes such an update.
- An answer to a device request could be started after the core had
  stopped; the USB transport refused it, now the core does.
- The length a dialect returns for an encoded or decoded message is
  checked against the buffer it was given; a wrong one warns once and
  the message fails, instead of being used.
- A failure of the USB probe right after its first allocation freed a
  lock that was not set up yet.
- Unit-test fakes stop on a message too short to decode, before they
  read past it.

Known issues:

- Closed: none.
- New: KI-24 (a firmware node whose children match no channel gives no
  warning), KI-25 (on LSTP, vendor, reserved and OEM channel types are
  dropped, not listed "(no driver)") and KI-26 (the sysfs attributes
  have no ABI documentation), found after drop 4 and present in it
  too; KI-27 (a discovery running at system suspend can add function
  devices after their parent is prepared), which comes with discovery
  after probe.
- New: KI-28 (on LSTP, an I2C bus recovery through the channel
  interface holds the channel 0 lock for a whole round trip; nothing
  calls it yet) and KI-29 (native message buffers follow the device's
  transfer sizes, for every channel), present in drop 4 too; KI-30 (a
  KUnit build runs every suite at each load of `obmf.ko`).
- New: KI-32 (a system suspend while the device does not answer, or
  answers slowly with a long SPI queue, can leave SPI transfers
  failing until a rebind), which comes with the SPI driver. KI-31 was
  found and fixed before this drop (Fixed, above).
- New: KI-33 (a bulk OUT stall drops the pending output of every
  serial port whose send it catches), which comes with the
  per-message bulk OUT transfers.
- New: KI-34 (on some LSTP firmware the chip select release after a
  failed SPI transfer is refused) and KI-35 (a serial break on the
  USB4715 board's LSTP UART succeeds without a break), found in this
  drop's hardware runs: KI-34 comes with the SPI driver; KI-35 holds
  for earlier drops too (no earlier run sent a break on that board).
- Not an issue: the build on 6.18 stable releases with the strict
  `kzalloc_obj()` is fixed in this drop (Fixed, above).
- Changed: KI-9 now reads "only the I2C, serial, GPIO and SPI function
  drivers exist".
- Changed: KI-14 (a channel that answers busy during discovery): on
  OBMF-ICP the failed discovery now releases the interface instead of
  leaving it bound.

Tested on Linux 6.12 (arm64) with a development microcontroller board
running OBMF-ICP firmware and then LSTP firmware (I2C, serial, GPIO
with one output looped to one input and two cable pulls on both, and
on LSTP SPI through spidev), on Linux 6.18 (arm64) with a USB4715-based
LSTP board (I2C, serial and GPIO on the hub's own LSTP interface; SPI
reads of the two SPI NOR flash chips of the board's LSTP
microcontroller), with device tree and ACPI descriptions of an I2C
and a serial channel in an arm64 guest at v7.3-rc6, and with the KUnit
suites on x86_64 at v7.3-rc6 (208 cases, 289 with LSTP) and on x86_64
at Linux 6.12 in QEMU, before the LSTP SPI write fix (Fixed, above;
one more LSTP case) (v6.12.0: 277 passed, 12 skipped for interfaces
6.12 lacks; the Raspberry Pi 6.12.112 source: the same 12 skipped,
275 passed, two GPIO cases fail, see `README.md`). The runs on
OBMF-ICP firmware, on the USB4715-based board and in the guest used
this drop before the LSTP SPI write fix, which none of them reaches;
the LSTP firmware runs used this drop. Built against Linux v6.18.34,
a 6.18 stable release with the strict `kzalloc_obj()`, without and
with LSTP.

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
