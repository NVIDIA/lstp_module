# Requirements

What the driver needs from the kernel, from the device, and from the
platform description, and what it offers to user space.

## Kernel versions

The code is written against the current mainline kernel interfaces.
It was built against Linux 6.12 (v6.12.0 and a 6.12 stable release)
and 6.18 (arm64)
and Linux v7.3-rc6 (arm64 and x86_64); Linux 6.12 is the oldest kernel
it was built for. Older kernels are not supported yet. It builds on
6.18 stable releases whose `kzalloc_obj()` takes the allocation flags
as a required argument (v6.18.34), as on those where they are
optional.

`compat/obmf-compat.h` lets the same sources build against kernels
older than v7.3. The out-of-tree `Kbuild` includes it in every file.
It defines the newer helpers the code uses (allocation helpers, the
scoped firmware node walk, the symbol namespace macros of 6.12, the
default workqueue name, a tty driver flag, and for the GPIO driver
the older form of the set callbacks and, below 6.15, the check of the
line mask on request, and the tty port hang-up helper missing from
v6.12) when the kernel lacks them, and is empty on a
kernel that has them all. `Kbuild` checks the kernel headers, or for
the GPIO line mask the kernel version, for four of them. In a kernel tree the
header is not used.

## Kernel configuration

The kernel needs `CONFIG_USB` and `CONFIG_AUXILIARY_BUS=y`.

Out of tree, the kernel the modules are built for must have
`CONFIG_AUXILIARY_BUS=y` itself: the `select AUXILIARY_BUS` of the
driver's Kconfig entry applies only in a kernel tree. The option has
no prompt, so it is set only when something in the kernel's
configuration selects it. From Linux 7.0 `CONFIG_ACPI` selects it;
before 7.0, and on kernels without ACPI (often the case for arm64
boards and BMCs), it depends on other drivers that select it, and a
kernel may lack it. Without it the modules cannot be built or loaded:
the auxiliary bus functions they call are missing. Check with
`grep AUXILIARY_BUS /boot/config-$(uname -r)` or the build tree's
`.config`; to get it, enable a driver that selects it, or build the
driver in the kernel tree.

The I2C
driver needs `CONFIG_I2C`, the serial driver the tty layer, and the
GPIO driver `CONFIG_GPIOLIB` with `CONFIG_GPIOLIB_IRQCHIP`; out of
tree, `gpio-obmf.ko` is built only against a kernel that has
`CONFIG_GPIOLIB_IRQCHIP`. The SPI driver needs
`CONFIG_SPI_MASTER`; out of tree, `spi-obmf.ko` is built only against
a kernel that has it. `auto_bind_spidev` needs the spidev driver
(`CONFIG_SPI_SPIDEV`).
`CONFIG_DEBUG_FS` gives the counters, `CONFIG_DYNAMIC_DEBUG` the debug
messages at run time, and `CONFIG_FAULT_INJECTION_DEBUG_FS` the
transfer fault injection.

In a kernel tree, `drivers/usb/misc/obmf/Kconfig` has:

| option | type | what |
|---|---|---|
| `CONFIG_USB_OBMF` | tristate | `obmf.ko`; depends on `USB`, selects `AUXILIARY_BUS` |
| `CONFIG_OBMF_DIALECTS` | bool | the dialect support |
| `CONFIG_OBMF_LSTP` | bool | LSTP devices; depends on `OBMF_DIALECTS` |
| `CONFIG_OBMF_KUNIT_TEST` | bool | the KUnit suites; depends on `KUNIT`, selects `DTC` |
| `CONFIG_OBMF_LSTP_KUNIT_TEST` | bool | the LSTP suite; depends on `OBMF_LSTP` |
| `CONFIG_I2C_OBMF` | tristate | `i2c-obmf.ko`; depends on `I2C` and `USB_OBMF` |
| `CONFIG_OBMF_TTY` | tristate | `obmf_tty.ko`; depends on `TTY` and `USB_OBMF` |
| `CONFIG_GPIO_OBMF` | tristate | `gpio-obmf.ko`; depends on `GPIOLIB` and `USB_OBMF`, selects `GPIOLIB_IRQCHIP` |
| `CONFIG_SPI_OBMF` | tristate | `spi-obmf.ko`; depends on `SPI_MASTER` and `USB_OBMF` |

