# OBMF-ICP over USB: Linux drivers (evaluation drop 3)

This is a Linux driver for USB devices that implement the OCP Open
Boot and Management Framework Interface Consolidation Protocol
(OBMF-ICP 1.0.0) over USB (OBMF-ICP over USB 1.0). It discovers the
channels of the device through channel 0 and gives each channel that
has a function driver its own device on the auxiliary bus. Function
drivers for I2C controller and serial channels are included.

As an option, the same driver also serves devices that speak LSTP 1.0.
Their messages are translated to OBMF-ICP where they enter and leave
the driver, so the function drivers and user space see OBMF-ICP only.

## Status

This is the third evaluation drop of work in progress toward
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

`i2c-obmf.ko` and `obmf_tty.ko` use symbols of `obmf.ko`, so load
`obmf.ko` first.

## The tree

The sources sit where they would in a kernel tree:

- `drivers/usb/misc/obmf/`: the core, with its `Kconfig`, `Makefile`
  and `.kunitconfig`;
- `include/linux/usb/obmf.h`: the interface for function drivers;
- `drivers/i2c/busses/i2c-obmf.c` and `drivers/tty/obmf_tty.c`: the
  function drivers;
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
| `OBMF_KUNIT_TEST=y` | builds the KUnit cases into `obmf.ko`, with the LSTP cases when `OBMF_LSTP=y` is set too; the kernel needs `CONFIG_KUNIT` and `CONFIG_DTC` |

Without `OBMF_DIALECTS=y` or `OBMF_LSTP=y`, `obmf.ko` holds no dialect
code at all: it serves OBMF-ICP devices only. `Kbuild` also has an
`OBMF_CHECK_NAMES=y` switch, a source check for the maintainers; it
needs a script that is not in this drop, so leave it unset.

The modules are out of tree and unsigned, so the first `insmod`
taints the kernel. `TESTING.md` covers loading and checking them.

In a kernel tree, `drivers/usb/misc/obmf/Kconfig` describes `obmf.ko`
and its options. The I2C and serial drivers have no Kconfig entries
yet, so they are built out of tree only; `TESTING.md` shows the two
entries the KUnit run adds. `REQUIREMENTS.md` covers the kernel
versions and the Kconfig options.

## What changed since drop 2

Reports:

- What the device does wrong is now counted and logged through one
  helper: each event counts in a new debugfs directory of the
  interface, `usb/obmf/<interface>/events/`, one file per event, and
  logs one line, at most 10 lines per 5 s from each place in the code.
  The counters also count what the rate limit kept out of the log.
  Errors are printed by name (`-EPROTO`), or as numbers on a kernel
  without `CONFIG_SYMBOLIC_ERRNAME`; the transport failure line is now
  "transport failed: <errno>, device stopped".
- A response timeout ("channel <n>: no response in <ms> ms"), a
  failure status that says the device could not do the request
  ("channel <n>: device error <status>"), a response that does not fit
  its request ("bad response"), a message the core cannot take
  ("malformed message: ...", "message for unknown channel <n>", "bad
  device request") and an answer that cannot be sent ("answer not
  sent") are now logged. The statuses a function driver acts on (busy,
  timeout, I2C NACK) stay quiet, and so does all of this during
  discovery, where a reply left from an earlier binding is expected.
- A stall of either bulk endpoint logs "bulk in stalled" or "bulk out
  stalled", and a halt clear that fails logs "bulk in clear halt
  failed: <errno>" or "bulk out clear halt failed: <errno>". A failed
  bulk OUT halt clear now counts as a failed transfer.
- A channel that reports HEALTH_STATUS 3 (not ready) or 15 (channel
  error), or sets CHANNEL_ERROR_INDICATOR, still binds, now with one
  warning per bind: "channel <n>: health <h>, error indicator <i>".
- A device request that finds the channel's queue full is reported:
  "channel <n>: device request queue full, request refused" (or
  "request dropped" for one that waits for no answer). Each channel
  has a new counter, `dreq_max_depth`, the most device requests it
  held at once.

Function drivers and the core:

- After a transport failure stops the device, the core tells each
  function driver, through a new optional `failed` operation, and
  `obmf_fdev_errno()` gives the errno a request would fail with. The
  serial driver hangs its tty up then, and an open fails with `EIO`
  until the interface is bound again.
- After a response timeout, the channel's next request waits until
  the late response arrives, which is dropped, or for one default
  timeout (1 s), so that the late response does not answer it. A
  response later than that is still taken by the next request.
