# OBMF-ICP over USB: Linux drivers (evaluation drop 5)

This is a Linux driver for USB devices that implement the OCP Open
Boot and Management Framework Interface Consolidation Protocol
(OBMF-ICP 1.0.0) over USB (OBMF-ICP over USB 1.0). It discovers the
channels of the device through channel 0 and gives each channel that
has a function driver its own device on the auxiliary bus. Function
drivers for I2C controller, serial and GPIO channels are included,
and for SPI controller channels (a first
version).

As an option, the same driver also serves devices that speak LSTP 1.0.
Their messages are translated to OBMF-ICP where they enter and leave
the driver, so the function drivers and user space see OBMF-ICP only.

## Status

This is the fifth evaluation drop of work in progress toward
upstream Linux. It is not a release. Module names, device names,
sysfs and debugfs files, and the in-kernel interface for function
drivers can still change, before or during upstream review. We share
it to get your feedback on the design, the requirements on devices
and firmware descriptions, and the behaviour on your hardware.

## Modules

| module | source | what it does | binds |
|---|---|---|---|
| `obmf.ko` | `drivers/usb/misc/obmf/` | the USB transport, the message layer, channel 0 and discovery, and the function devices; optionally the dialect support, LSTP, and the KUnit cases | the OBMF-ICP USB interface (class 0xef, subclass 0x09, protocol 0x01); LSTP interfaces when built with LSTP |
| `i2c-obmf.ko` | `drivers/i2c/busses/i2c-obmf.c` | one I2C adapter per I2C controller channel | auxiliary devices `obmf.i2c` |
| `obmf_tty.ko` | `drivers/tty/obmf_tty.c` | one `/dev/ttyOBMF<n>` per serial channel | auxiliary devices `obmf.serial` |
| `gpio-obmf.ko` | `drivers/gpio/gpio-obmf.c` | one GPIO chip per GPIO channel, with edge interrupts on its inputs | auxiliary devices `obmf.gpio` |
| `spi-obmf.ko` | `drivers/spi/spi-obmf.c` | one SPI controller per SPI channel | auxiliary devices `obmf.spi` |

The function drivers use symbols of `obmf.ko`, so load `obmf.ko`
first. `gpio-obmf.ko` is built only against a kernel with
`CONFIG_GPIOLIB_IRQCHIP`, `spi-obmf.ko` only against one with
`CONFIG_SPI_MASTER`.

## The tree

The sources sit where they would in a kernel tree:

- `drivers/usb/misc/obmf/`: the core, with its `Kconfig`, `Makefile`
  and `.kunitconfig`;
- `include/linux/usb/obmf.h`: the interface for function drivers;
- `drivers/i2c/busses/i2c-obmf.c`, `drivers/tty/obmf_tty.c`,
  `drivers/gpio/gpio-obmf.c` and `drivers/spi/spi-obmf.c`:
  the function drivers;
- `compat/obmf-compat.h`: what an out-of-tree build needs from
  kernels older than the one the code is written for;
- `Kbuild` and `Makefile`: the out-of-tree build.

The tree holds no other build or check scripts.

## Build

Out of tree, against the headers of the running kernel or of another
kernel build:

```sh
make                              # /lib/modules/$(uname -r)/build
make KDIR=/path/to/kernel/build   # another kernel
make clean
```

The switches go on the `make` line:

| switch | effect |
|---|---|
| `OBMF_DIALECTS=y` | builds the dialect support, for devices whose wire protocol is translated to OBMF-ICP |
| `OBMF_LSTP=y` | also binds LSTP devices; implies `OBMF_DIALECTS=y` |
| `OBMF_DEBUG=y` | builds the `dev_dbg()` messages in, for kernels without dynamic debug |
| `OBMF_KUNIT_TEST=y` | builds the KUnit cases into `obmf.ko`, with the LSTP cases when `OBMF_LSTP=y` is set too; the kernel needs `CONFIG_KUNIT` and `CONFIG_DTC`. **For test kernels only: do not load a KUnit build on a system that drives real devices** |

