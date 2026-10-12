# Low Speed Transport Protocol (LSTP) Driver for Linux

Out-of-tree Linux USB driver for [Low-Speed Transport Protocol (LSTP)](devicetree/bindings/usb/nvidia,lstp.yaml) device. See [LSTP Specification 1.0](LSTP%20Specification%201.0%20--%2008-03-2026.pdf) for more details.

The device firmware must implement the LSTP Specification 1.0 in this
repository, including SMBUS_BLOCK_READ, READ_CONFIG with an explicit length at
any byte offset, and the SPI channel configuration. With earlier firmware,
SMBus block reads fail with `-EOPNOTSUPP`, and channels whose configuration
cannot be read fail to initialise; update the device firmware.

## Contributing

This project is currently not accepting contributions.

## Cheatsheet

Requires Linux 6.9 or later. 6.8.12 and later 6.8 stable releases also build,
as do Ubuntu 24.04's 6.8 kernels from 6.8.0-40 on, which carry the
`DEFINE_RAW_FLEX()` backport.
On Red Hat Enterprise Linux, 9.6 and later 9.x releases build (their 5.14
kernels carry the required backports), as do RHEL 10 and rebuilds such as
AlmaLinux and Rocky Linux. Earlier RHEL 9 releases do not.

Dependencies:

```bash
sudo apt install -y build-essential linux-headers-$(uname -r)
# RHEL and its rebuilds:
sudo dnf install -y gcc make elfutils-libelf-devel kernel-devel-$(uname -r)
```

Build and load:

```bash
make build

# Signing optional (see creating signing keys below):
for ko in ./build/*.ko; do sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 ~/LSTP_KEYS/MOK.priv ~/LSTP_KEYS/MOK.der "$ko"; done

make load
```