- Function drivers can report events of their own through
  `obmf_fdev_report()`, counted with the core's.

Serial channels:

- Characters dropped after an error are reported as an event, counted
  in `events/tx_dropped`, with the line "dropped <n> bytes: <errno>".
- Each port has a debugfs directory, `obmf_tty/<tty>/`, with
  `tx_dropped` (characters dropped after an error or at close) and
  `tx_stalls` (the "transmit stalled" lines).
- Each timeout doubles the wait for the answers after it, up to 10 s,
  until the port is opened again: a device that answers later than the
  default timeout gets one repeat, not one per write. A device answer
  with a timeout status (0x02, or the line timeout 0x40) doubles the
  wait as well (see `KNOWN-ISSUES.md`).

## What changed in drop 2

Since drop 1: serial channels take firmware nodes; the serial driver
keeps characters the device refuses, answers busy or does not answer
and sends them again until the device takes them, with one "transmit
stalled" line after 5 s, refuses device writes while the port is
closed and hangs up on Carrier Down; a refused break is sent again up
to 20 times; out-of-range line settings give a port at 8N1. The I2C
driver registers no bus recovery, and SMBus clients with PEC work. The
core answers device requests that a driver leaves unanswered with the
default answer, and those that cannot be decoded with the invalid
status. LSTP accepts shorter I2C and UART configurations and reads a
GPIO channel's configuration to an empty slice. The KUnit suites also
run the I2C and serial drivers on the fake transport.

## What was tested

These modules, built from this tree:

- Linux 6.12, arm64: a microcontroller device with OBMF-ICP firmware:
  I2C through an at24 EEPROM client (a 256-byte pattern read back and
  compared), a 100,000-byte serial loopback compared byte for byte,
  and unbind and bind. All passed; the device's GPIO channel was
  discovered and left without a driver.
- Linux 6.18, arm64: a USB hub controller with LSTP management:
  discovery, an I2C scan and EEPROM reads, and unbind, idle and during
  I2C traffic. All passed.
- Device tree and ACPI, at Linux v7.3-rc6 in an arm64 KVM guest on a
  Linux 6.12 host, with the microcontroller device passed through, and
  the modules built with LSTP: the I2C and the serial channel matched
  to their nodes or objects, an at24 client created from the I2C node
  and read back, the tty below the serial function device, and a
  4096-byte serial loopback; a disabled serial node, a disabled I2C
  node, an I2C node with another name, no channel nodes, and on ACPI
  no interface object or one that is not present. All passed.
- KUnit on x86_64 at Linux v7.3-rc6: the native suites, with the I2C
  and serial drivers built in, and with LSTP, including channel nodes
  from a device tree overlay and from software nodes, the serial
  transmit retries, the stall and close lines, a suspend and an unbind
  during a retry, and the event reports, the hold after a timeout, the
  growing serial wait, the hang-up after a transport failure and the
  new counters: 127 of 127 tests passed without LSTP, 178 of 178 with
  it.

Not tested yet: the new report lines on a device (KUnit makes each
one on the fake transport), system sleep and resume on a device
(KUnit runs the serial driver's suspend and resume on the fake
transport), a `clock-frequency` that differs from the channel's
speed, and the out-of-tree KUnit build.

`TESTING.md` describes how to repeat these checks on your hardware.

## Documents

| file | what |
|---|---|
| `ARCHITECTURE.md` | the layers, how a channel becomes a device, the request and device request flows, the dialect support, lifetime and power management, the counters |
| `REQUIREMENTS.md` | kernel versions, Kconfig and build switches, the USB match, what the driver needs from a device, how to describe function devices in device tree and ACPI, the interfaces to user space |
| `KNOWN-ISSUES.md` | what is wrong or missing, why, and what closes each item |
| `ASSUMPTIONS.md` | where the specifications leave a point open: the reading this driver takes, the options, what a device needs, and what closes each item |
| `TESTING.md` | KUnit, build and load, what the kernel log shows, I2C and serial checks, unbind and unplug, the counters, what to send with a report |

The specification sections these documents cite are those of
OBMF-ICP 1.0.0 and OBMF-ICP over USB 1.0, and for LSTP devices of
LSTP 1.0.

When a report is about a known issue or an assumption, please quote
its number from `KNOWN-ISSUES.md` or `ASSUMPTIONS.md`, and attach what
`TESTING.md` "What to send with a report" lists.