Without `OBMF_DIALECTS=y` or `OBMF_LSTP=y`, `obmf.ko` holds no dialect
code at all: it serves OBMF-ICP devices only. `Kbuild` also has an
`OBMF_CHECK_NAMES=y` switch, a source check for the maintainers; it
needs a script that is not in this drop, so leave it unset.

`spi-obmf.ko` has the module parameter `auto_bind_spidev` (default
off), in the out-of-tree build only. **It is for development and debugging
only, not for production**: with `auto_bind_spidev=1`, each chip
select of an SPI channel that has no firmware description of its
clients gets a spidev device. Describe SPI clients in device tree or
ACPI instead (`REQUIREMENTS.md`). `make OBMF_SPI_SPIDEV=n` leaves
the parameter out.

The modules are out of tree and unsigned, so the first `insmod`
taints the kernel. `TESTING.md` covers loading and checking them.

In a kernel tree, `drivers/usb/misc/obmf/Kconfig` describes `obmf.ko`
and its options. The same `Kconfig` has the entries of the I2C,
serial, GPIO and SPI drivers (`I2C_OBMF`, `OBMF_TTY`, `GPIO_OBMF`,
`SPI_OBMF`); a kernel tree that takes them moves each entry to the
`Kconfig` of the directory its driver sits in.
`.kunitconfig` builds every suite. `REQUIREMENTS.md` covers the kernel
versions and the Kconfig options.

## What changed since drop 4

`CHANGELOG.md` lists the changes, the fixed and new known issues and
what was tested for every drop; this section describes drop 5's
changes in full.

SPI channels, new in this drop (a first version):

- `spi-obmf.ko` gives each SPI channel an SPI controller, on OBMF-ICP
  devices and on LSTP devices. Each transfer is one request: a write
  and read of the same length (full duplex), a write, or a read. The
  chip select travels in each request: the device asserts it before
  the first transfer of a message and releases it after the last one,
  and a `cs_change` on a transfer in the middle releases it and
  asserts it again. The chip select is always released at the end of
  a message; `cs_change` on the last transfer is ignored.
- On LSTP devices a write answered with success and data succeeds,
  with nothing read: LSTP 1.0 (4.2.2) gives the answer to a write as
  the header alone, and LSTP firmware has been seen to add a byte. The
  status decides, and the data is dropped with a rate-limited debug
  line (`ASSUMPTIONS.md`); an error status, or an answer longer than
  the request's buffer, still fails the transfer.
- The settings are the channel's configuration, read at bind and not
  changed by the host: the number of chip selects (1 to 4; on LSTP at
  most 2, a larger number is cut to 2 with one warning), the mode
  bits, 8 bits per word, and one clock. A client is refused at setup
  when it asks for other mode bits, for no chip select, for chip
  select setup or hold times, or for a clock below the channel's; a
  faster clock is lowered to the channel's by the SPI core. For each
  refused setup the SPI core logs an error line (`Failed to setup
  device: -22`, or `setup: unsupported mode bits <bits>` for mode bits
  the channel does not have); the driver's own reason is a debug line,
  and an error line for chip select setup or hold times.
- A transfer is at most the channel's request and response size (less
  the SPI header), at most 65535 bytes, and at most what the clock
  sends in 400 ms, so each request finishes well inside the default
  wait of 1 s. The SPI core splits a longer transfer and keeps the
  chip select asserted across the pieces.
- Refused with `EINVAL` before anything is sent, with a debug line
  only (user space can send such messages at will): a message whose
  first or last transfer has no data, a transfer without data that
  has `cs_change`, a delay between words (`word_delay`), a delay on a
  transfer that releases the chip select (one with `cs_change`, or the
  last), a transfer without data with a delay right after one that
  releases the chip select, and `cs_off`. A channel is refused at
  bind (no controller, one message) when its configuration asks for no
  chip select, ready signalling, dual, quad or octal lines, a chip
  select per word, a word size other than 8 bits, 0 or more than 4
  chip selects, a clock of 0 or below 20 Hz, or sizes too small for
  the header. On LSTP, a configuration without the device count and
  the clock (its first 5 bytes) leaves the channel out at discovery,
  with one warning.
