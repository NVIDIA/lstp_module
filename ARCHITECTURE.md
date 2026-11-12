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

On a stalled bulk IN endpoint the transport logs "bulk in stalled",
clears the halt and queues the transfers again (OBMF-ICP over USB 1.0,
8.2); a halt clear that fails logs "bulk in clear halt failed:
<errno>". Other bulk IN errors are retried after 250 ms, with a debug
message. Ten bulk IN errors or stalls in a row stop the device: the
core logs "transport failed: <errno>, device stopped", tells the
function drivers (see "Lifetime"), and every later request fails with
`-EIO` until the interface is bound again. On a stalled bulk OUT
endpoint the transport logs "bulk out stalled", the halt is cleared
(a failure logs "bulk out clear halt failed: <errno>" and counts as a
failed bulk OUT transfer) and that send fails with `-EPIPE`. None of
these lines is logged while the device goes away (`-ENODEV`). The driver does not reset the port or the device; that
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
response in time fails with `-ETIMEDOUT` and logs "channel <n>: no
response in <ms> ms". The channel's next request then waits until
the late response arrives, which is counted as late and dropped, or
for one default timeout (1000 ms), so that response does not answer
it. A stop or suspend of the device ends such a wait. A response that
comes after it ended is still taken by the next request (see
`KNOWN-ISSUES.md` and `ASSUMPTIONS.md`). A send that times out sets
no such wait. During a software unbind, the
first timeout, of a send or of a response, stops the device ("no
response during unbind, device stopped"). From then on requests fail
at once and no answer is sent, so that the removal of the function
drivers does not wait a timeout for every request.

### Events

What the device does wrong is reported through one helper: it counts
the event in the interface's `events/` directory in debugfs, then
logs one line, at most 10 lines per 5 s from each place in the code,
for all devices together (the kernel's default rate limit). The
events are the transport failure, a stall or a failed halt clear on
either endpoint, a response timeout, a failure status, a response
that does not fit its request, a message the core cannot take (too
short, larger than its transfer, trailing bytes, for an unknown
channel), a device request that cannot be decoded, an answer that
cannot be sent, an unhealthy channel at bind, and a full device
request queue. The statuses a function driver acts on (busy, timeout,
I2C NACK) are not logged, nor is a timeout during unbind, where the
core stops and says so. During discovery, timeouts, bad responses and
messages for unknown channels are taken as leftovers of an earlier
binding and stay quiet. Errors are printed by name (`-EPROTO`), or as
numbers on a kernel without `CONFIG_SYMBOLIC_ERRNAME`.

Function drivers report their own events through the same helper, on
their function device: the serial driver's dropped characters
(`tx_dropped`). A malformed response a function driver finds is
reported as the core's bad response.

### Requests from the device

A message without the request/response bit is a device request. The
core queues it on its channel and delivers it in process context, on
an unbound workqueue of the instance, one at a time per channel. Each
channel keeps three records in reserve for requests that wait for an
answer, and allocates at most 16 more. A request that waits for an
answer and finds no record is not lost: the core owes its answer and
sends the default answer later. Each request that finds no record is
reported, "channel <n>: device request queue full, request refused",
or "... request dropped" for one that waits for no answer, and the
channel's `dreq_max_depth` keeps the most records it had in use at
once.

A function driver takes device requests by passing its operations to
`devm_obmf_publish()`. Its `device_request()` returns 0 when it has
answered (or no answer is due), `-EINPROGRESS` to keep the request
and answer it later with `obmf_respond()` or end it with
`obmf_dreq_drop()`, or an error to have the core send the default
answer. A return of 0 that left a due answer unsent also gets the
default answer, with the one-time warning "channel <n>: request not
answered".

The default answer is the one the core sends when no driver answers:

- serial: a NACK with no characters accepted (OBMF-ICP 1.0.0, 5.5.2,
  Table 23);
- GPIO Interrupt Notify: success (5.4.1, Table 18); an Interrupt
  Notify Datagram waits for no answer;
- every other request: BUSY.

A device request that cannot be decoded reaches no driver: a serial
write whose Character Count runs past its data (5.5.1, Table 22), or
an empty IPMI request. The core answers it with the invalid status
(5.2, Table 4) and no body past the IPMI echo, so that the device does
not send it again as it would after a NACK, and counts it in
`rx_malformed`. On a channel, the queued requests go first, then the
default answers owed for want of a record, then the invalid answers.

When a driver unbinds, the core first lets the channel's delivery
work finish, which answers the requests still queued by default, and
then gives each request the driver kept the default answer, with
the one-time warning "channel <n>: device request kept at unbind".

## Channel 0 and discovery

`obmf-ctrl.c` reads channel 0 with MMIO short reads (OBMF-ICP 1.0.0,
5.3) of at most 64 bytes each. One discovery pass reads:

1. the device header (5.15, Table 64): the major version must be 1;
2. the table of channel header offsets, one per channel;
3. each channel header (Table 65) and its configuration data;
4. the device header again, which must be unchanged.

A channel is left out, with a warning naming it, when its header gives
another channel number, overlaps the device header or another channel,
runs past the 32-bit configuration space, has a configuration above
32 KiB or beyond the 256 KiB budget of the pass, or has payload limits
too small for its type. The other channels
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
CHANNEL_ERROR_INDICATOR do not decide: a kept channel whose
HEALTH_STATUS is 3 (not ready) or 15 (channel error), or that sets
CHANNEL_ERROR_INDICATOR, binds with one warning per bind, "channel
<n>: health <h>, error indicator <i>", after the discovery pass that
is kept (see `ASSUMPTIONS.md`). The
core never writes CHANNEL_ENABLE: it does not switch channels on at
bind or off at unbind. A channel is therefore on before its function
driver binds;
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

Both kinds look for a firmware node: I2C channels a node named `i2c`,
serial channels one named `serial`. Every other channel is discovered
and logged at debug level, but gets no device yet. `<id>` is unique
across all OBMF-ICP interfaces in the system; it is not the channel
number.

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
- `obmf_fdev_label()`, a name of the form `<device name>_<channel
  name>` for what the driver registers;
- `obmf_fdev_protocol_error()`, to count a malformed response the
  driver found;
- `obmf_fdev_report()`, to count and log one of the function
  driver's own events, rate limited per place in the code;
- `obmf_fdev_errno()`, the errno a request would fail with now (0
  while the device works, `-EIO` after a transport failure).

The operations a driver publishes may include `failed`: the core
calls it once, in process context, after a transport failure stopped
the device. It runs under the lock that delivers device requests, so
it must not unbind its device, publish, or wait for one of its own
device requests. A driver that publishes after the failure is
refused.

## The function drivers

### I2C (`i2c-obmf.ko`)

Each I2C message is one channel request: Read, Write, or a Write
followed by a Read of the same target as one Write Read (OBMF-ICP
1.0.0, 5.6.1). Every request of a transfer but the last asks for no
STOP. All messages are checked before the first is sent: 7-bit
addresses only, no zero-length messages, and no message flags other
than read, DMA-safe and the PEC flag the SMBus emulation copies from
a client. The largest read
and write follow from the channel's payload limits and are set as
adapter quirks. SMBus is emulated on top of I2C, except Quick.

The bus speed comes from the channel's configuration data (5.15.2,
Table 79). A `clock-frequency` property in the firmware node must
match it, or the probe fails; the driver cannot change the speed.
The adapter registers no bus recovery with the I2C core, also when
the configuration offers one (5.15.2, Table 79, options bit 0): no
status says that the bus is held, and a recovery sent after a timeout
could take the late answer as its own. `i2c_recover_bus()` therefore
returns `-EBUSY`, and a failed transfer returns its error; the client
decides. The I2C core retries a lost arbitration three times, unless
the channel's quirks say that the device retries itself.

### Serial (`obmf_tty.ko`)

Each serial channel gets `/dev/ttyOBMF<n>`. The tty is registered
below the serial function device and has no firmware node of its own;
a udev rule for the tty can read the node's properties from its
parent. The port is not registered through serdev, so no device
described below the node is bound.

Host writes are queued in a 4 KiB buffer and sent as Write requests
(5.5.1, Table 22) of up to the channel's limit. The wait for each
answer is the default response timeout plus twice the characters'
time on the wire, at ten bits a character; at low line speeds a
request carries fewer characters, so that the wait fits the core's
10 s limit. Each timeout (no answer, status 0x02, or the line timeout
0x40) doubles the default part of the wait for the answers after it,
up to the core's 10 s limit, which also caps the wire time; it goes
back to the default when the port is opened again. A device that
answers later than the default timeout therefore gets one repeat, not
one per write. Characters leave the buffer only when the device accepts
them; after a partial accept the next request starts with the first
character not accepted. Characters the device refuses (a NACK that
accepts none), answers busy (status 0x03, 5.2, Table 4) or does not
answer (no answer, status 0x02, or the line timeout 0x40, 5.5,
Table 21) stay at the head of the buffer, and the transmit work sends
them again 10 ms after each refusal, busy answer or timeout, with no
limit on the tries. A write that comes in meanwhile does not cut the
delay short. While the device refuses, the buffer fills, writers
block and `tcdrain()` waits. When the same head has been kept for
5 s, the port logs one rate-limited "transmit stalled: <reason>, <n>
characters waiting" warning (reason "refused", "busy" or "timed
out"), and none more until the device takes it; a flush ends the
stall. The driver cannot tell a device held off by flow control (CTS)
from one that refuses for its own reasons.

Any other error status, or an answer that accepts more than was sent
(a protocol error), drops the characters of that request, reported
as an event, "dropped <n> bytes: <errno>" (no line once the device
is gone), and the next characters go out. A flush discards what is
not yet sent; with a request in flight, the characters it carries
leave once it is answered, and characters written after the flush
stay. A hangup, an unbind, or the last close after `closing_wait`
discards the rest: a close that discards characters counts them as
dropped and logs "<n> characters not sent at close, discarded"; for a
request still in flight the line comes once it is answered or times
out, and counts the characters the device did not take.

Requests carry no tag (6.6). The core holds the next try after a
timeout until the late answer comes or one default timeout passes.
An answer that comes later still is taken by the next try, so the
device can get the same characters twice, and the answer of that try
is then taken by the request after it, so characters the device
refuses there can count as sent and be lost (`ASSUMPTIONS.md`).

After a transport failure the port is hung up, as on Carrier Down,
and a later open fails with `-EIO` until the interface is bound
again.

System suspend stops the tries; resume starts them again. A break is
one Generate Break event; a refused break is sent again up to 20
times, 10 ms apart, and then fails with `-EIO`.

Device writes are taken whole or not at all. The answer is a NACK
with no characters accepted when the port is closed and the write
carries characters, or when the tty buffer has no room for its
characters and the marks of its events; the device is expected to
send the write again. A refused write is not acted on at all. While
the port is closed, a write with events only is taken, and its events
are counted. On an open port, Break and overrun events become
`TTY_BREAK` and `TTY_OVERRUN` marks, and Carrier Down hangs the tty up
unless `CLOCAL` is set. `TIOCGICOUNT` counts the characters, the three
events, and in `buf_overrun` the refusals for a full buffer.

The line settings are those of the channel's configuration data
(Table 78). Data bits, stop bits or parity out of range give 8N1, with
the warning "unsupported line ..., using 8N1"; a speed of 0 gives no
tty. RTS/CTS flow control is reported as `CRTSCTS`. The driver reports
the settings in termios and cannot change them yet
(`KNOWN-ISSUES.md`).

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
  of the pass, as in the native pass. A configuration ends at a short
  slice, a GPIO table at an empty one (4.3.1.4). An error status after
  the first slice ends the configuration and keeps the channel; one on
  the first slice, or busy on any slice, leaves the channel out. Short
  slices are bounded per channel and per pass;
- I2C controller channels: Read, Write, Write Read and the no-STOP
  bit, both ways, and the LSTP statuses NACK and arbitration lost to
  their OBMF-ICP I2C codes (LSTP 1.0, 4.4.2). A configuration shorter
  than 4.4.1's is taken if it has the speed byte; the fields it lacks
  read as 0;
- UART channels as OBMF-ICP serial channels: host writes and their
  acknowledgement, device writes and the host's answers (LSTP 1.0,
  4.5.2), with the first seven configuration bytes as the line
  settings (Table 78); a shorter configuration leaves the channel out.
  A refusal goes to the device as `LSTP_NACK`, and an answer with an
  error status as `LSTP_NOT_SUPP`;