Out of tree, the `make` switches set the same options:
`OBMF_DIALECTS=y`, `OBMF_LSTP=y` (which also sets the dialect
support), `OBMF_KUNIT_TEST=y` (which needs a kernel with
`CONFIG_KUNIT` and `CONFIG_DTC`, and with `OBMF_LSTP=y` adds the LSTP
suite), and `OBMF_DEBUG=y`, which builds every file with `DEBUG`.

The KUnit cases of the function drivers are built only when the
driver is built into the kernel (`CONFIG_I2C_OBMF=y`,
`CONFIG_OBMF_TTY=y`, `CONFIG_GPIO_OBMF=y`,
`CONFIG_SPI_OBMF=y`), so an out-of-tree KUnit build
has none of them. `.kunitconfig` enables every
suite, with the dialects and LSTP. The KUnit build is for test kernels
only: do not load it on a system that drives real devices. The serial
cases that check the kernel log read it from the console tracepoint:
they need `CONFIG_PRINTK` and tracepoints (`.kunitconfig` sets
`CONFIG_FTRACE=y`) and are skipped without them.

## USB match

`obmf.ko` binds a USB interface with:

- class 0xef, subclass 0x09, protocol 0x01, from any vendor
  (OBMF-ICP over USB 1.0, 5.1);
- with LSTP support: class 0xff, subclass 0x3f, protocol 0x01, on the
  products 0955:cf11, 0424:4940 and 174c:2214, and on any product of
  the vendors 0955, 0424 and 174c. The LSTP support follows LSTP 1.0.

The LSTP 1.0 specification is published with the LSTP module at
https://github.com/NVIDIA/lstp_module.

The match entry chooses the protocol, so an interface cannot be bound
through the `new_id` file; the probe refuses it with `-ENODEV`.

## Device requirements: OBMF-ICP

The interface must have:

- a bulk IN and a bulk OUT endpoint (the first of each is used);
- the OBMF-ICP functional descriptor (OBMF-ICP over USB 1.0, 5.2) with
  no multimessage support, a version of 1.x, and write and read
  transfer sizes of at least 64 bytes, the size of the channel-0
  reads.

Messages:

- each bulk transfer carries exactly one message, header included,
  within the transfer sizes of the descriptor;
- a message whose length is a multiple of the endpoint's packet size
  ends with a zero-length packet, in both directions (OBMF-ICP over
  USB 1.0, 6.1). The host sends one;
- every host request gets its response within the caller's timeout,
  1000 ms unless the function driver gives another. The host sends
  one request per channel at a time. After a timeout the host waits
  up to 1000 ms more for the late response, which it drops, before
  the channel's next request; a response later than that is taken as
  the answer to the next request (`ASSUMPTIONS.md`);
- requests and answers of different channels can follow each other on
  the bulk OUT endpoint without waiting for one another: each is its
  own bulk transfer (OBMF-ICP over USB 1.0, 6). The time a message
  waits on the endpoint behind other channels' messages counts against
  its timeout, so a device that takes messages off the bus slowly
  makes busy channels time out (`ASSUMPTIONS.md`);
- after a bulk OUT stall, the host sends one
  `CLEAR_FEATURE(ENDPOINT_HALT)` for the burst; the messages queued
  behind the stalled one are cancelled before the clear and are not
  sent again (OBMF-ICP over USB 1.0, 8.2; `ASSUMPTIONS.md`);
- a device whose firmware stops taking bulk OUT data may NAK it, stall
  it or keep taking it: each host send is bounded by its own timeout
  whichever it does (`ASSUMPTIONS.md`).

Channel 0 (OBMF-ICP 1.0.0, 5.15):

- answers MMIO short reads (5.3) of up to 64 bytes anywhere in the
  device header, the channel header offsets, the channel headers and
  their configuration data;
- has a device header with major version 1;
- has, for each channel, a header whose CHANNEL_NUMBER is the
  channel's index, at an offset after the offset table, not
  overlapping another channel, with at most 32 KiB of configuration
  data, and ending within the 32-bit configuration space; all channels
  together within 256 KiB;
- keeps the device header unchanged while the host reads it; a
  device that changes it is read again, at most three times within
  30 seconds;