- Errors: a timeout status from the device and no answer in time are
  `ETIMEDOUT`; a refused or failed request gives the errno of its
  status (`TESTING.md` lists them). After a failed transfer that held
  or may have held the chip select, the driver sends one request with
  no data that releases it; the failed data is never sent again.
- Clients bind from the firmware description of the channel: device
  tree children of the channel's `spi` node (`reg` is the chip
  select), or ACPI `SpiSerialBus` resources (`REQUIREMENTS.md`).
  Without them no client binds.
- `auto_bind_spidev=1` (module parameter of `spi-obmf.ko`, built out
  of tree only, off by default) is **for development and debugging
  only, not for production**: each chip select of a channel without a
  firmware description of its clients gets a spidev device
  (`/dev/spidev<bus>.<cs>`), removed at unbind. A board's firmware
  description (device tree, ACPI) is the supported way to bind SPI
  clients. A spidev user that asks for another mode or a lower clock
  is refused at setup, with the SPI core's error line (above); so
  `spidev_test` needs `-s` with the channel's clock (`TESTING.md`), and
  flashrom's `linux_spi`
  programmer needs its `spispeed=` option set to the
  channel's clock when that clock is above flashrom's default speed.
- An unbind of the SPI driver (sysfs `unbind` of `obmf.spi.<id>`,
  `rmmod spi_obmf`) or of the whole USB interface: the SPI core
  removes the clients first, and every message a removed client left
  queued then fails with `ESHUTDOWN` without being sent; the SPI core
  logs "SPI transfer failed: -108" on the client for each. Only the
  message in flight and at most one request that releases its chip
  select are left, so the unbind ends inside the SPI core's wait for
  its queue (about 5 s) whatever the device does, as long as the host
  cancels a timed-out transfer in under about 0.5 s
  (`ASSUMPTIONS.md`): with a device that has stopped answering, after
  about 4 s if it takes the requests and does not answer, or about
  2 s if it does not take them (bulk OUT NAKed), with one warning
  "chip select <n> not released". A chip select whose client is removed
  serves its next client normally. A system suspend while the device
  does not answer is a known issue (`KNOWN-ISSUES.md`).

The unbind of a function driver (new in this drop):

- While a function driver is being unbound (sysfs `unbind` of its
  `obmf.<type>.<id>` device, or unloading its module), each request
  of its teardown goes out over the live device and ends on its own
  timeout like any other request: if the device does not answer, it
  fails with `ETIMEDOUT` after its wait (by default 1 s), after at
  most the hold an earlier timeout on that channel leaves (also 1 s),
  so about 2 s at most per request. A request waits for the one in
  flight on its own channel, never for another channel's: with the
  new USB transport (below) other channels' sends no longer add to
  it. A timeout does not fail the requests after it: a device that
  drops one reply, or answers late, still gets the rest of the
  teardown. The device's own requests on that channel are still
  answered, and the other channels go on. A device that takes a
  request and does not answer logs "channel <n>: no response in <ms>
  ms" and counts `timeouts` for it, at most a hold and a wait (about
  2 s). A device that does not take the request (bulk OUT NAKed, as
  `ASSUMPTIONS.md` expects of a stopped firmware) logs nothing at
  the default level: the send fails after its wait (about 1 s), with
  no hold after it, and counts the channel's `tx_errors` and the
  interface's `tx_urb_errors`.
- An unbind of the whole USB interface still stops the device at its
  first timeout ("no response during unbind, device stopped"), and
  an unplug behaves as before.

The USB transport (new in this drop):

