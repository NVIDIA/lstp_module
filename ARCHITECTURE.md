# Architecture

The driver has four layers. Each OBMF-ICP USB interface gets one
instance of the lower three; the function drivers bind the devices the
instance creates.

```text
  user space      /dev/i2c-N        /dev/ttyOBMFn
                      |                  |
  function        i2c-obmf.ko        obmf_tty.ko
  drivers             |                  |
                 obmf.i2c.<id>     obmf.serial.<id>   auxiliary bus
                      +--------+---------+
  obmf.ko                      |  include/linux/usb/obmf.h
                 function device registry (obmf-bus.c)
                 channel 0 and discovery  (obmf-ctrl.c)
                 message layer            (obmf-core.c)
                 [dialect, optional]      (obmf-dialect.c, obmf-lstp.c)
                 USB transport            (obmf-usb.c)
                               |
  USB                 bulk IN, bulk OUT
```

## The USB transport

`obmf-usb.c` binds the USB interface. Each entry of its match table
points to a protocol: a USB profile and, with the dialect support
built, a dialect (described below). The OBMF-ICP entry matches the
interface class triple of OBMF-ICP over USB 1.0, 5.1, on any vendor.

The profile says how the protocol uses the interface:

- whether the OBMF-ICP functional descriptor gives the transfer
  limits (OBMF-ICP over USB 1.0, 5.2), or whether each message is one
  packet with the limits taken from the bulk endpoints;
- whether a zero-length packet ends an OUT message that fills its
  last packet (OBMF-ICP over USB 1.0, 6.1);
- the largest packet, when there is no functional descriptor;
- a lower bound for every response wait.

For OBMF-ICP the descriptor is required. The probe refuses an
interface without it, with multimessage support set, with a major
version other than 1, or with transfer sizes below 64 bytes. Each
transfer carries one whole message, header included; the send limit
is the descriptor's write size, and each IN buffer holds the read
size plus one header.

Two bulk IN transfers are kept queued, and one bulk OUT transfer is in
flight at a time. A message is never sent twice: a failed or timed-out
send is an error for the caller.

On a stalled bulk IN endpoint the transport clears the halt and queues
the transfers again (OBMF-ICP over USB 1.0, 8.2). Other bulk IN errors
are retried after 250 ms. Ten bulk IN errors or stalls in a row stop
the device:
the core logs "transport failed (<errno>), device stopped", and every
later request fails with `-EIO` until the interface is bound again.
On a stalled bulk OUT endpoint the halt is cleared and that send fails
with `-EPIPE`. The driver does not reset the port or the device; that
is left to the USB core.

## The message layer

`obmf-core.c` holds one record per discovered channel, channel 0
included.

### Requests from the host

