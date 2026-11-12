# Testing

How to check the driver on your own machine and device, and what to
send with a report. The commands need root where they show `sudo`.

## KUnit

The unit tests run in a kernel tree with `kunit.py`. They need Linux
v7.3-rc6 or later, because the in-tree build has no compat header.
Copy the core driver, its header and the two function drivers into
the tree and hook them in. The function drivers have no Kconfig
entries of their own yet, so add the two entries the `.kunitconfig`
asks for; the serial entry goes outside `if TTY`, so it names `TTY`:

```sh
cd linux
cp -r <drop>/drivers/usb/misc/obmf drivers/usb/misc/
cp <drop>/include/linux/usb/obmf.h include/linux/usb/
cp <drop>/drivers/i2c/busses/i2c-obmf.c drivers/i2c/busses/
cp <drop>/drivers/tty/obmf_tty.c drivers/tty/
echo 'source "drivers/usb/misc/obmf/Kconfig"' >> drivers/usb/misc/Kconfig
echo 'obj-$(CONFIG_USB_OBMF) += obmf/' >> drivers/usb/misc/Makefile
cat >> drivers/i2c/busses/Kconfig <<'EOF'
config I2C_OBMF
	tristate "OBMF-ICP I2C channels"
	depends on USB_OBMF
EOF
echo 'obj-$(CONFIG_I2C_OBMF) += i2c-obmf.o' >> drivers/i2c/busses/Makefile
cat >> drivers/tty/Kconfig <<'EOF'
config OBMF_TTY
	tristate "OBMF-ICP serial channels"
	depends on TTY && USB_OBMF
EOF
echo 'obj-$(CONFIG_OBMF_TTY) += obmf_tty.o' >> drivers/tty/Makefile
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
`obmf-i2c` and `obmf-tty`, which run the I2C and serial drivers on
the fake transport; `obmf-dialect` with the dialect support;
`obmf-lstp` with LSTP. Every test must pass. Some cases make a driver
warn or fail a probe on purpose, so the log has such lines; only the
results count. The serial cases that check the kernel log read it
from the console tracepoint, which the `.kunitconfig` enables with
`CONFIG_FTRACE=y`; on a kernel without tracepoints or printk they are
skipped.

Out of tree, `make OBMF_KUNIT_TEST=y` against a kernel with
`CONFIG_KUNIT` and `CONFIG_DTC` builds the core suites into
`obmf.ko`, and loading the module runs them; the `obmf-i2c` and
`obmf-tty` suites and the LSTP cases of those drivers need them
built in, so they are not part of it. That path has not been run
yet.

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
| `discovery failed: <errno>`, `channel 0 changed during discovery` | channel 0 could not be read; nothing binds. With the debug messages on, a reply of neither layout first logs `channel 0: <n> bytes, <m> or <m+2> expected` |
| `device busy` | an LSTP device stayed busy during discovery; nothing binds |
| `channel <n>: not enabled, ignored`, `channel <n>: configuration not valid, ignored`, `channel <n>: not enabled, configuration not valid, ignored` | the device left that channel disabled or without a valid configuration (set both in its manufacturing configuration); the others bind |
| `channel <n>: ... ignored` (other reasons) | one channel's header was refused; the others bind |
| `channel <n>: health <h>, error indicator <e>` | at bind the channel reported HEALTH_STATUS 3 (not ready) or 15 (channel error), or set its error indicator; it binds anyway |
| `no device name, label ...`, `no channel name, label ...` | the device left a name empty; harmless |
| `transport failed: <errno>, device stopped` | repeated USB errors; unbind and bind, or replug. An open tty is hung up, and opening it fails with `EIO` until the interface is bound again |
| `bulk in stalled`, `bulk out stalled` | the device stalled an endpoint; the driver clears the halt and goes on; a request that met an OUT stall fails |
| `bulk in clear halt failed: <errno>`, `bulk out clear halt failed: <errno>` | clearing the halt failed; on the IN endpoint the driver tries again, and repeated failures end in `transport failed` |
| `no response during unbind, device stopped` | the device stopped answering during an unbind |
| `channel <n>: no response in <ms> ms` | a request timed out; its function driver got `ETIMEDOUT`. On a serial port each timeout doubles the wait of the requests after it, up to 10 s, until the port is opened again, so `<ms>` grows. After a timeout the channel's next request waits until the late response comes, which is dropped and counted in `rx_late`, or until one default timeout (1 s) has passed; a stop or a suspend ends the wait. A response later than that can still be taken by the next request |
| `channel <n>: device error <status>` | the device failed a request with a status that says it could not do it (invalid, permanent error, unknown channel, or a code this driver does not know); the statuses a function driver acts on (busy, timeout, NACK) print nothing |
| `channel <n>: bad response, <len> bytes`, `channel <n>: bad response` | a reply that does not fit its request |
| `malformed message: short, ...`, `malformed message: size over the transfer, ...`, `malformed message: trailing bytes, ...`, `message for unknown channel <n>`, `channel <n>: bad device request, ...` | a message from the device the driver could not take |
| `channel <n>: answer not sent: <errno>` | the answer to a device request could not be sent |
| `channel <n>: device request queue full, request refused` | the device sent requests faster than the host answered them, and the host refused one with the default answer (a serial write gets a NACK of count 0, so the device can send it again); `request dropped` for one that needs no answer. `dreq_max_depth` (see "Counters") shows how close a channel came |
| `channel <n>: request not answered`, `channel <n>: device request kept at unbind` | a function driver left a device request unanswered; the core sent the default answer |
| `dropped <n> bytes: <errno>` | the device answered a serial write with an error status other than busy or a timeout, or with a malformed answer; those characters are not sent again; they count in the port's `tx_dropped` (see "Counters") |
| `transmit stalled: <reason>, <n> characters waiting` | for 5 s the device has refused (`refused`), answered busy (`busy`) or not answered (`timed out`) the next serial write; one line per stall; the characters stay and go out once the device takes them. A port held off by flow control (CTS) for 5 s gets the same line |
| `<n> characters not sent at close, discarded` | the last close found characters the device had not taken; with a write in flight, the line comes once it is answered or times out |
| `line speed 0`, `configuration data too short`, `request limit <n> too small` | a serial channel's configuration or limits were refused; it has no tty |
| `unsupported line <d><p><s>, using 8N1` | a serial channel's data bits, parity or stop bits are out of range; the tty runs at 8N1 |
| `channel <n>: node <name> ignored` | a child node with the channel's `reg` has another name than `i2c` or `serial` |
| `unsupported LSTP version <n>` | the LSTP device is not version 1; nothing binds |
| `channel <n>: short config (<n>), ignored`, `channel <n>: over budget, ignored` | an LSTP channel's configuration was too short, or its slices too many; the others bind |

An error is printed by name (`-EPROTO`), or as a number (`-71`) on a
kernel built without `CONFIG_SYMBOLIC_ERRNAME`. Each line is rate
limited and also counted; see "Counters". During discovery the
response and message lines stay quiet: a reply left from an earlier
binding is expected there, and discovery runs its pass again.

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

The driver offers no I2C bus recovery: a bus held by a target shows
as transfers that time out or report busy. Unplug and replug the
device to recover it.

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
`status_errors`, and `timeouts` should stay 0. After a timeout, a
response within the hold (1 s, see `no response in` in "The bind log")
is dropped and counted in `rx_late`; one that comes later can still
complete the next request (`KNOWN-ISSUES.md`, "a late I2C response").
If `timeouts` or `rx_late` keep rising, treat later I2C results as
suspect: unload and reload before going on.

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

Host writes the device refuses (NACK), answers busy, or does not
answer stay in the driver's 4096-byte buffer and are sent again 10 ms
after each refusal, busy answer or timeout, with no limit. Each
timeout doubles the wait for the answers after it, up to 10 s, until
the port is opened again, so a device that does not answer is tried
less and less often, at last about every 10 s. A write blocks once
the buffer is full. A device that refuses for 5 s gets one
`transmit stalled` line, also when flow control holds it off. A
device that answers more than 1 s after the driver's timeout can get
the same characters twice, and characters it refuses right after that
can count as sent; report any duplicated or missing bytes in `cmp` with
the kernel log. The last close waits up to 30 s for the characters
the device has not taken, then discards them with `characters not
sent at close`; `tcflush` discards them without a line.

A break; a break the device keeps refusing fails with `EIO`:

```sh
python3 -c 'import termios, os, sys
f = os.open(sys.argv[1], os.O_RDWR); termios.tcsendbreak(f, 0)' $T
```

If the serial channel has a firmware node, check that the serial
function device carries it and the tty does not:

```sh
S=$(readlink -f /sys/class/tty/${T#/dev/}/device); echo "S=$S"
readlink -f $S/of_node                 # device tree: the serial@<n> node
cat $S/firmware_node/path 2>/dev/null  # ACPI: the channel object
ls /sys/class/tty/${T#/dev/}/ | grep -c node   # 0: the tty has none
```

With the node disabled, the channel has no `obmf.serial` device and
no tty. A child with the channel's `reg` and another name logs
"channel <n>: node <name> ignored".

While the port is closed, the host refuses device writes that carry
characters, and the device is expected to keep them and send them
again. To check it, have the device send a known pattern while no
program has `$T` open, then open the port and read:

```sh
(stty raw -echo clocal; cat) < $T > rx-closed.bin &
sleep 5; kill %1
```

`rx-closed.bin` should start with the pattern. If it is missing or
cut, the device dropped what the host refused; see `KNOWN-ISSUES.md`.

The counters of the port, with the received and sent characters,
breaks, overruns, Carrier Down events (`dcd`) and the refusals for a
full buffer (`buf_overrun`):

```sh
python3 -c 'import fcntl, os, struct, sys
f = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
b = fcntl.ioctl(f, 0x545D, bytes(80))  # TIOCGICOUNT
n = ("cts dsr rng dcd rx tx frame overrun parity brk "
     "buf_overrun").split()
print(dict(zip(n, struct.unpack("11i", b[:44]))))' $T
```

Carrier Down hangs the tty up only when `CLOCAL` is clear. With
`stty -F $T -clocal` and a program reading `$T`, a Carrier Down event
from the device ends the read, and `dcd` goes up by one.

## Unbind, unplug and unload

A software unbind and bind:

```sh
echo -n $IF | sudo tee /sys/bus/usb/drivers/obmf/unbind
echo -n $IF | sudo tee /sys/bus/usb/drivers/obmf/bind
```

Repeat it during traffic, for example with a loop of I2C reads or the
serial loopback running, and also unplug the device during traffic.
The transfers in progress fail, an open tty is hung up, and the
interface binds again on replug. After `transport failed` an open
tty is hung up too, and opening it fails with `EIO` until the
interface is bound again. An oops, a warning, or a task that
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

The interface's `events` directory has one file per event of this
table. Each counts its lines of the table in "The bind log", also
those the rate limit kept out of the log, but not the ones discovery
keeps quiet. After a bind every file should read 0.

| file | line |
|---|---|
| `transport_failed` | `transport failed: <errno>, device stopped` |
| `in_stalled`, `out_stalled` | `bulk in stalled`, `bulk out stalled` |
| `in_clear_halt_failed`, `out_clear_halt_failed` | `bulk in clear halt failed: <errno>`, `bulk out clear halt failed: <errno>` |
| `no_response` | `channel <n>: no response in <ms> ms` |
| `device_error` | `channel <n>: device error <status>` |
| `bad_response` | `channel <n>: bad response, <len> bytes`, `channel <n>: bad response` |
| `rx_short` | `malformed message: short, ...` |
| `rx_size_over_transfer` | `malformed message: size over the transfer, ...` |
| `rx_trailing_bytes` | `malformed message: trailing bytes, ...` |
| `rx_unknown_channel` | `message for unknown channel <n>` |
| `bad_device_request` | `channel <n>: bad device request, ...` |
| `answer_not_sent` | `channel <n>: answer not sent: <errno>` |
| `channel_health` | `channel <n>: health <h>, error indicator <e>` |
| `device_request_queue_full` | `channel <n>: device request queue full, request refused` or `request dropped` |
| `tx_dropped` | `dropped <n> bytes: <errno>` |

```sh
sudo sh -c "grep . /sys/kernel/debug/usb/obmf/$IF/events/*"
```

A channel's `dreq_max_depth` is the most device requests it held at
once; 19 is the most it can hold.

Each serial port has its own counters, under its tty's name:
`tx_dropped`, the characters dropped after an error or at close, and
`tx_stalls`, the `transmit stalled` lines:

```sh
sudo sh -c "grep . /sys/kernel/debug/obmf_tty/ttyOBMF*/*"
```

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
sudo grep -r . /sys/kernel/debug/obmf_tty/ > obmf-logs/tty-counters.txt
tar czf obmf-logs.tgz obmf-logs
```

Add the `make` line you built with, the kernel version and
architecture, the device's protocol (OBMF-ICP or LSTP) and its
firmware version, the steps that led to the problem, and the issue
number from `KNOWN-ISSUES.md` if one fits. Run the failing step with
the debug messages on if you can.
