# Testing

How to check the driver on your own machine and device, and what to
send with a report. The commands need root where they show `sudo`.

## KUnit

The unit tests run in a kernel tree with `kunit.py`. They need Linux
v7.3-rc6 or later, because the in-tree build has no compat header.
Copy the core driver and its header into the tree and hook it in:

```sh
cd linux
cp -r <drop>/drivers/usb/misc/obmf drivers/usb/misc/
cp <drop>/include/linux/usb/obmf.h include/linux/usb/
echo 'source "drivers/usb/misc/obmf/Kconfig"' >> drivers/usb/misc/Kconfig
echo 'obj-$(CONFIG_USB_OBMF) += obmf/' >> drivers/usb/misc/Makefile
```

The native suites:

```sh
./tools/testing/kunit/kunit.py run --arch=x86_64 \
	--kunitconfig=drivers/usb/misc/obmf/.kunitconfig
```

With the dialect support, and with LSTP:

```sh
./tools/testing/kunit/kunit.py run --arch=x86_64 \
	--kunitconfig=drivers/usb/misc/obmf/.kunitconfig \
	--kconfig_add CONFIG_OBMF_DIALECTS=y

./tools/testing/kunit/kunit.py run --arch=x86_64 \
	--kunitconfig=drivers/usb/misc/obmf/.kunitconfig \
	--kconfig_add CONFIG_OBMF_DIALECTS=y \
	--kconfig_add CONFIG_OBMF_LSTP=y \
	--kconfig_add CONFIG_OBMF_LSTP_KUNIT_TEST=y
```

The suites are `obmf-core`, `obmf-ctrl`, `obmf-bus` and `obmf-usb`;
`obmf-dialect` with the dialect support; `obmf-lstp` with LSTP. Every
test must pass.

Out of tree, `make OBMF_KUNIT_TEST=y` against a kernel with
`CONFIG_KUNIT` builds the same suites into `obmf.ko`, and loading the
module runs them. That path has not been run yet.

## Build and load

```sh
make                                # the running kernel
make KDIR=<kernel build directory>  # another kernel
make OBMF_LSTP=y OBMF_DEBUG=y       # with LSTP, with debug messages
ls *.ko                             # obmf.ko i2c-obmf.ko obmf_tty.ko
```

The kernel must be configured as `REQUIREMENTS.md` says. Mark the log
and load the modules, core first:

```sh
echo "obmf-start $(date -Is)" | sudo tee /dev/kmsg
sudo insmod obmf.ko
sudo insmod i2c-obmf.ko
sudo insmod obmf_tty.ko
```

The device can be plugged in before or after. The modules are out of
tree and unsigned, so the first load taints the kernel; record
`/proc/sys/kernel/tainted` for a report.

## The bind log

After discovery, `obmf` logs one line per device at info level: the
protocol and its version, the device name, and each channel with its
kind, number and name. A channel that no function driver in this drop
serves (GPIO, for example) is marked `(no driver)`. A line that would
grow too long ends in `and <n> more`. An OBMF-ICP device:

```text
obmf 1-1:1.2: OBMF-ICP 1.0 device 'MyDevice': I2C ch1 'I2C', serial ch2 'UART'
```

An LSTP device:

```text
obmf 1-1:1.2: LSTP 1 device 'MyDevice': I2C ch2 'I2C', GPIO ch3 'gpio' (no driver), serial ch5 'UART'
```

On a good bind nothing else from `obmf` appears at the default log
level, except a `channel <n>: ... ignored` warning for each channel
the driver refuses (see the table below).

The details are debug messages. On a kernel with
`CONFIG_DYNAMIC_DEBUG`, switch them on at load time with
`sudo insmod obmf.ko dyndbg=+p`, or later, before the device is
plugged in, with:

```sh
echo 'module obmf +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
```
 On other kernels, build with
`OBMF_DEBUG=y`. A good OBMF-ICP bind then shows one line per channel
and one for the device:

```text
obmf 1-1:1.2: MMIO read responses: 2-byte head, then data
obmf 1-1:1.2: channel 1: type 0x4 role 0 name 'I2C' status 0x21 control 0x1 limits 504/504 config 2
obmf 1-1:1.2: channel 2: type 0x3 role 0 name 'UART' status 0x21 control 0x1 limits 504/504 config 7
obmf 1-1:1.2: OBMF-ICP 1.0 device 1234:0001 'MyDevice', 2 channels
```

The first line says which channel 0 read response layout the device
uses: the two-byte transaction head before the data, or data only.
An LSTP device shows:

```text
obmf 1-1:1.2: channel 1: LSTP type 0x3 name 'I2C' limits 511/512 config 11
obmf 1-1:1.2: channel 2: LSTP type 0x4 name 'UART' limits 511/511 config 15
obmf 1-1:1.2: LSTP 1 device 'MyDevice', 2 channels
```

LSTP type 0x3 is I2C and 0x4 is UART.

Messages that point to a problem:

| message | meaning |
|---|---|
| `no OBMF-ICP functional descriptor`, `unsupported OBMF-ICP version`, `transfer sizes ... too small`, `no bulk endpoints` | the interface was refused; nothing binds |
| `discovery failed: <errno>`, `channel 0 changed during discovery` | channel 0 could not be read; nothing binds |
| `device busy` | an LSTP device stayed busy during discovery; nothing binds |
| `channel <n>: not enabled, ignored`, `channel <n>: configuration not valid, ignored`, `channel <n>: not enabled, configuration not valid, ignored` | the device left that channel disabled or without a valid configuration (set both in its manufacturing configuration); the others bind |
| `channel <n>: ... ignored` (other reasons) | one channel's header was refused; the others bind |
| `no device name, label ...`, `no channel name, label ...` | the device left a name empty; harmless |
| `transport failed (<errno>), device stopped` | repeated USB errors; unbind and bind, or replug |
| `no response during unbind, device stopped` | the device stopped answering during an unbind |
| `dropped <n> bytes: <errno>` | the device refused a serial write 20 times, or did not answer |
| `unsupported LSTP version <n>` | the LSTP device is not version 1; nothing binds |

Then the devices:

```sh
IF=$(ls /sys/bus/usb/drivers/obmf | grep ':'); echo "IF=$IF"
cat /sys/bus/usb/devices/$IF/obmf/name
ls /sys/bus/auxiliary/devices/
grep . /sys/bus/auxiliary/devices/obmf.*/channel \
	/sys/bus/auxiliary/devices/obmf.*/name
ls -l /sys/bus/auxiliary/devices/obmf.*/driver
```

With more than one bound interface, set `IF` to the one under test.

## I2C

Find the adapter by the interface, not by its number:

```sh
sudo modprobe i2c-dev
for d in /sys/bus/i2c/devices/i2c-*; do
	readlink -f $d | grep -qF "/$IF/obmf.i2c." &&
		echo "${d##*-} $(cat $d/name)"
done
N=<the number printed>
```

Start with one read of a target you know, here 0x50:

```sh
sudo i2ctransfer -y $N r1@0x50; echo rc=$?
```

| output | meaning |
|---|---|
| one byte, rc=0 | the target answered |
| `No such device or address` | NACK: no target at that address |
| `Device or resource busy` | the device reported the bus busy; check the pull-ups before going on |
| `Connection timed out` | no response, or the device's I2C timeout; collect the logs |

An EEPROM through the at24 driver tests the Write Read path:

```sh
echo 24c02 0x50 | sudo tee /sys/bus/i2c/devices/i2c-$N/new_device
ls -l /sys/bus/i2c/devices/$N-0050/driver      # at24
sudo cat /sys/bus/i2c/devices/$N-0050/eeprom > eeprom.bin
echo 0x50 | sudo tee /sys/bus/i2c/devices/i2c-$N/delete_device
```