- Each message goes out in its own bulk OUT transfer, and no lock is
  held across a transfer. A send waits for its transfer at most its
  own timeout and then cancels it, whatever the other channels send,
  so a request is bounded by its own waits: before, on a device that
  took no data, each send held the others for its whole timeout, and
  a request (or a function driver's unbind) could wait for several
  other channels' timeouts first. Messages of one channel keep their
  order; messages of different channels can be queued on the endpoint
  together (OBMF-ICP over USB 1.0, 6).
- A bulk OUT stall: the send that met it fails with `EPIPE` at once,
  with "bulk out stalled". The transport's recovery work then cancels
  every message queued on the endpoint (they fail with `EPIPE`; an
  I2C or SPI transfer with `EIO`) and clears the halt; until the clear
  succeeds, every send fails with `EPIPE` and sends nothing. A failed
  clear ("bulk out clear halt failed: <errno>") is tried again every
  250 ms, and ten in a row stop the device ("transport failed"). A
  serial port whose write is cancelled this way drops those
  characters (`KNOWN-ISSUES.md`). Before, the send cleared the halt
  itself, for up to 5 s, with every other send waiting.
- A request can fail with `ENOMEM`: each send allocates its transfer,
  and a submission the USB core refuses for lack of memory is `ENOMEM`
  too (other refused submissions stay `EIO`). A system suspend waits
  for every send in flight, each within its own timeout. Unit cases
  check, without a device, the order in which a send is refused
  (stopped, suspended, stalled), the stall mark, a cancelled
  transfer's errno, the suspend wait and the stop; the transfers
  themselves, a real stall and its clear need a device.
- These changes apply to every channel and every function driver.

The core:

- Discovery runs after probe, in a work item: a device that does not
  answer no longer holds the probe, the module load or the parent
  device's lock. `obmf/name` reads empty until discovery has read the
  device's name. An unbind ends a running discovery at its next
  request or in the wait between passes; a system suspend ends it the
  same way, and resume runs it again.
- A failed discovery releases the interface: the driver unbinds from
  it, so a replug or a manual bind tries again. Before, the interface
  stayed bound with no function device. A resume whose discovery
  fails does the same. The line "discovery failed: <errno>" is then
  the only record: the interface's counters go with the binding.
- A timeout status from the device (0x02, or 0x40 on I2C, I3C and
  serial channels) now reaches function drivers as `ETIME`; no answer
  in time stays `ETIMEDOUT`. I2C clients, GPIO users and a serial
  break see `ETIMEDOUT` for both, as before.
- The configuration warnings of one bind are capped at ten, followed
  by one line "<n> more configuration warnings not logged", so a
  broken device cannot fill the log and cannot hide another device's
  lines.
- The length a dialect returns for a message is checked against its
  buffer; a control-register update with an empty mask sends nothing;
  an answer to a device request is not started once the core has
  stopped.

LSTP devices:

- An I2C write-read whose write part is at the adapter's write limit
  fits in one message; a plain write's limit is 2 bytes lower (57
  instead of 59 bytes at 64-byte packets).
- The status "not supported" is reported as the OBMF-ICP status
  "invalid", and a lost arbitration on a channel other than I2C as a
  permanent error (errnos unchanged). A debug line gives the status
  the device sent next to the OBMF-ICP status it became.

Serial channels: the "transmit stalled" line names the reason
"timeout status" or "no answer" (before: "timed out").

Building: the core and the serial driver build on Linux v6.12.0
itself again. The driver builds on Linux 6.18 stable releases whose
`kzalloc_obj()` takes the allocation flags as a required argument
(seen on v6.18.34; drop 4 did not build there either).
Out of tree, the kernel the modules are built for must itself have
`CONFIG_AUXILIARY_BUS=y`. From Linux 7.0 `CONFIG_ACPI` selects it;
before 7.0, and on any kernel without ACPI, it is set only when
another driver in the kernel's configuration selects it
(`REQUIREMENTS.md`).