- comes up with each channel it wants used already enabled and with a
  valid configuration, as set during manufacturing ("pre-configured in
  Manufacturing and always enabled", OBMF-ICP 1.0.0, 4.3 step 3, p22):
  the driver uses only channels whose CHANNEL_CONTROL has
  CHANNEL_ENABLE and whose CHANNEL_STATUS has CONFIG_VALID. Enabling
  and configuring channels is not the driver's job; it never writes
  CHANNEL_ENABLE. This differs from 4.3 step 2a (p22) and 5.15 (p59),
  where the host enables the channels: a device that waits for the
  host to enable a channel, in particular a consumer channel its boot
  depends on, waits for good, and the driver leaves the channel out
  with "channel <n>: not enabled, ignored".
- reports a channel it wants used as healthy: a kept channel with
  HEALTH_STATUS 3 (not ready) or 15 (channel error), or with
  CHANNEL_ERROR_INDICATOR set, still binds, with the warning
  "channel <n>: health <h>, error indicator <i>" at each bind.

Per channel:

- payload limits large enough for the type's head: at least 3 bytes
  for serial and for GPIO (one line in a request and in a response),
  4 for I2C;
- I2C controller: the configuration data of Table 79, with the bus
  speed index; the bus recovery option is not used: the driver
  registers no bus recovery and writes no CHANNEL-SPECIFIC CONTROL
  (Table 69);
- serial: the configuration data of Table 78 (at least its first 7
  bytes), with a nonzero speed, and a request limit that holds at
  least one character. A speed of 0 gives no tty ("line speed 0").
  Data bits outside 5 to 8, stop bits other than 1 or 2, or a parity
  code above 4 give a tty at 8N1, with the warning "unsupported line
  <data bits><parity><stop bits>, using 8N1"; the device keeps running
  its line as it is;
- GPIO: the configuration data of Tables 76 and 77: GPIO_COUNT of at
  most 4096, and one entry per line with an index listed once and no
  reserved bits set; a line whose direction, default output, drive or
  bias is out of range is left out. The GPIO driver serves channels
  whose DEVICE_ROLE is producer (the device owns the lines).

### Serial channels

The serial driver expects this of a device:

- Answer each host write at once: accept the characters, or refuse
  some or all of them with status 0x00, the NACK bit and the count
  accepted (5.5.2, Table 23). The host sends the rest again 10 ms
  later, with no limit, until the device takes them or the port is
  flushed, hung up or closed. Answer busy (status 0x03) only when
  none of the request's characters was taken: the host sends them
  all again 10 ms later. Use other error statuses only for real
  failures: they drop the characters at once.
- Answer within the wait: the default response timeout (1000 ms) plus
  twice the characters' time on the wire at ten bits a character.
  Without an answer in time the host sends the same characters again
  10 ms later, after waiting up to 1000 ms for the late answer; each
  timeout with no answer doubles the default part of the wait for the
  next answers, up to 10 s in all, until the port is opened again. A
  timeout status is an answer in time: the characters are sent again,
  and the wait stays as it is. A device that answers later than the
  wait and the 1000 ms after it, or that took characters it answers
  with a timeout status, gets them twice, and the late answer is
  taken as the answer to the next try (`ASSUMPTIONS.md`).
- Generate a complete break for each Generate Break request; there is
  no "break off" request.
- Keep a device write that the host refuses, and send it again later.
  The host refuses a write that carries characters while nobody has
  the port open, and any write while its receive buffer is full; such
  a refusal can last as long as the port stays closed. Resend no
  faster than every few milliseconds, and keep the other channels
  going meanwhile. A write with events only is taken while the port
  is closed.
- Send the events of a refused write again with it: the host acts on
  none of them until the write is taken.
- Set the overrun event only for characters lost on their way to the
  host; Carrier Down hangs the tty up unless `CLOCAL` is set.

### GPIO channels

The GPIO driver expects this of a device:

- Keep each line's direction as its configuration gives it; the host
  never asks to change it, and drives only outputs.
- Answer Get Values (5.4.1, Table 19) with the echo and one entry for
  each line asked, in any order, each with data 0 or 1. The host asks
  only for inputs: an output reads the value last set, from its
  default output on.
- Answer Set Values with the echo when it set every line, and with an
  error status otherwise; the host then keeps the values set before
  (`ASSUMPTIONS.md`). A Set with more lines than one message carries
  is split, and stops at the first message that fails.
- Answer Set Interrupt Config (Table 17) with success only for a line
  whose edges it will report, and with an error status otherwise. The host asks for rising,
  falling or both edges, or disable; it never asks for a level.
- Report the edges with an Interrupt Notify or an Interrupt Notify
  Datagram (Table 18) that lists each line that saw its edge, with
  data 0; send nothing for a line after a disable was answered. The
  host answers a Notify before it runs the handlers, and a handler may
  read lines then, so the device must answer GPIO requests while it
  waits for the next answer. One bad entry makes the host refuse the
  whole request, and a Notify is then answered invalid.
- Send no other GPIO request: the host answers the other commands
  invalid.

GPIO interrupts were tested with unit-test fakes only.

### SPI channels

The SPI driver expects this of a device:

- A configuration (Table 84) with 1 to 4 chip selects, a clock of at
  least 20 Hz, 8 bits per word, and mode bits without no chip select, ready,
  dual, quad, octal or a chip select per word; the request and response
  limits larger than the SPI headers (5 and 3 bytes). Otherwise the
  channel gets no controller. The host never writes the
  configuration.
- Write/Read clocks both ways together, with the same length out and
  in (full duplex), and answers with the bytes read.
- The chip select bits of each request are followed: assert before,
  release after, or neither (the chip select stays as it is until the
  next request). A Write with no data and the release bit releases the
  chip select and nothing else; the host sends one after a failed
  transfer.
- An answer within the default wait of 1 s: a transfer is cut to at
  most 400 ms on the wire at the configured clock.

## Device requirements: LSTP

The interface has no functional descriptor. Each message is one bulk
packet, at most 512 bytes, and each endpoint's packet size is at
least 64 bytes; no zero-length packets are sent. Channel 0 is the
management channel of LSTP version 1, read with READ_CONFIG. A
channel's configuration is read up to offset 0x7fff and cut there
with a warning, and all channels together are read within 256 KiB.
Every response wait is at least 1000 ms. A configuration ends at the
first slice shorter than asked; a GPIO table ends at an empty slice,
so a device answers a read at the end of a table with no data. A
configuration answered with an error status after its first slice
ends there; busy, or an error on the first slice, leaves the channel
out. A channel may end at most 683 slices short, and a discovery pass
at most 5461; past either, the channel is left out with "over
budget".

I2C, UART, GPIO and SPI
channels get function devices. An I2C write-read is one byte shorter
on the wire than in OBMF-ICP form and a plain write three; the
adapter's write limit is that of a write-read. An I2C configuration
needs at least its first byte, the speed; the fields it lacks read as
0. The UART configuration starts with the bytes of OBMF-ICP Table 78,
needs at least those 7, and follows the serial rules above: the host
sends a write again that the device answers `LSTP_NACK` (0x04),
`LSTP_BUSY` (0x03) or `LSTP_TIMEOUT` (0x02), and drops one answered
with another error status. The host refuses a device write with
`LSTP_NACK`. A GPIO table (LSTP 1.0, 4.3.1) gives the lines, a line's
index its position across the table's blocks; an output with LED
support is left out of the GPIO chip. GET_VALUE is answered with the
states in the order of the request. SET_IRQ_CONFIG carries one line;
an IRQ_EVENT lists lines whose offsets fit 12 bits, each with a state
of 0 or 1, which the host checks and does not use.
An SPI configuration (LSTP 1.0, 4.2.1) needs at least its first 5
bytes, the device count and the clock; the fields after them are not
used (`ASSUMPTIONS.md`). A shorter one leaves the channel out, with
one warning ("short config"). More than two chip selects are cut to
two, with one warning. SPI requests use the chip select bits of LSTP
1.0, 4.2.2; no posted write is sent. LSTP SPI is taken as mode 0 and 8
bits per word (`ASSUMPTIONS.md`).

## Describing function devices: device tree, ACPI, software nodes

A firmware node is optional. Without one, an I2C channel still gets
its adapter, and clients can be added from user space; a serial
channel still gets its tty, and a GPIO channel its GPIO chip. A node
gives the I2C clients on the channel's bus, a `clock-frequency` to
check, properties a udev rule can read for a serial port, the GPIO
chip's node for consumers of its lines, or a way to leave a channel
unused. I2C controller, serial and GPIO channels are looked up.

The node of a channel is a child of the USB interface's node. On
device tree and with software nodes, it is the child named `i2c` for
an I2C channel, `serial` for a serial channel or `gpio` for a GPIO
channel (the name before any `@`) whose `reg` is the channel number. A child with that `reg` and
another name is ignored with the warning
"channel <n>: node <name> ignored", where `<name>` is the node's name
with its unit address, for example `spi@1`. On ACPI, it is the child
of the interface object whose `_ADR` is the channel number (see the
ACPI example).

A node that is disabled (`status = "disabled"`), or an ACPI object
whose `_STA` says it is not present, means the channel gets no
function device.

A serial channel's node belongs to its function device,
`obmf.serial.<id>`, whose add uevent carries it. The tty below it has
no node of its own. The port is not registered through serdev, so a
device described below a serial node (a Bluetooth or GNSS chip, for
example) is not bound; user space can still open the tty.

A GPIO channel's node belongs to its function device, `obmf.gpio.<id>`,
which is the GPIO chip's parent, so the chip takes that node as any
GPIO chip takes its own: consumers refer to its lines through it, and
`gpio-line-names` in it replaces the names the device gives. A
consumer that takes a line as an interrupt from device tree needs the
node to be an interrupt controller with two cells (the line and the
edge). Only the match of the node to the function device is tested
(KUnit); consumers through a node are not tested yet.

An SPI channel's node belongs to its function device,
`obmf.spi.<id>`, and the SPI controller takes it. On device tree the
clients are its child nodes, with `reg` the chip select and
`#address-cells = <1>`, `#size-cells = <0>` in the channel's node, as
for any SPI controller; on ACPI they are the devices whose
`SpiSerialBus` resource names the channel's object. A client must ask
for the channel's mode and a clock at or above the channel's, and for
no chip select setup or hold time, or it is refused at setup. SPI
clients through a firmware node are not tested yet.

If an I2C channel's node has `clock-frequency`, it must equal the bus
speed in the channel's configuration; otherwise the I2C probe fails with
"clock-frequency <n>, channel runs at <m>". The driver cannot set the
speed.

The I2C clients are the child nodes of the channel's node, or of its
`i2c-bus` child if it has one, as for any I2C adapter on device tree.
On ACPI they are the devices whose `I2cSerialBus` resource names the
channel's object. The speed given in that resource is not checked.

The examples below are derived from the code. The same layout, with
an I2C and a serial channel, was tested in a virtual machine
(`README.md`, "What was tested"), not on a board. `1234` and `5678`
stand for the device's vendor and product IDs.

### Device tree example

The USB device on port 1 of a hub or root hub, configuration 1,
OBMF-ICP on interface 2; channel 1 is an I2C controller with an
EEPROM at 0x50, channel 2 is a serial channel, and channel 3 is an
I2C controller left unused:

```dts
device@1 {
	compatible = "usb1234,5678";
	reg = <1>;
	#address-cells = <2>;
	#size-cells = <0>;

	interface@2 {
		compatible = "usbif1234,5678.config1.2";
		reg = <2 1>;
		#address-cells = <1>;
		#size-cells = <0>;

		i2c@1 {
			reg = <1>;
			clock-frequency = <100000>;
			#address-cells = <1>;
			#size-cells = <0>;

			eeprom@50 {
				compatible = "atmel,24c02";
				reg = <0x50>;
			};
		};

		serial@2 {
			reg = <2>;
		};

		i2c@3 {
			reg = <3>;
			status = "disabled";
		};
	};
};
```

The interface node follows the kernel's USB device binding
(`Documentation/devicetree/bindings/usb/usb-device.yaml`). For a
device with one configuration and one interface, and a device class
of 0 or 9, the USB core uses the device node for the interface too;
the channel nodes then go directly below `device@1`.

### ACPI example

A USB interface has no ACPI companion of its own: the USB core gives
it the companion of its USB device, which is the object of the port
the device is plugged into. So the driver looks for an interface
object below the port object: the child whose `_ADR` is the
interface's `bInterfaceNumber`. The channel objects are children of
the interface object, with `_ADR` = the channel number. A device with
a single interface still needs the interface object, with `_ADR` 0.
The same device on root hub port 1, with the same three channels:

```asl
Scope (\_SB.PCI0.XHC.RHUB.HS01)
{
  Device (IF02)
  {
    Name (_ADR, 2)

    Device (CH01)
    {
        Name (_ADR, 1)
        Name (_DSD, Package () {
            ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
            Package () {
                Package () { "clock-frequency", 100000 },
            }
        })

        Device (EE50)
        {
            Name (_HID, "PRP0001")
            Name (_DSD, Package () {
                ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
                Package () {
                    Package () { "compatible", "atmel,24c02" },
                }
            })
            Name (_CRS, ResourceTemplate () {
                I2cSerialBusV2 (0x50, ControllerInitiated, 100000,
                    AddressingMode7Bit, "\\_SB.PCI0.XHC.RHUB.HS01.IF02.CH01",
                    0x00, ResourceConsumer, , Exclusive, )
            })
        }
    }

    Device (CH02)
    {
        Name (_ADR, 2)
    }

    Device (CH03)
    {
        Name (_ADR, 3)
        Method (_STA, 0) { Return (0) }
    }
  }
}
```

The path of the port object depends on the platform's ACPI tables.
Interface and channel objects are matched on `_ADR` alone; their names
are free. On a USB hub that carries OBMF-ICP on one of its own
interfaces, the hub's port objects are also children of the hub's
object, with `_ADR` 1, 2, and so on. An interface object with `_ADR`
0 does not collide with them, but one with the number of a port does
(`KNOWN-ISSUES.md`).

### Software nodes

A platform driver or a board file can attach software nodes to the
interface. The rules are those of device tree: a child named `i2c`,
`serial` or `gpio` with a `reg` property equal to the channel number,
and the I2C clients below an `i2c` child.

## Interfaces to the kernel and user space

On the USB interface:

- `obmf/name`, a sysfs attribute group with the device name from the
  device header;
- `usb/obmf/<interface>/` in debugfs, with its `events/` directory
  (`ARCHITECTURE.md`, "Counters").

On the auxiliary bus, one device per function channel, named
`obmf.i2c.<id>`, `obmf.serial.<id>`, `obmf.gpio.<id>` or
`obmf.spi.<id>` in
`/sys/bus/auxiliary/devices/`, each with:

- `name`: the channel name (CHANNEL_NAME, Table 65);
- `channel`: the channel number.

I2C: an I2C adapter, a child of the auxiliary device, named
`<device name>_<channel name>`. It supports plain I2C transfers and
the SMBus commands emulated on them except Quick (no Host Notify and
no block read yet). Its quirks: no zero-length messages, and read and
write lengths up to the channel's payload limits less four bytes.
Only 7-bit addresses. The adapter's timeout covers its retries.

SPI: an SPI controller, a child of the auxiliary device, with a
dynamic bus number (`/sys/class/spi_master/spi<B>`), its clients as
`spi<B>.<cs>`. It takes the SPI core's messages with the limits and
refusals of `ARCHITECTURE.md`. With `auto_bind_spidev=1` (out of tree
only, for development and debugging only) and no firmware clients,
each chip select is `/dev/spidev<B>.<cs>`.

Serial: a tty `/dev/ttyOBMF<n>`, with up to 256 ports in the system,
below its auxiliary device, in
`/sys/bus/auxiliary/devices/obmf.serial.<id>/tty/`. Its line settings
are fixed by the device (`KNOWN-ISSUES.md`); termios reports them,
with `CRTSCTS` for RTS/CTS flow control. `TIOCGICOUNT` reports
received and sent characters, breaks, overruns, Carrier Down events
(`dcd`) and device writes refused for a full buffer (`buf_overrun`). Break is supported; modem
control lines are not. The default termios has `CLOCAL` set, so
Carrier Down hangs the tty up only after `stty -clocal`. After a
transport failure the tty is hung up and an open fails with `EIO`
until the interface is bound again. In debugfs,
`obmf_tty/<tty>/tx_dropped` and `tx_stalls` count the characters the
port dropped and its stall lines.

GPIO: a GPIO chip, a child of the auxiliary device, labelled
`<device name>_<channel name>`, with one line per GPIO_INDEX up to the
highest the configuration lists; the indices it does not list are
holes. Line names are GPIO_NAME. Each line keeps the direction the
device gives; a request for the other one fails with `EINVAL`. Reading
an output gives the value last set. Inputs give edge interrupts
(rising, falling, both); level types fail with `EINVAL`. No drive or
bias can be set (`KNOWN-ISSUES.md`). The interface's `events/` counts
failed Sets (`gpio_set_failed`), refused interrupt configurations
(`gpio_irq_config_failed`) and interrupt events for lines with no
interrupt set up (`gpio_irq_unused`).
