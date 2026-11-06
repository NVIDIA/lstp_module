# Requirements

What the driver needs from the kernel, from the device, and from the
platform description, and what it offers to user space.

## Kernel versions

The code is written against the current mainline kernel interfaces.
It was built against Linux 6.12 and 6.18 (arm64) and Linux v7.3-rc6
(x86_64); Linux 6.12 is the oldest kernel it was built for. Older
kernels are not supported yet.

`compat/obmf-compat.h` lets the same sources build against kernels
older than v7.3. The out-of-tree `Kbuild` includes it in every file.
It defines the newer helpers the code uses (allocation helpers, the
scoped firmware node walk, the symbol namespace macros of 6.12, the
default workqueue name, a tty driver flag) when the kernel lacks
them, and is empty on a kernel that has them all. `Kbuild` also
checks the kernel headers for two of them. In a kernel tree the
header is not used.

## Kernel configuration

The kernel needs `CONFIG_USB` and `CONFIG_AUXILIARY_BUS=y`. The I2C
driver needs `CONFIG_I2C`, and the serial driver the tty layer.
`CONFIG_DEBUG_FS` gives the counters, `CONFIG_DYNAMIC_DEBUG` the debug
messages at run time, and `CONFIG_FAULT_INJECTION_DEBUG_FS` the
transfer fault injection.

In a kernel tree, `drivers/usb/misc/obmf/Kconfig` has:

| option | type | what |
|---|---|---|
| `CONFIG_USB_OBMF` | tristate | `obmf.ko`; depends on `USB`, selects `AUXILIARY_BUS` |
| `CONFIG_OBMF_DIALECTS` | bool | the dialect support |
| `CONFIG_OBMF_LSTP` | bool | LSTP devices; depends on `OBMF_DIALECTS` |
| `CONFIG_OBMF_KUNIT_TEST` | bool | the KUnit suites; depends on `KUNIT` |
| `CONFIG_OBMF_LSTP_KUNIT_TEST` | bool | the LSTP suite; depends on `OBMF_LSTP` |

Out of tree, the `make` switches set the same options:
`OBMF_DIALECTS=y`, `OBMF_LSTP=y` (which also sets the dialect
support), `OBMF_KUNIT_TEST=y` (which needs a kernel with
`CONFIG_KUNIT`, and with `OBMF_LSTP=y` adds the LSTP suite), and
`OBMF_DEBUG=y`, which builds every file with `DEBUG`.

The I2C and serial drivers have no Kconfig entries yet.

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
  one request per channel at a time.

Channel 0 (OBMF-ICP 1.0.0, 5.15):

- answers MMIO short reads (5.3) of up to 64 bytes anywhere in the
  device header, the channel header offsets, the channel headers and
  their configuration data;
- has a device header with major version 1;
- has, for each channel, a header whose CHANNEL_NUMBER is the
  channel's index, at an offset after the offset table, not
  overlapping another channel, with at most 32 KiB of configuration
  data; all channels together within 256 KiB;
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

Per channel:

- payload limits large enough for the type's head: at least 3 bytes
  for serial, 4 for I2C;
- I2C controller: the configuration data of Table 79, with the bus
  speed index; bus recovery, if offered, through CHANNEL-SPECIFIC
  CONTROL and STATUS (Table 69);
- serial: the configuration data of Table 78, with a nonzero speed,
  5 to 8 data bits, 1 or 2 stop bits and a parity the table defines;
  a request limit that holds at least one character.

## Device requirements: LSTP

The interface has no functional descriptor. Each message is one bulk
packet, at most 512 bytes, and each endpoint's packet size is at
least 64 bytes; no zero-length packets are sent. Channel 0 is the
management channel of LSTP version 1, read with READ_CONFIG. A
channel's configuration is read up to offset 0x7fff and cut there
with a warning, and all channels together are read within 256 KiB.
Every response wait is at least 1000 ms. I2C and UART channels get
function devices; the UART configuration starts with the bytes of
OBMF-ICP Table 78 and follows the serial rules above.

## Describing function devices: device tree, ACPI, software nodes

A firmware node is optional. Without one, an I2C channel still gets
its adapter, and clients can be added from user space. A node gives
the I2C clients on the channel's bus, a `clock-frequency` to check,
or a way to leave a channel unused. Only I2C channels are looked up;
serial channels take no node yet.

The node of a channel is a child of the USB interface's node. On
device tree and with software nodes, it is the child named `i2c` (the
name before any `@`) whose `reg` is the channel number. A child with
that `reg` and another name is ignored with the warning
"channel <n>: node <name> ignored", where `<name>` is the node's name
with its unit address, for example `spi@1`. On ACPI, it is the child
of the interface object whose `_ADR` is the channel number (see the
ACPI example).

A node that is disabled (`status = "disabled"`), or an ACPI object
whose `_STA` says it is not present, means the channel gets no
function device.

If the node has `clock-frequency`, it must equal the bus speed in the
channel's configuration; otherwise the I2C probe fails with
"clock-frequency <n>, channel runs at <m>". The driver cannot set the
speed.

The I2C clients are the child nodes of the channel's node, or of its
`i2c-bus` child if it has one, as for any I2C adapter on device tree.
On ACPI they are the devices whose `I2cSerialBus` resource names the
channel's object. The speed given in that resource is not checked.

The examples below are derived from the code and have not yet been
tested on a board. `1234` and `5678` stand for the device's vendor
and product IDs.

### Device tree example

The USB device on port 1 of a hub or root hub, configuration 1,
OBMF-ICP on interface 2; channel 1 is an I2C controller with an
EEPROM at 0x50, and channel 3 is an I2C controller left unused:

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
the `i2c@N` nodes then go directly below `device@1`.

### ACPI example

A USB interface has no ACPI companion of its own: the USB core gives
it the companion of its USB device, which is the object of the port
the device is plugged into. So the driver looks for an interface
object below the port object: the child whose `_ADR` is the
interface's `bInterfaceNumber`. The channel objects are children of
the interface object, with `_ADR` = the channel number. A device with
a single interface still needs the interface object, with `_ADR` 0.
The same device on root hub port 1, with the same two channels:

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
interface. The rules are those of device tree: a child named `i2c`
with a `reg` property equal to the channel number, and the I2C
clients below it.

## Interfaces to the kernel and user space

On the USB interface:

- `obmf/name`, a sysfs attribute group with the device name from the
  device header;
- `usb/obmf/<interface>/` in debugfs (`ARCHITECTURE.md`, "Counters").

On the auxiliary bus, one device per function channel, named
`obmf.i2c.<id>` or `obmf.serial.<id>` in
`/sys/bus/auxiliary/devices/`, each with:

- `name`: the channel name (CHANNEL_NAME, Table 65);
- `channel`: the channel number.

I2C: an I2C adapter, a child of the auxiliary device, named
`<device name>_<channel name>`. It supports plain I2C transfers and
the SMBus commands emulated on them except Quick (no Host Notify and
no block read yet). Its quirks: no zero-length messages, and read and
write lengths up to the channel's payload limits less four bytes.
Only 7-bit addresses. The adapter's timeout covers its retries.

Serial: a tty `/dev/ttyOBMF<n>`, with up to 256 ports in the system.
Its line settings are fixed by the device (`KNOWN-ISSUES.md`);
`TIOCGICOUNT` reports received and sent characters, breaks and
overruns. Break is supported; modem control lines are not.