Compare `eeprom.bin` with what you know the EEPROM holds. If at24
does not bind, its first read failed; the kernel log says why.

A scan must use read probes, because the adapter has no SMBus Quick:

```sh
sudo i2cdetect -y -r $N
```

`i2cdetect` prints `--` for every error alike, so read the channel's
counters afterwards: NACKs and busy statuses count as
`status_errors`, and `timeouts` should stay 0. If `timeouts` is above
0, see `KNOWN-ISSUES.md`, "a late I2C response".

An open `/dev/i2c-N` delays an unplug or unload until it is closed
(`KNOWN-ISSUES.md`): do not leave an I2C tool running.

## Serial

```sh
T=/dev/$(ls /sys/bus/auxiliary/devices/obmf.serial.*/tty/); echo "T=$T"
stty -F $T -a
```

`stty` shows the device's line settings; they cannot be changed. For a
loopback, join the channel's transmit and receive lines on the
device side. Set the port up and read it in one command, so that no
echo is ever on, then send 100,000 random bytes and compare:

```sh
head -c 100000 /dev/urandom > tx.bin
(stty raw -echo clocal; cat) < $T > rx.bin &
sleep 0.5; cat tx.bin > $T
sleep 15; kill %1                 # 15 s covers 115200 baud
cmp tx.bin rx.bin && echo SAME
```

A break:

```sh
python3 -c 'import termios, os, sys
f = os.open(sys.argv[1], os.O_RDWR); termios.tcsendbreak(f, 0)' $T
```

Bytes the device sends while the port is closed are dropped, as on a
UART with no reader.

## Unbind, unplug and unload

A software unbind and bind:

```sh
echo -n $IF | sudo tee /sys/bus/usb/drivers/obmf/unbind
echo -n $IF | sudo tee /sys/bus/usb/drivers/obmf/bind
```

Repeat it during traffic, for example with a loop of I2C reads or the
serial loopback running, and also unplug the device during traffic.
The transfers in progress fail, an open tty is hung up, and the
interface binds again on replug. An oops, a warning, or a task that
hangs is a finding.

Unload in reverse order:

```sh
sudo rmmod obmf_tty
sudo rmmod i2c-obmf
sudo rmmod obmf
```

If something hangs: close the programs that use `$T` and
`/dev/i2c-$N`, then unbind; if the unbind hangs, unplug the device,
which needs no answer from it. Then record the blocked tasks with
`echo w | sudo tee /proc/sysrq-trigger` and collect the logs.

## Counters

```sh
sudo sh -c "grep . /sys/kernel/debug/usb/obmf/$IF/*"       # interface
sudo sh -c "grep . /sys/kernel/debug/usb/obmf/$IF/ch*/*"   # channels
```

The shell runs as root because debugfs is usually readable by root
only, so a glob of the calling user finds nothing.

`ARCHITECTURE.md` explains each counter. After a clean run, only the
message, request, response and answer counts are above 0, and
`status_errors` counts the NACKs of a scan.

## What to send with a report

```sh
mkdir obmf-logs
dmesg | sed -n '/obmf-start/,$p' > obmf-logs/dmesg.txt
uname -a > obmf-logs/uname.txt
cat /proc/sys/kernel/tainted > obmf-logs/tainted.txt
modinfo obmf.ko > obmf-logs/modinfo.txt
lsusb -v -d <vid>:<pid> > obmf-logs/lsusb.txt 2>&1
ls -lR /sys/bus/auxiliary/devices/ > obmf-logs/aux.txt
sudo grep -r . /sys/kernel/debug/usb/obmf/ > obmf-logs/counters.txt
tar czf obmf-logs.tgz obmf-logs
```

Add the `make` line you built with, the kernel version and
architecture, the device's protocol (OBMF-ICP or LSTP) and its
firmware version, the steps that led to the problem, and the issue
number from `KNOWN-ISSUES.md` if one fits. Run the failing step with
the debug messages on if you can.