- the channel header bytes, which LSTP lacks: every channel reads as
  healthy, enabled and with a valid configuration. An I2C bus
  recovery request would be sent as the LSTP bus recovery command;
  nothing sends one, as the I2C driver offers no bus recovery.

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
transfers stop. The first request or answer that times out, in its
send or in its response, stops the device: later requests fail at
once and no more answers are sent, so a device that stops answering,
or whose bulk OUT stops completing, costs the unbind one timeout. A
device that keeps sending device requests during the unbind gets no
answers once the device is stopped.

**Unplug.** The device is gone, so the transfers stop first. Waiting
requests fail at once with `-ENODEV`, and then the function devices
are removed.

**Module unload.** `i2c-obmf.ko` and `obmf_tty.ko` use symbols of
`obmf.ko`, so they are unloaded first; each unbinds its devices.
Unloading `obmf.ko` then unbinds every interface the software way,
and waits for the device-link releases that the removed function
devices queued, so that none runs after the module is gone. An open
`/dev/i2c-N` holds `i2c-obmf.ko` (`KNOWN-ISSUES.md`); an open tty is
hung up when its device goes, and an open that races the teardown
fails with `-ENODEV`.

**Transport failure.** After ten bulk IN errors in a row the device
is stopped and every request fails with `-EIO`. The core then calls
the `failed` operation of each published function driver, once, from
a work item; the serial driver hangs its tty up. The function devices
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
| `rx_malformed` | received messages shorter than a header or than their header size, dropped; longer ones, kept without the extra bytes; device requests that cannot be decoded, answered invalid; with a dialect, messages it cannot decode |
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
| `default_answers` | default and invalid answers sent by the core |
| `rx_dropped` | device requests not delivered for want of a record (a due answer still goes out as the default answer), ended by the driver without an answer, left queued when the channel is freed, or answers with no wire form |
| `rx_replaced` | reserved, stays 0 |
| `dreq_max_depth` | the most device requests the channel held at once (at most 19: 3 reserved records and a queue of 16) |

The interface directory also has `events/`, one file per event of
"Events" above, named `transport_failed`, `in_stalled`, `out_stalled`,
`in_clear_halt_failed`, `out_clear_halt_failed`, `no_response`,
`device_error`, `bad_response`, `rx_short`, `rx_size_over_transfer`,
`rx_trailing_bytes`, `rx_unknown_channel`, `bad_device_request`,
`answer_not_sent`, `channel_health`, `device_request_queue_full` and
`tx_dropped`. Each counts the events of the interface, also those the
rate limit kept out of the log.

Each serial port has a directory `obmf_tty/<tty>/` (for example
`obmf_tty/ttyOBMF0/`) with `tx_dropped`, the characters dropped after
an error or at close, and `tx_stalls`, the "transmit stalled" lines.
It goes at unbind.

With `CONFIG_FAULT_INJECTION_DEBUG_FS`, the interface directory also
holds `fail_bulk_in/` and `fail_bulk_out/`. They follow the kernel's
fault injection attributes, and their `error` file sets the errno a
failed transfer gets (default `EPROTO`).
