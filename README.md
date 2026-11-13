# OBMF-ICP over USB: Linux drivers (evaluation drop 4)

This is a Linux driver for USB devices that implement the OCP Open
Boot and Management Framework Interface Consolidation Protocol
(OBMF-ICP 1.0.0) over USB (OBMF-ICP over USB 1.0). It discovers the
channels of the device through channel 0 and gives each channel that
has a function driver its own device on the auxiliary bus. Function
drivers for I2C controller, serial and GPIO channels are included.

As an option, the same driver also serves devices that speak LSTP 1.0.
Their messages are translated to OBMF-ICP where they enter and leave
the driver, so the function drivers and user space see OBMF-ICP only.

## Status

This is the fourth evaluation drop of work in progress toward
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

The function drivers use symbols of `obmf.ko`, so load `obmf.ko`
first. `gpio-obmf.ko` is built only against a kernel with
`CONFIG_GPIOLIB_IRQCHIP`.

## The tree

The sources sit where they would in a kernel tree:

- `drivers/usb/misc/obmf/`: the core, with its `Kconfig`, `Makefile`
  and `.kunitconfig`;
- `include/linux/usb/obmf.h`: the interface for function drivers;
- `drivers/i2c/busses/i2c-obmf.c`, `drivers/tty/obmf_tty.c` and
  `drivers/gpio/gpio-obmf.c`: the function drivers;
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
and its options. The I2C, serial and GPIO drivers have no Kconfig
entries yet, so they are built out of tree only; `TESTING.md` shows
the three entries the KUnit run adds. `REQUIREMENTS.md` covers the kernel
versions and the Kconfig options.

## What changed since drop 3

GPIO channels, new in this drop:

- `gpio-obmf.ko` gives each GPIO channel whose device owns the lines
  a GPIO chip, on OBMF-ICP devices and on LSTP devices. The lines are
  numbered by their GPIO_INDEX, and an index the configuration does
  not list is a hole; the names are GPIO_NAME. Each line keeps the
  direction the device gives it: a request for the other direction
  fails with `EINVAL`, and setting an input with `EPERM`. Reading an output
  gives the value last set, from its default output on, without a
  request to the device. Get and Set of several lines go in one
  request. A Set the device refuses keeps the values set before and
  is reported as an event, counted in `events/gpio_set_failed`.
- On LSTP devices the GPIO table and the Get, Set and interrupt
  requests are translated; an output with LED support is left out of
  the GPIO chip, with one debug line per channel.
- GPIO interrupts: each input can be an interrupt with **edge triggers
  only** (rising, falling or both); level triggers are refused with
  `EINVAL`. The driver sends the trigger with Set Interrupt Config,
  and the device's Interrupt Notify (on LSTP, IRQ_EVENT) runs the
  handlers. A configuration the device refuses leaves the interrupt
  off and is reported (`events/gpio_irq_config_failed`); an event for
  a line with no interrupt set up is counted
  (`events/gpio_irq_unused`). **The interrupts were tested with
  unit-test fakes only: no device we have raises GPIO interrupts.**
  Reports from devices that do are welcome.

The core:

- A function driver can now take the device requests of its channel
  (a new optional `device_request` operation), count an event of its
  own without a log line (`obmf_fdev_count()`), and report a device
  request it cannot decode (`obmf_fdev_bad_request()`).
- A GPIO Interrupt Notify on a channel without a driver is answered
  success; GPIO commands a device must not send are answered invalid.

Serial channels:

- The wait for an answer now doubles only after a timeout on the
  host, when no answer came in time. A device answer with a timeout
  status is an answer in time: the characters are sent again and the
  wait stays as it is.

## What changed in drop 3

Since drop 2: what the device does wrong is counted in
`usb/obmf/<interface>/events/` and logged through one rate-limited
helper, with errors printed by name; response timeouts, device
errors, bad responses, malformed messages, unsent answers, endpoint
stalls, unhealthy channels and full device request queues are
logged; after a transport failure the core tells each function
driver, and the serial driver hangs its tty up; after a response
timeout, the channel's next request waits for the late response or
for 1 s; serial ports count dropped characters and stalls in
`obmf_tty/<tty>/`, and the serial wait grows after timeouts.

## What was tested

These modules, built from this tree:

- Linux 6.12, arm64: a development microcontroller (MCU) board with
  OBMF-ICP firmware: I2C through an at24 EEPROM client (a 256-byte
  pattern read back and compared), a 100,000-byte serial loopback
  compared byte for byte, the GPIO chip's lines, names and
  directions, one output looped to one input set to both levels and
  read back on both, the refused directions, unbind and bind, and two
  cable pulls, one idle and one during I2C reads. All passed.
- The same MCU with LSTP firmware: the same I2C, serial and GPIO
  checks. All passed.
- Linux 6.18, arm64: a USB4715-based LSTP device (a USB hub controller
  with LSTP management): discovery, an I2C scan and EEPROM reads, its
  GPIO lines (outputs looped to inputs, set and read back in pairs,
  the refused directions), and unbind, idle and during I2C traffic.
  All passed; its UART behaves as `KNOWN-ISSUES.md` describes.
- Device tree and ACPI, at Linux v7.3-rc6 in an arm64 KVM guest on a
  Linux 6.12 host, with the MCU device passed through, and the
  modules built with LSTP: the I2C and the serial channel matched to
  their nodes or objects, an at24 client created from the I2C node
  and read back, the tty below the serial function device, and a
  4096-byte serial loopback; a disabled serial node, a disabled I2C
  node, an I2C node with another name, no channel nodes, and on ACPI
  no interface object or one that is not present. All passed.
- KUnit on x86_64 at Linux v7.3-rc6: the native suites, with the I2C,
  serial and GPIO drivers built in, and with LSTP, including channel
  nodes from a device tree overlay and from software nodes, the GPIO
  lines, holes, names and directions, Get and Set of several lines,
  refused and failed Sets, the edge interrupts with Interrupt Notify
  and Interrupt Notify Datagram, a refused interrupt configuration,
  bad device requests, an unbind with interrupts set up, the LSTP
  GPIO translation, and the serial wait after a host timeout and after
  a timeout status: 147 of 147 tests passed without LSTP, 218 of 218
  with it.

Not tested yet: GPIO interrupts on a device (no device we have
raises them; KUnit runs them on the fake transport), GPIO consumers
described through a firmware node, the report lines on a device
(KUnit makes each one on the fake transport), system sleep and resume
on a device (KUnit runs the serial driver's suspend and resume on the
fake transport), the USB transport's error handling (transfer errors,
a real stall, the failure after repeated errors, the fault injection
attributes: none of these ran on a device or under fault injection,
and KUnit covers the layers above the transport only), the hold after
a real response timeout, a transport failure and the serial hang-up
on a device, a USB reset, module autoload, two OBMF-ICP devices on one
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
| `TESTING.md` | KUnit, build and load, what the kernel log shows, I2C, serial and GPIO checks, unbind and unplug, the counters, what to send with a report |

The specification sections these documents cite are those of
OBMF-ICP 1.0.0 and OBMF-ICP over USB 1.0, and for LSTP devices of
LSTP 1.0.

When a report is about a known issue or an assumption, please quote
its number from `KNOWN-ISSUES.md` or `ASSUMPTIONS.md`, and attach what
`TESTING.md` "What to send with a report" lists.
