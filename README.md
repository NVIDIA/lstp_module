# Low Speed Transport Protocol (LSTP) Driver for Linux

Out-of-tree Linux USB driver for [Low-Speed Transport Protocol (LSTP)](devicetree/bindings/nvidia,lstp.yaml) device. See [LSTP Specification 1.0.pdf](LSTP%20Specification%201.0.pdf) for more details.

## Contributing

This project is currently not accepting contributions.

## Cheatsheet

Dependencies:
```
sudo apt install -y build-essential linux-headers-$(uname -r)
```

Build and load:

```
make build
make load

# Signing optional (see creating signing keys below):
sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 ~/LSTP_KEYS/MOK.priv ~/LSTP_KEYS/MOK.der ./build/lstp.ko
```

Unload:

```
make unload
```

### Signing

Required on any kernel that enforces signed modules: x86 distros with UEFI
Secure Boot (e.g. Ubuntu), BMC / OpenBMC builds, and other locked-down
kernels. Check with `mokutil --sb-state` or
`cat /sys/module/module/parameters/sig_enforce`.

One-time: create a key and enroll it (x86 Secure Boot via MOK shown here;
BMCs typically sign with a key already in the kernel's trusted keyring):

```
mkdir ~/LSTP_KEYS/
openssl req -new -x509 -newkey rsa:2048 -keyout ~/LSTP_KEYS/MOK.priv -outform DER -out ~/LSTP_KEYS/MOK.der -nodes -days 36500 -subj "/CN=NVLSTP/"
sudo mokutil --import ~/LSTP_KEYS/MOK.der
sudo reboot now
# In MOK manager: Enroll MOK > Continue > password > Finish and reboot
```

Every build: sign `lstp.ko` before `make load`:

```
sudo /usr/src/linux-headers-$(uname -r)/scripts/sign-file sha256 ~/LSTP_KEYS/MOK.priv ~/LSTP_KEYS/MOK.der ./build/lstp.ko
```

### Cross-build

Required dependencies:
```
sudo apt install -y build-essential bc bison flex libssl-dev libncurses5-dev git
```

You will also need your target cross toolchain. Using aarch64 for example:
```
sudo apt install -y gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

To build for a non-host kernel, point `KDIR` at a `modules_prepare`'d kernel
source tree and pass `ARCH`/`CROSS_COMPILE`.
For Raspberry Pi: clone your target branch of
[raspberrypi/linux](https://github.com/raspberrypi/linux) into `~/linux`,
run the board's `*_defconfig` and `modules_prepare` against it, then
`make build_rpi` (cross-compiles against `~/linux`).

## Module Parameters

### auto_bind_spidev

By default, the driver registers SPI controllers but does not create spidev
character devices on them. On systems without firmware node descriptions
(device tree or ACPI), spidev devices must be explicitly enabled:

```
make load auto_bind_spidev=1
```

On systems with device tree or ACPI, SPI child devices are enumerated
automatically from the firmware description and this parameter has no effect.

When the driver is built in-tree, the compile-time default is controlled by
`CONFIG_USB_LSTP_SPI_SPIDEV`.

## Using and Inspecting LSTP

### Driver Logs

Driver messages go to the kernel log. View with `dmesg`:

```
sudo dmesg | grep -i lstp
```

### Finding LSTP Devices

Bound USB interfaces appear as symlinks under the driver's sysfs directory:

```
$ find /sys/bus/usb/drivers/lstp -maxdepth 1 -type l
/sys/bus/usb/drivers/lstp/3-3.1:1.2
/sys/bus/usb/drivers/lstp/3-4.4:1.2
...
```

### Sysfs Topology

Each bound interface exposes an `lstp/` directory with a `name` and a
`channel/` subdirectory. Each channel has `name`, `enable`, and a `device`
symlink to the underlying Linux device:

> **Don't hardcode bus numbers, `spiX.Y`, `i2c-N`, or `gpiochipN`.** USB is
> dynamic: these IDs can change across boots, topologies, and hotplug order.
> Only a device-tree (or equivalent firmware) description gives stable
> ordering. Otherwise, always discover devices via sysfs.

```
$ tree -L 3 /sys/bus/usb/devices/1-1.2.2:1.2/lstp/
/sys/bus/usb/devices/1-1.2.2:1.2/lstp/
├── channel
│   ├── 1
│   │   ├── device -> ../../../spi_master/spi7
│   │   ├── enable
│   │   └── name
│   ├── 2
│   │   ├── device -> ../../../i2c-22
│   │   ├── enable
│   │   └── name
│   ├── 3
│   │   ├── device -> ../../../i2c-23
│   │   ├── enable
│   │   └── name
│   └── 4
│       ├── device -> ../../../gpiochip2
│       ├── enable
│       └── name
└── name
```

Discovery flow:

1. List bound interfaces in `/sys/bus/usb/drivers/lstp/`.
2. For each, iterate `lstp/channel/*/`.
3. Match on `channel/<N>/name` (the stable identifier).
4. Follow `channel/<N>/device` at runtime to get the node to open.

Example: find every SPI channel named `MY_BOARD_SPI` across all bound LSTP
devices and print its underlying SPI master:

```
for ch in /sys/bus/usb/drivers/lstp/*:*/lstp/channel/*/; do
    [ "$(cat "$ch/name")" = "MY_BOARD_SPI" ] || continue
    target=$(readlink -f "$ch/device")
    case "$target" in */spi_master/*) ;; *) continue ;; esac
    usb_path=$(readlink -f "$ch/../../..")

    echo "usb=$usb_path spi=$target"
done
```

## Device Tree Support Example
(For general USB device DTs, see: https://www.kernel.org/doc/Documentation/devicetree/bindings/usb/usb-device.txt)
```
&ehci1 {
    status = "okay";

    hub@1 {
        reg = <1>;
        hub@2 {
            reg = <2>;
                device@1 {
                    reg = <1>;
                    interface@2 {
                        reg = <2>;
                        #address-cells = <1>;
                        #size-cells = <0>;

                        channel@1 {
                            reg = <1>;
                            compatible = "nvidia,lstp-spi";
                            label = "spi-lstp-host0";
                            #address-cells = <1>;
                            #size-cells = <0>;

                            spidev@0 {
                                compatible = "nvidia,lstp-spidev";
                                reg = <0>;
                                spi-max-frequency = <18750000>;
                            };

                            spidev@1 {
                                compatible = "nvidia,lstp-spidev";
                                reg = <1>;
                                spi-max-frequency = <18750000>;
                            };
                        };

                        channel@2 {
                            reg = <2>;
                            compatible = "nvidia,lstp-ipmi";
                            label = "ipmi-lstp-host0";
                        };
                    };
                };
            };
        };
    };
};
```