KUnit: the I2C, serial, GPIO and SPI drivers have Kconfig entries
(`I2C_OBMF`, `OBMF_TTY`, `GPIO_OBMF`, `SPI_OBMF`), and `drivers/usb/misc/obmf/.kunitconfig`
builds every suite with LSTP. The test drivers in the KUnit cases bind
only devices the tests create, never a real device. A kernel or
module built with the KUnit cases is for test kernels only: each load
of `obmf.ko` runs every suite (`KNOWN-ISSUES.md`).

## What was tested

These modules, built from this tree. The runs on OBMF-ICP firmware,
on the USB4715-based board and in the guest used this tree before its
last change, the LSTP SPI write fix (`CHANGELOG.md`, Fixed), which
changes only how the answer to an LSTP SPI write is read; none of
those runs sends one. The runs on LSTP firmware used this tree.

- Linux 6.12, arm64: a development microcontroller (MCU) board with
  OBMF-ICP firmware: I2C through an at24 EEPROM client (a 256-byte
  pattern read back and compared), a 100,000-byte serial loopback
  compared byte for byte, the GPIO chip's lines, names and
  directions, one output looped to one input set to both levels and
  read back on both, the refused directions, unbind and bind, and two
  cable pulls, one idle and one during I2C reads. All passed.
- The same MCU with LSTP firmware: the same I2C, serial and GPIO
  checks and the same two cable pulls, and its SPI channel through
  spidev (`auto_bind_spidev=1`): the controller and its spidev
  devices, a write, a write and read, and a 4096-byte message on chip
  select 0 (a read of the SPI NOR flash there). All passed. Before the
  fix, the earlier runs of this drop on two LSTP firmware images had
  the write and the 4096-byte message fail, and showed the chip select
  release after a failed transfer refused (`KNOWN-ISSUES.md`).