A channel has one request in flight at a time. A second caller on the
same channel waits until the first one is done; requests on different
channels run in parallel. The core builds the header (OBMF-ICP 1.0.0,
5.1), sends the message and waits for the response with the same
channel number. Nothing else matches a response to its request,
because the protocol carries no tag (see `KNOWN-ISSUES.md`, "a late
I2C response").

The response is checked before the caller sees it:

- a nonzero status becomes an errno, from the generic table of
  OBMF-ICP 1.0.0, 5.2 and the table of the channel type;
- on I2C, GPIO, IPMI and SPI channels the first payload byte must echo
  the request's command bits;
- on channel 0 and memory-mapped channels, a read response may start
  with the two-byte transaction head or carry the data alone; both are
  taken, and the length must be the one asked for;
- a response longer than the caller's buffer is `-EPROTO`.

The wait is the caller's timeout, or 1000 ms by default, at most
10000 ms, and at least the profile's floor. A request that gets no
response in time fails with `-ETIMEDOUT`; a response that arrives
later is counted as late and dropped. During a software unbind, the
first timeout stops the device ("no response during unbind, device
stopped"), so that the removal of the function drivers does not wait
a timeout for every request.

### Requests from the device

A message without the request/response bit is a device request. The
core queues it on its channel and delivers it in process context, on
an unbound workqueue of the instance, one at a time per channel. Each
channel keeps three records in reserve for requests that wait for an
answer, and allocates at most 16 more. A request that waits for an
answer and finds no record is not lost: the core owes its answer and
sends the default answer later.

A function driver takes device requests by passing its operations to
`devm_obmf_publish()`. Its `device_request()` returns 0 when it has
answered (or no answer is due), `-EINPROGRESS` to keep the request
and answer it later with `obmf_respond()` or end it with
`obmf_dreq_drop()`, or an error to have the core send the default
answer.

The default answer is the one the core sends when no driver answers:

- serial: a NACK with no characters accepted (OBMF-ICP 1.0.0, 5.5.2,
  Table 23);
- GPIO Interrupt Notify: success (5.4.1, Table 18); an Interrupt
  Notify Datagram waits for no answer;
- every other request: BUSY.

When a driver unbinds, the requests it kept get the default answer.

## Channel 0 and discovery

`obmf-ctrl.c` reads channel 0 with MMIO short reads (OBMF-ICP 1.0.0,
5.3) of at most 64 bytes each. One discovery pass reads:

1. the device header (5.15, Table 64): the major version must be 1;
2. the table of channel header offsets, one per channel;
3. each channel header (Table 65) and its configuration data;
4. the device header again, which must be unchanged.

A channel is left out, with a warning naming it, when its header gives
another channel number, overlaps the device header or another channel,
has a configuration above 32 KiB or beyond the 256 KiB budget of the
pass, or has payload limits too small for its type. The other channels
still bind. Each channel's payload limits are cut to what the
transport can carry.

A pass that times out, gets a malformed response, or sees the device
header change is discarded and run again, after the device has been
quiet for one response timeout. There are at most three passes within
30 seconds. Any other error fails the probe at once, with "discovery
failed: <errno>", or "channel 0 changed during discovery" when the
header kept changing.

### Enabled channels only

The device decides which channels are in use. Discovery keeps a
channel only if its CHANNEL_CONTROL has CHANNEL_ENABLE and its
CHANNEL_STATUS has CONFIG_VALID (Table 65); any other channel is left
out with a warning and gets no function device. HEALTH_STATUS and
CHANNEL_ERROR_INDICATOR are not checked (see `ASSUMPTIONS.md`). The
core never writes CHANNEL_ENABLE: it does not switch channels on at
bind or off at unbind. A channel is therefore on before its function driver binds;
device requests that arrive before the driver publishes its
operations get the default answer. LSTP channels have no such bits
and count as enabled and valid.

## The OBMF bus: one auxiliary device per channel

`obmf-bus.c` turns channels into devices on the kernel's auxiliary
bus. A channel gets a device when its type and role have a function
driver:

| CHANNEL_TYPE | DEVICE_ROLE | device name |
|---|---|---|
| I2C controller (0x04) | producer | `obmf.i2c.<id>` |
| serial (0x03) | producer or symmetric | `obmf.serial.<id>` |

Every other channel is discovered and logged at debug level, but gets
no device yet. `<id>` is unique across all OBMF-ICP
interfaces in the system; it is not the channel number.

The steps after a successful discovery:

1. For each channel that will get a device, the core looks for its
   firmware node below the USB interface's node (`REQUIREMENTS.md`
   gives the rules and examples). A node that is disabled, or an ACPI
   object that is not present, means the channel gets no device.
2. The channel table goes live: from here on, received messages reach
   the channels.
3. The core creates the auxiliary devices, children of the USB
   interface, each with its firmware node, if any, and the attributes
   `name` (CHANNEL_NAME) and `channel` (the channel number).
4. The auxiliary bus matches each device to its driver by name. With
   the modules installed, the `auxiliary:` modalias loads the driver.

A function driver gets a `struct obmf_fdev` with the channel's
information (number, type, role, name, payload limits, configuration
data and per-channel quirks) and calls the functions of
`include/linux/usb/obmf.h`, exported in the `OBMF` symbol namespace:

- `devm_obmf_enable()`, first in probe, and `devm_obmf_publish()`,
  last in probe, which also hands over the device request operations;
- `obmf_request()`, one request and its response;
- `obmf_respond()` and `obmf_dreq_drop()`, for device requests;
- `obmf_ch_status_read()` and `obmf_ch_control_update()`, the
  channel's status and its channel-specific control byte;
- `obmf_fdev_label()`, a name of the form `<device name>_<channel
  name>` for what the driver registers;
- `obmf_fdev_protocol_error()`, to count a malformed response the
  driver found.

## The function drivers

### I2C (`i2c-obmf.ko`)

Each I2C message is one channel request: Read, Write, or a Write
followed by a Read of the same target as one Write Read (OBMF-ICP
1.0.0, 5.6.1). Every request of a transfer but the last asks for no
STOP. All messages are checked before the first is sent: 7-bit
addresses only, no zero-length messages, and no message flags other
than read (and DMA-safe). The largest read
and write follow from the channel's payload limits and are set as
adapter quirks. SMBus is emulated on top of I2C, except Quick.

The bus speed comes from the channel's configuration data (5.15.2,
Table 79). A `clock-frequency` property in the firmware node must
match it, or the probe fails; the driver cannot change the speed.
When the configuration offers bus recovery, a timeout or busy status
starts one bus recovery through the channel-specific control and
status bytes (5.15.1, Table 69), polled ten times, 10 ms apart. The I2C
core retries a lost arbitration three times, unless the channel's
quirks say that the device retries itself.

