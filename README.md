# OBMF-ICP over USB: Linux drivers (evaluation drop 2)

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

This is the second evaluation drop of work in progress toward
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

## What changed since drop 1

Serial channels:

- Characters the device refuses (a NACK that accepts none), answers
  busy (status 0x03) or does not answer (no answer, status 0x02, or
  the line timeout 0x40) now stay queued and are sent again 10 ms
  after each refusal, busy answer or timeout, with no limit; a device
  that does not answer is therefore tried about once a second. Drop 1
  dropped them after 20 refusals in a row or at the first busy answer
  or timeout. While the device refuses, `write_room()` shrinks,
  writers block and `tcdrain()` waits. When the same characters have
  waited 5 s, the port logs one "transmit stalled: <reason>, <n>
  characters waiting" warning, and nothing more until the device
  takes them. The driver cannot tell a device held off by flow
  control (CTS) from one that refuses for its own reasons, so a port
  held off for 5 s gets the line too.
- The characters are discarded only on a flush, a hangup, an unbind,
  or a close after `closing_wait` (30 s by default); a close that
  discards some logs "<n> characters not sent at close, discarded",
  for characters in a write still in flight once the device answers
  it or it times out. Other error statuses still drop them with a
  rate-limited "dropped <n> bytes" warning.
- Requests carry no tag: after an answer that comes after the
  timeout, characters can reach the line twice, and the next request
  takes that answer, so characters the device then refuses can count
  as sent and be lost (`ASSUMPTIONS.md`).
- A serial channel now takes a firmware node, as an I2C channel
  does: a child node named `serial` on device tree, or a channel
  object on ACPI (`REQUIREMENTS.md`). A disabled node leaves the
  channel without a tty.
- While the port is closed, a device write that carries characters
  is refused with a NACK and a count of 0, so the device keeps them;
  drop 1 dropped them. A write with events only is taken.
- A refused device write is not acted on: its events are counted and
  passed on once, when the device sends it again and it is taken. A
  refusal for a full receive buffer is counted in `buf_overrun` of
  `TIOCGICOUNT`.
- Carrier Down hangs the tty up, unless `CLOCAL` is set, and is
  counted in `dcd`.
- A break the device refuses is sent again, up to 20 times, 10 ms
  apart; then `tcsendbreak()` fails with `EIO`. Unlike characters, a
  break keeps this limit.
- termios reports `CRTSCTS` when the device uses RTS/CTS.
- Data bits, stop bits or parity out of range give a port at 8N1,
  with a warning; drop 1 gave no port. A line speed of 0 still does.
- At low line speeds a write request carries fewer characters, so
  that its wait fits the 10 s limit of the core.
- No "dropped" warning after an unplug; an open that races an unbind
  fails with `ENODEV`.

I2C channels:

- No bus recovery is registered with the I2C core, also on a channel
  that offers one, so `i2c_recover_bus()` returns `-EBUSY`; a failed
  transfer returns its error, and the client decides. Drop 1 started
  one after a busy answer or a timeout on a channel that offered it.
- SMBus clients with PEC enabled work.

The core:

- A device request that a function driver leaves unanswered gets the
  default answer, with a one-time warning.
- A serial or IPMI device request that cannot be decoded is answered
  with the invalid status (OBMF-ICP 1.0.0, 5.2, Table 4) instead of
  being dropped unanswered.
- A software unbind also stops at the first send timeout, not only at
  the first response timeout.
- A channel whose header and configuration run past the 32-bit
  configuration space is left out as a bad layout.
- Unloading `obmf.ko` waits for the device-link work the function
  devices left queued.
- The channel list in the kernel log at discovery names an I2C target
  channel "I2C-target"; drop 1 cut it to "I2C-targe".

LSTP:

- An I2C configuration of at least 1 byte and a UART configuration of
  at least 7 bytes are accepted; the missing fields read as 0.
- A GPIO channel's configuration is read to an empty slice, and an
  error status after the first slice ends a configuration and keeps
  the channel.
- The host's answer to a device's UART write that carries an error
  status is sent as `LSTP_NOT_SUPP`.

The KUnit suites now also run the I2C and serial drivers on the fake
transport.

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
  from a device tree overlay and from software nodes, and the serial
  transmit retries, the stall and close lines, a suspend and an unbind
  during a retry: 100 of 100 tests passed without LSTP, 149 of 149
  with it.

Not tested yet: system sleep and resume on a device (KUnit runs the
serial driver's suspend and resume on the fake transport), a
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
| `TESTING.md` | KUnit, build and load, what the kernel log shows, I2C and serial checks, unbind and unplug, the counters, what to send with a report |

The specification sections these documents cite are those of
OBMF-ICP 1.0.0 and OBMF-ICP over USB 1.0, and for LSTP devices of
LSTP 1.0.

When a report is about a known issue or an assumption, please quote
its number from `KNOWN-ISSUES.md` or `ASSUMPTIONS.md`, and attach what
`TESTING.md` "What to send with a report" lists.