- Linux 6.18, arm64: a USB4715-based LSTP board. On the hub's own LSTP
  interface: discovery, an I2C scan and EEPROM reads, its GPIO lines
  (outputs looped to inputs, set and read back in pairs, the refused
  directions), the UART, and unbind, idle and during I2C traffic. On
  the board's LSTP microcontroller, whose SPI channel has two SPI NOR
  flash chips: the controller, a spidev device per chip select
  (`auto_bind_spidev=1`), and read-only commands on each chip (JEDEC
  ID 50 times, device ID), with the expected IDs every time. Nothing
  was written to the flash. All passed. The UART: a paced
  100,000-byte loopback compared equal; the unpaced loopback and a
  serial break behave as `KNOWN-ISSUES.md` describes (the break is
  new in this drop's checks).
- Device tree and ACPI, at Linux v7.3-rc6 in an arm64 KVM guest on a
  Linux 6.12 host, with the MCU device passed through, and the
  modules built with LSTP: the I2C and the serial channel matched to
  their nodes or objects, an at24 client created from the I2C node
  and read back, the tty below the serial function device, and a
  4096-byte serial loopback; a disabled serial node, a disabled I2C
  node, an I2C node with another name, no channel nodes, and on ACPI
  no interface object or one that is not present. All passed.
- KUnit on x86_64 at Linux v7.3-rc6: the native suites, with the I2C,
  serial, GPIO and SPI drivers
  built in, and with LSTP; new in this drop: discovery after probe
  (unbind and suspend during discovery, a failed discovery and a
  failed resume releasing the interface once), the timeout status's
  errno per function driver, the cap on configuration warnings, a
  dialect result past its buffer, and the SPI
  driver (probe refusals, setup, transfers, chip select handling, the
  refused messages, the split at the limit, the errno per status, the
  release after an error, an LSTP write answered with data, an unbind
  while the device does not answer,
  unplug and suspend), the unbind (each teardown request on its own
  timeout, a lost reply failing no later request, the interface
  unbind's stop at its first timeout; the SPI unbind with a silent
  device, with a slow device and a long queue, on the SPI function and
  on the whole interface, and a removed client's messages), the send
  bound (unbinds and serial writes while other channels' sends take
  their whole timeout) and the USB transport's bulk OUT refusals,
  stall mark, suspend wait and stop without a device:
  208 of 208 tests passed without LSTP, 289 of 289 with it.
- A build against Linux v6.18.34, a 6.18 stable release whose
  `kzalloc_obj()` takes the allocation flags as a required argument,
  without and with LSTP: rc 0, 0 warnings, all five modules built.

- KUnit on x86_64 at Linux 6.12 (in QEMU), on two kernel sources,
  each with the driver's `.kunitconfig`, which builds LSTP, run on
  this tree before its last change (the LSTP SPI write fix, above,
  which adds one LSTP case; not rerun at 6.12): 289 cases, the 288
  LSTP cases of that tree and the `auto_bind_spidev` case, built only
  with the out-of-tree spidev option, which these runs enabled (at
  6.12 nothing ran without LSTP); on v6.12.0,
  277 passed and 12 were skipped, the cases for kernel interfaces
  6.12 does not have (GPIO set callbacks that return an error, added
  in 6.17, and a KUnit helper that suppresses expected warnings); on
  the Raspberry Pi 6.12.112 source, the same 12 skipped, 275 passed
  and two GPIO cases failed: one because that tree changes gpiolib's
  refusal to drive a line used as an interrupt, so the driver refuses
  it instead, and one where a GPIO read in flight at unbind ends with
  `ENODEV` instead of at its timeout (cause not yet found).

Not tested yet: SPI clients described through a firmware node (device
tree or ACPI; only spidev through `auto_bind_spidev` ran on a device),
a function driver's unbind (SPI among them) while the device does
not answer or answers slowly, on a device (KUnit runs it on the fake
transport), writes to SPI flash, GPIO interrupts
on a device (no device we have raises them; KUnit runs them on the
fake transport), GPIO consumers described through a firmware node,
the report lines on a device (KUnit makes each one on the fake
transport), system sleep and resume on a device (KUnit runs it on the
fake transport, discovery included), a failed discovery on a device,
the USB transport's error handling (transfer errors, a real stall,
the failure after repeated errors, the fault injection attributes:
none of these ran on a device or under fault injection, and KUnit
covers the layers above the transport only), the per-message bulk
OUT transfers on a device that takes no data or stalls its bulk OUT
endpoint (KUnit shows the bound on the fake transport; the runs above
are the transfers' first test on a device, where the device takes
every message), the hold after a real
response timeout, a transport failure and the serial hang-up on a
device, a USB reset, module autoload, two OBMF-ICP devices on one
host, a kernel with KASAN at run time on a device, a
`clock-frequency` that differs from the channel's speed, and the
out-of-tree KUnit build.

`TESTING.md` describes how to repeat these checks on your hardware.

## Documents

| file | what |
|---|---|
| `ARCHITECTURE.md` | the layers, how a channel becomes a device, the request and device request flows, the dialect support, lifetime and power management, the counters |
| `REQUIREMENTS.md` | kernel versions, Kconfig and build switches, the USB match, what the driver needs from a device, how to describe function devices in device tree and ACPI, the interfaces to user space |
| `KNOWN-ISSUES.md` | what is wrong or missing, why, and what closes each item |
| `ASSUMPTIONS.md` | where the specifications leave a point open: the reading this driver takes, the options, what a device needs, and what closes each item |
| `TESTING.md` | KUnit, build and load, what the kernel log shows, I2C, serial, GPIO and SPI checks, unbind and unplug, the counters, what to send with a report |
| `CHANGELOG.md` | per drop: what was added, changed and fixed, the known issues closed, opened and changed, and what it was tested on |

The specification sections these documents cite are those of
OBMF-ICP 1.0.0 and OBMF-ICP over USB 1.0, and for LSTP devices of
LSTP 1.0.

When a report is about a known issue or an assumption, please quote
its number from `KNOWN-ISSUES.md` or `ASSUMPTIONS.md`, and attach what
`TESTING.md` "What to send with a report" lists.