`make load` is for development: it loads the modules straight from `build/`
with `insmod`, the core first, with any `name=value` arguments as its
parameters, then every subsystem driver; see [Modules](#modules). It installs
nothing, so nothing loads on demand: a subsystem module that is removed stays
out until it is loaded again, and the modules are gone after a reboot. `make
reload` unloads, builds and loads. `KO_DIR=<dir>` loads the modules from
another directory. If this tree's modules are also installed, run `make
uninstall` first, or udev may load an installed subsystem module before
`make load` does.

Unload:

```bash
make unload      # the subsystem drivers, then the core
```

Install, so the modules load at boot and on hotplug, each subsystem module
when a channel needs it:

```bash
make install     # modules to /lib/modules/$(uname -r)/updates/, then depmod
make uninstall
```

`updates/` is searched before the kernel's own modules, so the installed copy
is used even on a kernel that builds its own LSTP driver as a module. Set
`INSTALL_MOD_DIR=<dir>` to install elsewhere. Earlier releases installed to
`extra/`; `make install` removes a copy left there. Before installing an
earlier release, run `make uninstall` from this one: earlier releases install
only `lstp.ko` and leave the subsystem modules in place.

### Build options

```bash
make build IPMI_POSTCODES=1   # IPMI POST codes get their own device node
make build IPMI_MSG_NUM=0     # IPMI nodes drop the message number
make build DEBUG=1            # -DDEBUG, so dev_dbg() output is compiled in
```

`IPMI_MSG_NUM` defaults to on, and only `IPMI_MSG_NUM=0` turns it off; the
others turn on when set to any value. An out-of-tree build never reads this
driver's `Kconfig`, so the driver's own `CONFIG_` options are reachable only
through switches like these. The kernel's own options are as the kernel being
built against has them.

By default an IPMI message on `/dev/ipmi-lstp<n>` is a 32-bit length, a
one-byte message number that the response echoes back, and the payload. The
message number is specific to this driver: it is not part of the LSTP
protocol, and no other BMC-side IPMI driver has it. `IPMI_MSG_NUM=0` drops it,
so the node uses the `struct ipmi_ssif_msg` layout of the kernel's `ssif_bmc`
driver, and userspace written for that driver works unchanged. The default
stays on for userspace that parses the message number.

### Signing

Required on any kernel that enforces signed modules: x86 distros with UEFI
Secure Boot (e.g. Ubuntu), BMC / OpenBMC builds, and other locked-down
kernels. Check with `mokutil --sb-state` or
`cat /sys/module/module/parameters/sig_enforce`.

One-time: create a key and enroll it (x86 Secure Boot via MOK shown here;
BMCs typically sign with a key already in the kernel's trusted keyring):

```bash
mkdir ~/LSTP_KEYS/
openssl req -new -x509 -newkey rsa:2048 -keyout ~/LSTP_KEYS/MOK.priv -outform DER -out ~/LSTP_KEYS/MOK.der -nodes -days 36500 -subj "/CN=NVLSTP/"
sudo mokutil --import ~/LSTP_KEYS/MOK.der
sudo reboot now
# In MOK manager: Enroll MOK > Continue > password > Finish and reboot
```

Every build: sign every module before `make load`:

```bash
for ko in ./build/*.ko; do sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 ~/LSTP_KEYS/MOK.priv ~/LSTP_KEYS/MOK.der "$ko"; done
```

### Cross-build

Required dependencies:

```bash
sudo apt install -y build-essential bc bison flex libssl-dev libncurses5-dev git
```

You will also need your target cross toolchain. Using aarch64 for example:

```bash
sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

To build for a non-host kernel, point `KDIR` at a `modules_prepare`'d kernel
source tree and pass `ARCH`/`CROSS_COMPILE`.
For Raspberry Pi: clone your target branch of
[raspberrypi/linux](https://github.com/raspberrypi/linux) into `~/linux`,
run the board's `*_defconfig` and `modules_prepare` against it, then
`make build_rpi` (cross-compiles against `~/linux`).

## Modules

The driver is a core module, `lstp`, and one module per subsystem driver.
The core binds the USB interface and adds an auxiliary device,
`lstp.<type>.<instance>`, for each channel; each subsystem module binds the
auxiliary devices of its type:

| Module | Binds | Driver directory under `/sys/bus/auxiliary/drivers/` |
| --- | --- | --- |
| `lstp` | the USB interface (`/sys/bus/usb/drivers/lstp`) | -- |
| `gpio-lstp` | `lstp.gpio.*` | `gpio_lstp.gpio` |
| `leds-lstp` | `lstp.led.*`, a GPIO channel's LED pins | `leds_lstp.led` |
| `i2c-lstp` | `lstp.i2c.*` | `i2c_lstp.i2c` |
| `spi-lstp` | `lstp.spi.*` | `spi_lstp.spi` |
| `lstp_tty` | `lstp.uart.*` | `lstp_tty.uart` |
| `ipmi-lstp` | `lstp.ipmi.*` | `ipmi_lstp.ipmi` |
| `lstp-mmio` | `lstp.mmio.*` | `lstp_mmio.mmio` |

`lsmod` and `/sys/module/` show the names with underscores (`gpio_lstp`);
`modprobe` and `rmmod` take either form.

Each subsystem module lists the devices it binds as module aliases
(`modinfo -F alias build/gpio-lstp.ko` prints `auxiliary:lstp.gpio`), so once the
modules are installed (`make install`), udev loads the one a channel needs
when the core adds the channel. `make load` loads all of them instead, from
the build directory. A host without udev has to load them itself, or have its hotplug
helper run `modprobe` on each uevent's `MODALIAS`. Only the modules for the
channel types in use need to be loaded: a channel whose module is absent gets
its auxiliary device and stays unbound until the module loads. LED pins need
`gpio-lstp` as well as `leds-lstp`: their `lstp.led.*` devices are added by
`gpio-lstp` when it binds the GPIO channel, not by the core.

Every subsystem module depends on `lstp`, which `modprobe` loads first;
`insmod` does not, so load `lstp` before any subsystem module. The
core cannot be removed while any subsystem module is loaded, so `rmmod lstp`
and `modprobe -r lstp` fail with "in use" until they are gone; `make unload`
removes them first. Removing a subsystem module unbinds its channels. It
stays in use, and cannot be removed, while something holds one of its
devices: an open tty, IPMI or MMIO node, an open `/dev/i2c-<n>`, or a
requested GPIO line.

### Features of this package only

The copy of the driver prepared for the mainline kernel leaves out what this
package keeps for deployed configurations:

- the `auto_bind_spidev` parameter and `CONFIG_USB_LSTP_SPI_SPIDEV`, its
  in-tree default (see [auto_bind_spidev](#auto_bind_spidev));
- the IPMI POST-code node: built with `IPMI_POSTCODES=1`, each IPMI channel's
  misc/ipmi-lstp<n> class device has a misc/ipmi-lstp<n>-postcodes sibling,
  the node `/dev/ipmi-lstp<n>-postcodes`;
- `lstp-mmio`, whose misc/lstp_mmio_<n> class device is the node
  `/dev/lstp_mmio_<n>`;
- matching every LSTP interface of the known USB vendors, not only the listed
  products.

The sysfs and device-node rules in
`Documentation/ABI/testing/sysfs-bus-usb-devices-lstp` apply to these nodes
too.

### Copying the driver into a kernel tree

`scripts/export-upstream.sh <kernel-tree>` writes that copy: the drivers,
`<linux/usb/lstp.h>`, the documentation and the bindings, each at its path in
the kernel tree. In a tree that has a `MAINTAINERS` file it also applies
`scripts/upstream-hookup.patch`, which adds one Kconfig option per driver
(`USB_LSTP`, `GPIO_LSTP`, `LEDS_LSTP`, `I2C_LSTP`, `SPI_LSTP`, `LSTP_TTY` and
`LSTP_IPMI_BMC`), their Makefile lines, `USB_LSTP_IPMI_MSG_NUM`, and the
`MAINTAINERS` and documentation index entries. `LEDS_LSTP` depends on
`GPIO_LSTP`, whose driver adds the LED devices. The patch is made against the
kernel release the copy targets; the script fails if it does not apply.
`unifdef` 2.12 must be installed.

The I2C driver reads the retry count from `nvidia,retries`. This package's
build also reads `retries`, the property's older name, when `nvidia,retries`
is absent, so existing device trees and ACPI tables keep working; the copy
reads only `nvidia,retries`.

A BMC kernel that needs the `auto_bind_spidev` parameter, the IPMI POST-code
node or the older `retries` name takes `scripts/export-upstream.sh --bmc
<kernel-tree>` instead. That copy keeps those features and the POST-code
header, and adds the Kconfig options of the first two with
`scripts/upstream-hookup-bmc.patch`. Every option defaults to
what `make build` does here, so a kernel configured with the defaults gets
the same driver:

| Kconfig option | Default | Here |
| --- | --- | --- |
| `USB_LSTP_IPMI_MSG_NUM` | y | on unless `IPMI_MSG_NUM=0` |
| `USB_LSTP_SPI_SPIDEV` (`--bmc` only) | n | `auto_bind_spidev` defaults to 0 |
| `SEPARATE_LSTP_IPMI_POSTCODES` (`--bmc` only) | n | on with `IPMI_POSTCODES=1` |

Neither copy has `lstp-mmio` or the vendor-wide USB match.

## Module Parameters

### auto_bind_spidev

By default, the driver registers SPI controllers but creates no SPI devices
on a channel that no firmware node describes. `auto_bind_spidev=1` creates one
SPI device with the `spidev` modalias on each chip select of such a channel:

```bash
make load auto_bind_spidev=1
```

The parameter belongs to the core module, `lstp`, although only `spi-lstp`
reads it: `options lstp auto_bind_spidev=1` in a `modprobe.d` file and
`lstp.auto_bind_spidev=1` on the kernel command line set it.

spidev binds to these devices by itself only on kernels whose spidev ID table
lists `spidev`, which some distribution kernels (Raspberry Pi OS, for one)
add. Mainline spidev does not, so elsewhere bind each device explicitly:

```bash
echo spidev | sudo tee /sys/bus/spi/devices/spi<bus>.<cs>/driver_override
echo spi<bus>.<cs> | sudo tee /sys/bus/spi/drivers_probe
```

The parameter is deprecated where SPI host controllers have a `new_device`
attribute (Linux 7.3 and later, when built with `CONFIG_SPI_DYNAMIC`), and the
driver warns when it is set there.
Leave the parameter unset and create each device from userspace instead;
`new_device` sets `driver_override` itself, so spidev binds on any kernel:

```bash
echo "spidev <cs>" | sudo tee /sys/class/spi_master/spi<bus>/new_device
echo <cs> | sudo tee /sys/class/spi_master/spi<bus>/delete_device
```

Where the controller has no `new_device`, the parameter is the only way to get
SPI devices on an undescribed channel and keeps working without a warning.

A channel that any firmware node describes -- device tree, ACPI or software
node -- is left alone by this parameter. Device tree and ACPI enumerate the
SPI children themselves; a software node enumerates none, and the driver logs
that rather than substituting a spidev for a bus firmware has already spoken
for. For raw access to a described peripheral, write `spidev` to its
`driver_override` attribute, as above.

In a kernel tree the driver was copied into with `export-upstream.sh --bmc`
(see [Copying the driver into a kernel tree](#copying-the-driver-into-a-kernel-tree)),
`CONFIG_USB_LSTP_SPI_SPIDEV` sets the compile-time default.

## Using and Inspecting LSTP

### Driver Logs

Driver messages go to the kernel log. View with `dmesg`:

```bash
sudo dmesg | grep -i lstp
```

### Finding LSTP Devices

Bound USB interfaces appear as symlinks under the driver's sysfs directory:

```console
$ find /sys/bus/usb/drivers/lstp -maxdepth 1 -type l
/sys/bus/usb/drivers/lstp/3-3.1:1.2
/sys/bus/usb/drivers/lstp/3-4.4:1.2
...
```

Every supported firmware-reported channel is also registered on the auxiliary
bus, so the channels of all bound interfaces can be listed in one place:

```console
$ ls /sys/bus/auxiliary/devices/ | grep '^lstp\.'
lstp.gpio.12
lstp.i2c.14
lstp.ipmi.16
lstp.mmio.15
lstp.spi.11
lstp.uart.13
```

A GPIO channel with LED pins also lists its `lstp.led.<n>`, which is not a
channel.

### Sysfs Topology

> **Static vs. runtime identification.** There are two supported ways to pin a
> channel to a stable handle. On platforms with a firmware description, describe
> the device statically in a device tree or ACPI (see "Device Tree Support"
> below) for the devices behind its channels (SPI and I2C peripherals, GPIO line
> names, LEDs); in a device tree, an `i2c` or `spi` alias also fixes that bus
> number. Where no firmware description is present, identify handles at runtime
> through the sysfs interfaces below -- by channel `name`, or by USB port path +
> `channel`. The two approaches are complementary and can be combined.

Each channel is an `auxiliary_device` named `lstp.<type>.<instance>`, where
`<type>` is the channel's subsystem -- `spi`, `i2c`, `gpio`, `ipmi`, `uart` or
`mmio` -- and `<instance>` is a kernel-assigned auxiliary-bus instance number
that is unique across every LSTP interface in the system. `<instance>` is
**not** the firmware channel id and need not equal it: it is handed out in
channel-discovery order from a module-global pool shared by every LSTP device,
so it depends on how many channels were created before this one. It is fixed
for as long as the channel exists, including across an unbind and rebind of
the channel's driver, and is reassigned when the USB interface is probed
again. The firmware-reported channel id is exposed by the read-only `channel`
attribute, which is the only supported way to obtain it. A firmware channel
whose type this driver does not support gets no auxiliary device, so newer
firmware can advertise types an older module simply ignores.

Each channel auxiliary device carries `name`, `enable` and `channel`
attributes, a `statistics/` subdirectory of per-channel counters, and the
underlying Linux device (SPI controller, I2C adapter, GPIO chip, etc.) nested
beneath it, either directly or inside its class directory. That nested device
directory is the way to reach the underlying node.

The nested device directory is present only while a driver is bound to the
channel, because the bound driver is what creates the backing device. An
unbound channel still lists `name`, `enable`, `channel` and `statistics/`;
treat the missing device as "not currently bound" rather than as a channel
that does not exist. It may be waiting for its driver, its driver may have
failed to probe, or it may have been unbound by hand.

The same auxiliary devices are reachable both through the auxiliary bus
(`/sys/bus/auxiliary/devices/lstp.<type>.<instance>`) and beneath their parent
USB interface:

> **Don't hardcode bus numbers, `spiX.Y`, `i2c-N`, or `gpiochipN`.** USB is
> dynamic: these IDs can change across boots, topologies, and hotplug order.
> The channel `name` is the most portable stable identifier; the USB port path
> (the interface directory name) is also stable as long as the device stays in
> the same physical port. Always discover the underlying nodes via sysfs at
> runtime (a device-tree `i2c` or `spi` alias fixes those bus numbers).

```console
$ tree -L 3 /sys/bus/usb/devices/1-1.3:1.2/
/sys/bus/usb/devices/1-1.3:1.2/
├── lstp.spi.11/                 # SPI channel (auxiliary_device)
│   ├── name                     #   firmware channel name, e.g. "MY_BOARD_SPI"
│   ├── enable                   #   "1" if the channel is enabled
│   ├── channel                  #   firmware channel id (may differ from ".11")
│   ├── statistics/              #   per-channel LSTP counters
│   └── spi_master/
│       └── spi7/                #   the Linux SPI controller
├── lstp.i2c.14/
│   ├── name
│   ├── enable
│   ├── channel
│   ├── statistics/
│   └── i2c-67/                  #   the Linux I2C adapter
├── lstp.gpio.12/
│   ├── name
│   ├── enable
│   ├── channel
│   ├── statistics/
│   ├── gpiochip9/               #   the Linux GPIO chip
│   └── lstp.led.12/             #   the channel's LED pins, if it has any
│       └── leds/                #   one LED class device per pin
├── lstp.ipmi.16/
│   ├── name
│   ├── enable
│   ├── channel
│   ├── statistics/
│   └── misc/
│       └── ipmi-lstp0/          #   the IPMI character device
├── lstp.uart.13/
│   ├── name
│   ├── enable
│   ├── channel
│   ├── statistics/
│   └── tty/
│       └── ttyLSTP0/            #   the serial port
└── lstp.mmio.15/
    ├── name
    ├── enable
    ├── channel
    ├── statistics/
    └── misc/
        └── lstp_mmio_0/         #   the MMIO character device
```

Channel types backed by a character device expose it as well:
`/dev/ttyLSTP<line>` for UART, `/dev/ipmi-lstp<n>` for IPMI -- plus an
`-postcodes` sibling when built with `IPMI_POSTCODES=1` -- and
`/dev/lstp_mmio_<n>` for MMIO. None of those suffixes is the firmware channel
id, and none is the auxiliary-bus instance either: each comes from its own
subsystem's numbering, lowest free number first each time the channel's
driver binds, so a rebind while a file descriptor still holds the old node
takes another number; "Stable IPMI node names" below gives
IPMI nodes names that survive replug and reboot. A GPIO channel whose
firmware reports LED pins hands them to the LED driver through an auxiliary
device of its own, `lstp.led.<instance>` with the channel's instance number,
and the LED class devices nest under that as `leds/<name>`. It is not a
channel and has none of the channel attributes.

A UART's baud rate, framing, parity and flow control are set by the device
firmware and keep those values whatever `stty` sets. A firmware node for a
UART channel is attached to the channel's auxiliary device, not to the tty,
so the tty's uevent carries no firmware properties; a udev rule has to match
the parent, with `KERNELS==` or `ATTRS{}`.

Discovery flow:

1. Iterate the channels in `/sys/bus/auxiliary/devices/lstp.*` (or, scoped to a
   single interface, `<interface>/lstp.*.*`). On the auxiliary bus this also
   lists the `lstp.led.*` devices, which are not channels; skip any entry
   without a `channel` file.
2. Match on `<channel>/name` (the stable identifier). Read `<channel>/channel`
   for the firmware channel id; do not parse it out of the auxiliary device's
   instance-numbered name.
3. Descend into the channel directory to find the underlying device node nested
   beneath it.

Example: find every SPI channel named `MY_BOARD_SPI` and print its underlying
SPI controller:

```bash
for aux in /sys/bus/auxiliary/devices/lstp.spi.*; do
    [ "$(cat "$aux/name")" = "MY_BOARD_SPI" ] || continue
    spi=$(ls -d "$aux"/spi_master/spi* 2>/dev/null | head -1)
    echo "channel=$(basename "$aux") spi=$(basename "$spi")"
done
```

### Statistics

Each interface and each channel has a `statistics/` directory of read-only
counters. They start at zero when the interface is probed and only ever
increase, so sample twice and subtract to get a rate. There is no reset, and
the driver never acts on a value it reports.

`lstp/statistics/` on the interface covers the USB bulk endpoints that all
channels share: `rx_packets`, `rx_errors`, `rx_no_channel`, `rx_urb_errors`,
`rx_stalls` and `rx_retries`. `lstp.<type>.<instance>/statistics/` on each
channel auxiliary device covers that channel's LSTP exchanges:
`tx_packets`, `rx_packets`, `rx_events`, `timeouts`, `errors`,
`tx_errors`, `rx_dropped` and `rx_late`. Every file is defined in
[Documentation/ABI/testing/sysfs-bus-usb-devices-lstp](Documentation/ABI/testing/sysfs-bus-usb-devices-lstp).

Reading them:

- `timeouts` against `errors` separates a silent device from a failing one.
  A device that answers and reports failures increments `errors`; one that
  has stopped answering increments `timeouts`.
- `rx_late` climbing alongside `timeouts` means responses are still arriving,
  just later than the driver waits -- a slow device rather than a dead one.
  Worth reducing the request rate on that channel: a reply that arrives after
  its requester gave up is only recognised as late if nothing else is waiting
  by then, so on a busy channel some of them are instead handed to whichever
  request is outstanding at the time, and `rx_late` undercounts.
- `rx_stalls` or `rx_retries` growing points at the USB link rather than at
  any one channel.
- `rx_dropped` against `rx_events` shows what share of the traffic the device
  originates is being lost. On a channel like UART receive, `rx_events` is
  where inbound data is counted, not `rx_packets`.

The interface counters include the control channel used to enumerate and
configure channels, which has no `statistics/` directory, so they read higher
than the per-channel counters add up to. Note that reading a channel's
`enable` queries the device, so polling it advances those interface counters.

Channels backed by a Linux subsystem that keeps its own counters have those
as well, so a SPI channel also has `statistics/` under its `spi_master`
device and a UART channel reports line errors through `TIOCGICOUNT`.

### Stable identification by USB port path + channel number

If the device stays in the same physical USB port, the interface's port path
(its directory name under `/sys/bus/usb/devices/`, e.g. `1-1.3:1.2`) is stable
across reboots and hotplug -- unlike the bus numbers baked into node names
(`spiX.Y`, `i2c-N`, `gpiochipN`). Combined with the firmware channel number (the
read-only `channel` attribute), it pins a specific handle without depending on
the channel `name`:

```bash
# Resolve the SPI node on interface 1-1.3:1.2, firmware channel 1:
intf=/sys/bus/usb/devices/1-1.3:1.2
for aux in "$intf"/lstp.*.*; do
    [ "$(cat "$aux/channel")" = "1" ] || continue
    ls -d "$aux"/spi_master/spi*   # underlying node, nested under the auxdev
done
```

Use the matching nested lookup for other channel types: `"$aux"/i2c-*`,
`"$aux"/gpiochip*`, `"$aux"/tty/*`, or `"$aux"/misc/*`. This is the runtime
equivalent of the (USB port path, channel number) key used by the device-tree
description in "Device Tree Support" below.

### Stable IPMI node names

IPMI nodes are named `ipmi-lstp<n>` in probe order, so which channel gets
which number can change between boots. Name a channel with a udev rule keyed
on the USB port path and the firmware channel number, the same key as above.
`udev/60-lstp-ipmi.rules` is an example: it links channel 4 of the adapter
in port path `platform-xhci-hcd.0-usb-0:1.3:1.2` to `/dev/ipmi-stable-symlink`.

```text
ENV{ID_PATH}=="platform-xhci-hcd.0-usb-0:1.3:1.2", ATTRS{channel}=="4", \
  SYMLINK+="ipmi-stable-symlink", TAG+="systemd"
```

Find the port path and channel of a node, and put yours and the name you
want in the rule:

```bash
udevadm test-builtin path_id /sys/class/misc/ipmi-lstp0 | grep ^ID_PATH=
cat /sys/class/misc/ipmi-lstp0/device/channel
```

Then install it and apply it to the nodes already present:

```bash
sudo install -m 0644 udev/60-lstp-ipmi.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=misc
ls -l /dev/ipmi-stable-symlink
```

The symlink's target is the node, so `/sys/class/misc/$(basename
"$(readlink -f /dev/ipmi-stable-symlink)")/device` is the channel directory.

### From a device node to its channel

Every node this driver creates sits below its channel's auxiliary device, and
that directory is the first one above the node with a `channel` file. How far
above depends on the type: for `/dev/ipmi-lstp<n>`, `/dev/ttyLSTP<n>` and
`/dev/lstp_mmio_<n>` it is the class device's `device` link, while
`/dev/i2c-<n>`, `/dev/gpiochip<n>` and `/dev/spidev<b>.<c>` sit one or more
levels further down. Walking up from the node's sysfs path works for all of
them, and for any symlink udev made to one:

```bash
lstp_channel_of() {
    local d
    d=$(readlink -f "/sys$(udevadm info -q path -n "$1")") || return
    while [ "$d" != /sys ] && [ ! -e "$d/channel" ]; do d=${d%/*}; done
    [ "$d" != /sys ] && echo "$d"
}

c=$(lstp_channel_of /dev/ipmi-stable-symlink)
cat "$c/channel" "$c/name" "$c/enable"
ls "$c/statistics/"
```

It prints nothing for a node that is not an LSTP channel's, so it also tells
LSTP nodes apart from other `gpiochip`, `i2c-` or `spidev` nodes. The parent
of the channel directory is the USB interface. LEDs have no device node, and
`/sys/class/leds/<name>/device` is their `lstp.led.<instance>` device, so
their GPIO channel is `/sys/class/leds/<name>/device/..`. To see the
same chain by eye, `udevadm info -a -n <node>` lists the channel as
`KERNELS=="lstp.<type>.<instance>"` with its `ATTRS{channel}` and
`ATTRS{name}`.

### Migrating from earlier driver releases

Drivers that create `lstp.<type>.<instance>` directories under the USB
interface use the layout below; earlier drivers placed channels under
`<interface>/lstp/channel/<id>/`. The interface's own `lstp/name` is unchanged.

| Earlier path | Use instead |
| --- | --- |
| `<interface>/lstp/channel/<id>/{name,enable}` | the same files in the `<interface>/lstp.*.*` directory whose `channel` reads `<id>` |
| `<interface>/lstp/channel/<id>/device` | the device nested in that channel directory |
| `<interface>/{i2c-<n>,gpiochip<n>}` | `<interface>/lstp.<type>.<instance>/{i2c-<n>,gpiochip<n>}` |
| `<interface>/{spi_master,tty,misc}/<dev>` | `<interface>/lstp.<type>.<instance>/{spi_master,tty,misc}/<dev>` |
| `<interface>/leds/<dev>` | `<interface>/lstp.gpio.<instance>/lstp.led.<instance>/leds/<dev>` |

The firmware channel number stays the stable key within an interface; only the
path it is read from moved. See "Stable identification by USB port path +
channel number" above for the lookup.

A UART, IPMI or MMIO node held open across an unbind and rebind of its
channel's driver no longer comes back under its old name. Earlier releases
kept the node's state with the channel, so the rebind reused the name, and a
held IPMI or MMIO descriptor carried on against the new bind. Now the
descriptor stays with the bind it was opened on: IPMI and MMIO return
`-ENODEV`, and a tty is hung up, as before. The node's number stays taken
until the last close, so the rebind takes the lowest free one. Reopen the node
after a rebind, and use a udev symlink where a fixed name matters.

Starting from a class device, `<classdev>/device` is the channel directory, so
`/sys/class/misc/ipmi-lstp0/device/enable` is that channel's `enable`. The
move also changed what lies *above* a subsystem device: a class device's
`device` link and every `../..` walk out of it resolve through the physical
parent chain, which gained the channel level. So `/sys/class/tty/ttyLSTP0/device`
is the channel's auxiliary device rather than the USB interface, and the same
holds for every `/sys/class/*` entry this driver creates except LEDs, whose
`device` is the LED auxiliary device one level further down. Read
`<classdev>/device/..` for the interface, `/sys/class/leds/<name>/device/..`
for an LED's GPIO channel, or keep the interface path the search started from.

Earlier releases built every subsystem driver into the one `lstp` module.
Each is now a module of its own (see [Modules](#modules)), which changes the
following:

- `modprobe lstp` loads only the core. The subsystem modules load on demand
  through udev; where nothing loads them, load them explicitly. Module lists
  that name only `lstp` -- initramfs configuration, `modules-load.d` files,
  distribution or image packaging -- need the subsystem modules added, either
  the ones for the channel types in use or all of them.
- Each auxiliary driver is named after the module that registers it, so
  `lstp.<type>` is now `<module>.<type>`, for example `gpio_lstp.gpio`
  or `ipmi_lstp.ipmi`; the table in [Modules](#modules) lists them all.
  That name is the driver directory under `/sys/bus/auxiliary/drivers/`,
  the target of a channel's `driver` link, the `DRIVER` value in its
  uevents that udev's `DRIVER` and `DRIVERS` keys match, and the driver
  name at the start of its kernel log messages. Scripts, udev rules and
  log filters that use the old name need the new one. Likewise
  `/sys/module/lstp/drivers/` lists only the USB driver, and each
  auxiliary driver appears under its own module.
- Unload the subsystem modules before `lstp`.

Device names (`lstp.<type>.<instance>`, `lstp.led.<instance>`), device nodes,
the USB driver directory `/sys/bus/usb/drivers/lstp` and the `auto_bind_spidev`
parameter, which stays on `lstp`, are unchanged.

## Device Tree Support

The device reports its channels, their types and their IDs at probe time, so
it needs no device tree to work. A description adds only what the device
cannot report: peripherals on its SPI and I2C buses, GPIO line names and hogs,
and LEDs; an `i2c` or `spi` alias also fixes that bus's number. The USB core
attaches a device tree node to a USB device by position alone, matching each
hub level on its port number, so a description suits only an LSTP device
hard-wired on the board. For a pluggable device, use the runtime discovery in
"Sysfs Topology" above.

The bindings live in [`devicetree/bindings/`](devicetree/bindings/). The
top-level binding is
[`usb/nvidia,lstp.yaml`](devicetree/bindings/usb/nvidia,lstp.yaml). SPI, I2C
and GPIO channels have their own bindings under `spi/`, `i2c/` and `gpio/`; the
IPMI and UART channel nodes are defined in `usb/nvidia,lstp.yaml`. They build
on the kernel's
[USB device binding](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/Documentation/devicetree/bindings/usb/usb-device.yaml).

Each channel node is matched to a firmware-reported channel by `reg` (the
channel ID) and `compatible` (`nvidia,lstp-{spi,i2c,gpio,ipmi,uart}`), and is
named for its type: `spi@N`, `i2c@N`, `gpio@N`, `ipmi@N` or `serial@N`. A
channel without a node is still created. A channel whose node has
`status = "disabled"` is not created at all.

Where the channel nodes go depends on the device's USB descriptors, following
the USB core's rules:

- A device of class 0 with one configuration and one interface has a
  *combined node*: the channels go directly under the device node, which has
  `#address-cells = <1>`.
- Any other device has an *interface node*, `interface@N` with
  `reg = <N C>` (interface number, configuration value), and the channels go
  under it. The device node has `#address-cells = <2>`.

To tell which applies, read the device's descriptors from sysfs. `00`, `1` and
`1` mean a combined node:

```bash
cat /sys/bus/usb/devices/1-1.3/{bDeviceClass,bNumConfigurations,bNumInterfaces}
```

The example below describes the device at USB path `1-1.3:1.2`: interface 2,
configuration 1, of the device on port 3 of the hub on root port 1. It goes
under the host controller node the device is attached to.

The `reg` of a node on a root-hub port, `hub@1` here, is the host
controller's raw port number (`usb_hcd_find_raw_port_number()`), which is not
always the port number in the USB path. On xHCI it is the controller's
hardware port number, and the USB2 and USB3 root hubs share one hardware port
array. A node on a port of an external hub, such as `device@3`, uses the port
number as the USB path shows it.

```dts
&usb {
    #address-cells = <1>;
    #size-cells = <0>;

    hub@1 {
        reg = <1>;
        #address-cells = <1>;
        #size-cells = <0>;

        device@3 {
            compatible = "usb955,cf11";
            reg = <3>;
            #address-cells = <2>;
            #size-cells = <0>;

            interface@2 {
                compatible = "usbif955,cf11.config1.2";
                reg = <2 1>;
                #address-cells = <1>;
                #size-cells = <0>;

                spi@1 {
                    compatible = "nvidia,lstp-spi";
                    reg = <1>;
                    #address-cells = <1>;
                    #size-cells = <0>;

                    flash@0 {
                        compatible = "jedec,spi-nor";
                        reg = <0>;
                        spi-max-frequency = <18750000>;
                    };
                };

                i2c@2 {
                    compatible = "nvidia,lstp-i2c";
                    reg = <2>;
                    #address-cells = <1>;
                    #size-cells = <0>;

                    temperature-sensor@48 {
                        compatible = "ti,tmp102";
                        reg = <0x48>;
                    };
                };

                gpio@3 {
                    compatible = "nvidia,lstp-gpio";
                    reg = <3>;
                    gpio-controller;
                    #gpio-cells = <2>;
                    gpio-line-names = "LED", "INT";
                };

                ipmi@4 {
                    compatible = "nvidia,lstp-ipmi";
                    reg = <4>;
                };

                serial@5 {
                    compatible = "nvidia,lstp-uart";
                    reg = <5>;
                };
            };
        };
    };
};
```

GPIO pins the device firmware reports as LEDs register LED class devices named
after their firmware pin names. A `leds` node under the GPIO channel describes
them by pin number. `label`, or `color` and `function`, name the LED class
device. `default-state` and `linux,default-trigger` set its initial behaviour.
`led-pattern` gives a list of `<brightness duration>` steps that is sent to the
device once, when the LED registers, and then played by the device on its own.
It takes the place of both: the binding rejects a node that combines
`led-pattern` with `default-state` or `linux,default-trigger`. A pattern that
does not fit in a single request is not sent, and an error is logged; with
64-byte USB endpoints a request holds ten steps. A node without `led-pattern`
or `default-state` turns its LED off, as `default-state = "off"` would, and
`"keep"` leaves it as the device firmware set it. A pin without a node keeps
its firmware name and is left as the device firmware set it.

```dts
gpio@3 {
    compatible = "nvidia,lstp-gpio";
    reg = <3>;
    gpio-controller;
    #gpio-cells = <2>;

    leds {
        #address-cells = <1>;
        #size-cells = <0>;

        led@4 {
            reg = <4>;
            color = <LED_COLOR_ID_AMBER>;
            function = LED_FUNCTION_STATUS;
            led-pattern = <255 100>, <0 100>, <255 100>, <0 700>;
        };
    };
};
```

spidev is not described in device tree. For raw userspace access to an SPI
channel, bind spidev to a described peripheral through its `driver_override`
attribute, or leave the channel undescribed and create the devices through
`new_device` (Linux 7.3 and later, when built with `CONFIG_SPI_DYNAMIC`) or
`auto_bind_spidev=1`; see [auto_bind_spidev](#auto_bind_spidev).

On ACPI platforms the same properties apply, written as `_DSD` packages on
`Device()` objects under the USB port object, with the interface number or
channel ID as the object's `_ADR` in place of `reg`. See
[`Documentation/firmware-guide/acpi/lstp.rst`](Documentation/firmware-guide/acpi/lstp.rst),
which also covers what happens to ACPI-described GPIO consumers when the
device is reset or rebound.

Earlier releases matched ACPI interface and channel objects on a `reg`
property in their `_DSD` and required `_ADR` to have the same value. Tables
written that way keep working. This driver does not read `reg` on ACPI, so a
table may keep it for those releases, which still read it. Two kinds of table
match differently now: an object whose `_ADR` and `reg` differ is matched on
its `_ADR`, and an interface or channel written as a `_DSD` data node instead
of a `Device()` no longer matches at all; the driver logs a warning naming
each such data node.

How the driver resolves these nodes on device tree, ACPI and software nodes,
and which layouts it warns about, is described for driver developers in
[`Documentation/driver-api/usb/lstp.rst`](Documentation/driver-api/usb/lstp.rst),
together with the channel lifetime rules that subsystem drivers rely on.

### Device Tree Changes

The bindings were brought in line with the kernel's USB device binding and
the generic device tree conventions. Device trees written against earlier
releases need the changes below. Validate an updated tree with `dt-validate`
against the bindings in `devicetree/bindings/`.

**Interface node `reg` takes two cells.** The USB core attaches an interface
node only when its `reg` holds both the interface number and the
configuration value. A tree with `#address-cells = <1>` on the device node and
`reg = <N>` on `interface@N` still loads, but every channel comes up
undescribed: no peripherals, no GPIO line names. The driver logs
a warning naming the node. Set `#address-cells = <2>` on the device node and
`reg = <N 1>` on the interface node.

**Single-interface devices use a combined node.** For a device of class 0 with
one configuration and one interface, the USB core attaches the device node to
the interface itself, so channels written under an `interface@0` node are not
found and come up undescribed. The tree still validates, but the driver logs
a warning naming the node. Move the channel nodes up into the device node,
delete `interface@0`, and set `#address-cells = <1>` on the device node.

**A disabled channel node removes the channel.** A channel whose node has
`status = "disabled"` is no longer created. It gets no auxiliary device and
no device node. Instance numbers, `ipmi-lstpN` names
and `ttyLSTPN` minors are handed out in creation order, so those of channels
created later can shift. Remove `status` to keep the channel. Where a stable
name matters, use a device-tree `i2c` or `spi` alias for those bus numbers, and
a udev symlink as in "Stable IPMI node names" for a `/dev` node.

**IPMI channel nodes take no `label`.** The IPMI node was named after the
channel's `label`; it is now always `ipmi-lstp<n>`, and with
`IPMI_POSTCODES=1`, `/dev/<label>-postcodes` becomes
`/dev/ipmi-lstp<n>-postcodes`.
A tree that still sets `label` fails validation. The driver logs once that the
property is ignored. Delete it, and point whatever opened `/dev/<label>` at the
symlink from "Stable IPMI node names".

**An LED node without `default-state` turns the LED off.** A `led@N` node
with neither `default-state` nor `led-pattern` used to leave the LED as the
device firmware set it, and now turns it off when the LED registers, as for
any other LED binding. Add `default-state = "keep"` to keep the old
behaviour. Pins without a node are not affected.

**The device compatible drops the leading zero.** Write `usb955,cf11`, not
`usb0955,cf11`, as for every other USB device. Boot behaviour does not change,
since nothing matches on this string, but a node with the old spelling is not
checked against the LSTP binding. The binding lists the supported devices:
`usb955,cf11`, `usb424,4940` and `usb174c,2214`.

**Channel nodes are named for their type.** `channel@N` is no longer accepted.
Rename each channel node to `spi@N`, `i2c@N`, `gpio@N`, `serial@N` or `ipmi@N`
to match its compatible. The driver never reads node names, but `of_node`
link targets and `/sys/firmware/devicetree` paths change with them.

**`ngpios` is no longer accepted.** The line count always comes from the
device and the driver ignored the property. Delete it.

**UART channel nodes take no child nodes.** The UART channel registers a plain
tty, so a peripheral described under it was never probed. Delete the child and
attach the peripheral from userspace, for example through a line discipline.

**The `nvidia,lstp-spidev` example is gone.** Nothing ever bound to that
compatible. Describe the real peripheral, or use one of the spidev options
above.

**The binding files moved** into `usb/`, `spi/`, `i2c/` and `gpio/`, with
`$id` values to match. SPI, I2C and GPIO channels have their own bindings; the
IPMI and UART channel nodes are defined in `usb/nvidia,lstp.yaml`. Update
tooling that refers to them by path.