### Serial (`obmf_tty.ko`)

Each serial channel gets `/dev/ttyOBMF<n>`. Host writes are queued in
a 4 KiB buffer and sent as Write requests (5.5.1, Table 22) of up to
the channel's limit. Characters the device does not accept are sent
again; after 20 refusals in a row, 10 ms apart, or at once when a
request fails, they are dropped with a "dropped <n> bytes" warning.
A break is one Generate Break event.

Device writes are taken whole or not at all: when the tty buffer has
no room, the answer is a NACK with no characters accepted. Characters
that arrive while the port is closed are dropped and acknowledged, as
on a UART with no reader. Break and overrun events are passed to the
tty layer and counted for `TIOCGICOUNT`.

The line settings are those of the channel's configuration data
(Table 78). The driver reports them in termios and cannot change them
yet (`KNOWN-ISSUES.md`).

## Dialects: devices with another wire protocol

A dialect lets the core serve a device whose interface speaks another
wire protocol, as long as that protocol uses the OBMF-ICP message
header layout: channel, status with the request/response bit, and
size. The core routes every message by its channel and that bit
before a dialect sees it, and works on the OBMF-ICP form only. The
function drivers and user space never see the dialect.

The seam is `struct obmf_dialect` in `obmf-dialect.h`. Every member is
optional, and a member left unset means the native OBMF-ICP step:

| member | replaces | when unset |
|---|---|---|
| `encode` | the outgoing message, request or answer: rewritten into the wire form; may drop an answer the wire protocol has no form for | the message goes out as it is |
| `decode` | the incoming message: rebuilt in OBMF-ICP form, response or device request | the message is routed as received |
| `discover` | one discovery pass, filling the same channel records | the native pass over channel 0 |
| `status_to_errno` | the errno of a wire status with no OBMF-ICP code | the OBMF-ICP status tables |
| `ch_reg` | reads and writes of a channel header byte (status, control, channel-specific status and control) | MMIO reads and writes of channel 0 |
| `ch0_whole_msgs` (flag) | channel 0 is read in whole wire messages | 64-byte chunks |
| `keep_ch_limits` (flag) | the channel limits are the device's, in OBMF-ICP form, and are not cut to the transport's | cut to the transport |

`obmf_dialect_attach()` checks the table when the interface is probed.
The probe fails with `-EINVAL` and a warning for a table that sets
only one of `encode` and `decode`, that sets nothing, or that sets
`keep_ch_limits` without `encode`.

A dialect also brings its USB profile in its match entry (above), and
can set per-channel quirks in the channel information for the function
drivers. One quirk exists today: the device retries a lost I2C
arbitration itself, so the I2C driver adds no retries.

With `CONFIG_OBMF_DIALECTS` off (no `OBMF_DIALECTS=y` and no
`OBMF_LSTP=y` out of tree), the dialect code is compiled out: the
fields and calls that serve it are gone, and `obmf.ko` is the native
driver only.

### LSTP

With `CONFIG_OBMF_LSTP` (`OBMF_LSTP=y`), `obmf-lstp.c` adds a dialect
for LSTP 1.0 devices and match entries for them (`REQUIREMENTS.md`).
Its USB profile has no functional descriptor and no zero-length
packet: each message is one bulk packet of at most 512 bytes, and
every response wait is at least 1000 ms. What it translates:

- discovery: the device header and each channel's configuration are
  read with READ_CONFIG (LSTP 1.0, 4.1.1 and 4.1.2), in slices that
  each fit one packet, and the device header is read again at the end
  of the pass, as in the native pass;
- I2C controller channels: Read, Write, Write Read and the no-STOP
  bit, both ways, and the LSTP statuses NACK and arbitration lost to
  their OBMF-ICP I2C codes (LSTP 1.0, 4.4.2);
