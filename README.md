# OBMF-ICP over USB: Linux drivers (evaluation drop)

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

This is an evaluation drop of work in progress toward upstream Linux.
It is not a release. Module names, device names, sysfs and debugfs
files, and the in-kernel interface for function drivers can still
change, before or during upstream review. We share it to get your
feedback on the design, the requirements on devices and firmware
descriptions, and the behaviour on your hardware.

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
| `OBMF_KUNIT_TEST=y` | builds the KUnit cases into `obmf.ko`, with the LSTP cases when `OBMF_LSTP=y` is set too; the kernel needs `CONFIG_KUNIT` |

Without `OBMF_DIALECTS=y` or `OBMF_LSTP=y`, `obmf.ko` holds no dialect
code at all: it serves OBMF-ICP devices only.

The modules are out of tree and unsigned, so the first `insmod`
taints the kernel. `TESTING.md` covers loading and checking them.

In a kernel tree, `drivers/usb/misc/obmf/Kconfig` describes `obmf.ko`
and its options. The I2C and serial drivers have no Kconfig entries
yet, so they are built out of tree only. `REQUIREMENTS.md` covers the
kernel versions and the Kconfig options.

## What was tested

These modules, built from this tree:

- Linux 6.12, arm64: a microcontroller device with OBMF-ICP firmware,
  and the same device with LSTP firmware. On each: I2C through an
  at24 EEPROM client (a 256-byte pattern read back and compared), a
  100,000-byte serial loopback compared byte for byte, and unplugs,
  idle and during I2C traffic. Both firmwares gave the same results.
  These runs used earlier revisions; on this tree the OBMF-ICP run was
  repeated without the unplugs, with the same results.
- Linux 6.18, arm64: a USB hub controller with LSTP management:
  discovery, an I2C scan and EEPROM reads, and unbind, idle and during
  I2C traffic; repeated on this tree, with the same results.
- Device tree and ACPI, at Linux v7.3-rc6 in an arm64 QEMU guest with
  the microcontroller device passed through to it: the I2C channel
  matched to its node or object and an at24 client created from it and
  read back; a disabled channel node, a node with another name, no
  channel nodes, and on ACPI no interface object or one that is not
  present. On this tree the base cases (matching and the at24 client)
  were repeated, with the same results.
- KUnit on x86_64 at Linux v7.3-rc6: the native suites, with the
  dialect support, and with LSTP, including channel nodes from a
  device tree overlay and from software nodes.
- Builds with `W=1` and sparse, with and without LSTP, against
  Linux 6.12 (arm64) and v7.3-rc6 (x86_64).
- A clang build and a 32-bit x86 build.

Not tested yet: system sleep and resume, a `clock-frequency` that
differs from the channel's speed, and the out-of-tree KUnit build.

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

## Feedback

Send feedback, questions and reports to the person who sent you this
drop.

Please quote the issue number from `KNOWN-ISSUES.md` when your report
is about a known issue, and attach what `TESTING.md` "What to send
with a report" lists.