- UART channels as OBMF-ICP serial channels: host writes and their
  acknowledgement, device writes and the host's answers (LSTP 1.0,
  4.5.2), with the first seven configuration bytes as the line
  settings (Table 78);
- the channel header bytes, which LSTP lacks: every channel reads as
  healthy, enabled and with a valid configuration, and an I2C bus
  recovery request sends the LSTP bus recovery command.

The channel limits are the packet size less the header, plus what the
OBMF-ICP form of the type adds. SPI, GPIO and IPMI channels get a
record but no translation and no device yet. Channels of other LSTP
types (vendor, OEM or reserved) are left out with a warning. LSTP
devices give no PCI-SIG vendor ID, so the core records 0 for it.

An LSTP device that answers BUSY to its device header is asked again
in the next pass. If it is still busy on the last pass, the probe
fails with "device busy" and `-EBUSY`.

## Lifetime

**Bind.** The probe checks the interface, starts the bulk transfers,
runs discovery, looks up the firmware nodes, and then creates the
function devices. A probe that fails leaves nothing behind. A probe
that succeeds with no function device is valid: the counters and the
debug log still describe the device.

**Software unbind** (writing to the USB driver's `unbind` file, or
unloading `obmf.ko`). The device is still there, so the function
devices are removed first, while requests still work: their drivers
answer the device requests they kept. Then the
transfers stop. A function driver that sends a request after the
device stopped responding stops the device at the first timeout, so
the unbind takes at most one timeout.

**Unplug.** The device is gone, so the transfers stop first. Waiting
requests fail at once with `-ENODEV`, and then the function devices
are removed.

**Module unload.** `i2c-obmf.ko` and `obmf_tty.ko` use symbols of
`obmf.ko`, so they are unloaded first; each unbinds its devices.
Unloading `obmf.ko` then unbinds every interface the software way. An
open `/dev/i2c-N` holds `i2c-obmf.ko` (`KNOWN-ISSUES.md`); an
open tty is hung up when its device goes.

**Transport failure.** After ten bulk IN errors in a row the device
is stopped and every request fails with `-EIO`. The function devices
stay until the interface is unbound, or the device is unplugged.

**System suspend and resume.** Each function device suspends before
the interface, because it is the interface's child: the I2C adapter
is marked suspended, and the serial port stops sending. The interface
then waits for the requests in flight, stops delivering device
requests, and stops the transfers. New requests fail with `-EAGAIN`
until resume. Resume starts the transfers again and delivers the
device requests that were queued. A resume that cannot start the
transfers stops the device, as a transport failure. When the USB core
resets the device during resume, the interface is unbound and bound
again, with a new discovery and new function devices.

**Runtime power management.** The driver has no autosuspend support,
so the USB core keeps the device active while it is bound.

## Counters

Each bound interface has a debugfs directory
`usb/obmf/<interface>/` (for example `usb/obmf/1-1:1.0/`) with these
read-only counters:

| file | counts |
|---|---|
| `rx_messages`, `tx_messages` | messages received and sent |
| `rx_malformed` | received messages shorter than a header or than their header size, dropped; longer ones, kept without the extra bytes; with a dialect, messages it cannot decode |
| `rx_no_channel` | messages for a channel with no record |
| `rx_urb_errors`, `tx_urb_errors` | failed bulk IN and bulk OUT transfers |
| `stalls` | stalled endpoints |
| `fatal_errors` | transport failures that stopped the device |

A subdirectory `ch<n>/` exists for channel 0 and for each channel with
a function device:

| file | counts |
|---|---|
| `tx_requests`, `rx_responses` | host requests sent and their responses |
| `rx_late` | responses after their request timed out, dropped |
| `timeouts` | host requests that got no response in time |
| `status_errors` | responses with a nonzero status |
| `protocol_errors` | malformed responses |
| `tx_errors` | requests or answers that could not be sent |
| `rx_device_requests` | device requests received |
| `tx_answers` | answers sent by a function driver |
| `default_answers` | default answers sent by the core |
| `rx_dropped` | device requests dropped: no room, or no wire form for the answer |
| `rx_replaced` | reserved, stays 0 |

With `CONFIG_FAULT_INJECTION_DEBUG_FS`, the interface directory also
holds `fail_bulk_in/` and `fail_bulk_out/`. They follow the kernel's
fault injection attributes, and their `error` file sets the errno a
failed transfer gets (default `EPROTO`).
