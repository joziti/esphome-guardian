# Guardian sensor-board UART protocol

This document describes the serial protocol between a Bluelab Guardian's Wi-Fi
module (an ESP32-S3) and its sensor/display board (a PIC18 microcontroller). The
ESPHome component in this repository implements it. It contains enough to write
another implementation.

**Basis and limits.** The protocol was worked out for interoperability, from
the behaviour of the stock Wi-Fi module and from captures of the link while
driving a real unit. The sensor board's own firmware was never available. So
everything said about how the *board* behaves is **observed, not specified**: it
describes three units of one model, the Guardian Monitor Wi-Fi; the one whose
link was captured in detail reports its sensor-board application as
`DeviceVersion` `01 00 00 07 14 00` (§8). Other models or firmware versions may
differ. Decode leniently (§8), and treat the "Open questions" in §15 as open.

**Contents:** 1 Physical layer · 2 Framing · 3 Length varint · 4 Checksum ·
5 Payload · 6 Float24 · 7 Request pattern · 8 Board → host messages ·
9 Host → board messages · 10 Enumerations · 11 Worked examples ·
12 Driving the link · 13 Board behaviour · 14 Debugging the link ·
15 Open questions

---

## 1. Physical layer

| Parameter | Value |
|---|---|
| Baud rate | **57600** |
| Format | 8 data bits, no parity, 1 stop bit (8N1) |
| Flow control | none |
| Logic level | 3.3 V; both lines idle high (about 3.2 V) |
| Stock module wiring | module GPIO4 = TX → sensor board RX; module GPIO5 = RX ← sensor board TX |

The baud rate is fixed; it never changes at runtime.

**Drive strength.** The sensor board's RX line has a strong pull-up (about
0.7 kΩ, estimated from measurement). A normal push-pull output (an ESP32 pin, a
Raspberry Pi UART, an FT232R) pulls it to a valid low. Adapters with series
resistance may not: a Bus Pirate 5 (330 Ω in series on every IO pin) only reached
about 1.0 V, which the board's Schmitt-trigger input ignores. If the board
receives nothing, measure the "low" level before suspecting software.

**Reading.** Frames can arrive split across reads, and several frames can
arrive in one read. Buffer across reads and rescan for the sync byte.

---

## 2. Framing

Every frame, in both directions, has the same layout:

```
+-------------+--------------------+-----------------+----------+
| SYNC = 0xAA | LENGTH (varint)    | PAYLOAD         | XOR      |
| 1 byte      | 1, 3 or 5 bytes    | LENGTH bytes    | 1 byte   |
+-------------+--------------------+-----------------+----------+
```

There is no address, no sequence number and no escaping: a payload byte may
equal `0xAA`.

Receiving:

1. Scan for `0xAA`.
2. Read the length varint (§3).
3. If fewer than `LENGTH + 1` bytes follow, wait for more data.
4. XOR every byte from the sync byte through the checksum byte. The result must
   be `0x00` (§4). If it isn't, the sync was false: advance one byte and rescan.

> **Framing is not self-synchronising.** Because `0xAA` is not escaped and the
> length has no check of its own, a stray `0xAA` followed by a large "length"
> makes a naive parser wait for hundreds of bytes before the checksum rejects
> it. Meanwhile valid frames sit in the buffer behind it. A robust receiver
> should, while a candidate frame is incomplete, look further ahead for a
> complete frame with a valid checksum, and resync there if it finds one. The
> ESPHome component does this.

---

## 3. The length varint

Unsigned, little-endian, in the `bincode` "varint" encoding. The same encoding
is used for the message count inside the payload.

| Value *n* | Encoding | Bytes |
|---|---|---|
| 0 – 250 | one byte, `n` | 1 |
| 251 – 65 535 | `0xFB` + u16 LE | 3 |
| 65 536 – 2³²−1 | `0xFC` + u32 LE | 5 |

In practice every frame is shorter than 251 bytes, so the length and the count
are each a single byte.

---

## 4. Checksum

An 8-bit XOR over the whole frame, sync byte included, with the checksum byte
itself taken as zero:

```
cs = 0xAA ^ (each byte of the length varint) ^ (each payload byte)
```

Equivalently, **the XOR of every byte of a valid frame is `0x00`.** An 8-bit
XOR catches every single-byte error but only about half of random multi-byte
corruption.

---

## 5. Payload: a list of messages

The payload is a count followed by that many messages:

```
+----------------+-------------+-------------+-----+
| COUNT (varint) | MESSAGE 0   | MESSAGE 1   | ... |
+----------------+-------------+-------------+-----+
                   id + body     id + body
```

Each message is a **one-byte id** followed by a body whose length and layout
depend only on the id. Messages carry no length prefix, so **to find message
*n*+1 you must know the body size of message *n***. §8 and §9 give the sizes.
All multi-byte integers are **little-endian**. Enum fields are one byte.

Host → board and board → host use **separate id tables** (§8, §9). An id that
means "request X" outbound generally means "acknowledgement of X" inbound.

---

## 6. Float24 — the value format

Readings and thresholds use a 3-byte decimal number:

```
+---------------------------+----------------------+
| MANTISSA  i16 little-end. | DECIMALS  signed i8  |
| bytes 0-1                 | byte 2               |
+---------------------------+----------------------+

value = mantissa / 10^decimals
```

- **Both fields are signed.** Sign-extend byte 2: `0xFF` is −1, i.e.
  `mantissa × 10`.
- The board sends 2 decimals for EC, temperature and pH readings, and 1
  decimal for EC/pH alarm thresholds. You may send any `decimals` value: the
  board recovers the value as `mantissa / 10^decimals` either way. To keep
  precision, use the most decimals that still fit the i16 mantissa.
- An encoder should **saturate** values beyond ±32767 / 10^decimals rather than
  wrap them.

| Bytes | Mantissa | Decimals | Value |
|---|---|---|---|
| `27 00 02` | 39 | 2 | 0.39 (EC, mS/cm) |
| `2E 02 02` | 558 | 2 | 5.58 (pH) |
| `A9 08 02` | 2217 | 2 | 22.17 (temperature, °C) |
| `12 00 01` | 18 | 1 | 1.8 (EC alarm low) |
| `84 EA 03` | −5500 | 3 | −5.5 |

---

## 7. Request pattern

Requests and data are paired by id:

- The host sends a request with id `X`.
- The board acknowledges with the **same id** `X`, carrying a 2-byte
  `RequestResult { context, result }`. Every acknowledgement seen carried
  `00 00`.
- The data comes as separate messages with ids just **above** `X`.

| Request (host → board) | Ack id | Data ids (board → host) | Data |
|---|---|---|---|
| `0x00` VersionRequest | — | `0x00` | version |
| `0x02` StateRequest | `0x02` | `0x03`, `0x04`, `0x05`, `0x07` (`0x06` and `0x08` are defined but not sent by the Monitor) | device mode, status words |
| `0x10` MeasurementRequest | `0x10` | `0x11` × 4 | sensor readings |
| `0x18` CalibrationRequest | `0x18` | `0x19` × 3 (`0x1A` is defined but not sent by the Monitor) | calibration records |
| `0x20` GetSettingsRequest | `0x20` | `0x22`, `0x23`, `0x2B` × 3 on the Monitor (`0x21`–`0x32` are defined) | settings, incl. alarm ranges |
| `0x41`–`0x5F` Set… | same id | — | acknowledgement only |

**A request selects a data set, and the board then repeats it.** The board sends
a frame about once a second whether asked or not (§13). A request switches
*which* data set those frames carry, and the board keeps sending that set until
another request arrives. A reply therefore does not prove that a request was
outstanding, and an acknowledgement can be a repeat of an old one.

---

## 8. Board → host messages

"Body" excludes the id byte. Names in *italics* are descriptive: the layouts
are known, but no official names exist for them.

| Id | Message | Body | Fields |
|---|---|---|---|
| `0x00` | DeviceVersion | 6 | u8 app_id · u8 kind (§10) · u8 board_revision · u8 · u16 version |
| `0x01` | *VersionAck* | 0 | — |
| `0x02` | RequestResult | 2 | u8 context · u8 result — ack for StateRequest |
| `0x03` | DeviceMode | 1 | u8 mode (§10) |
| `0x04` | *Low alarm state* | 2 | u16: one bit per sensor whose reading is below its low alarm threshold (bit 0 EC, 1 temperature, 2 pH); §12 |
| `0x05` | *High alarm state* | 2 | u16: one bit per sensor whose reading is above its high alarm threshold; §12 |
| `0x06` | *Lockout state* | 3 | u8 sensor · u16 set of lockout conditions (6 bits); controller models only |
| `0x07` | *Calibration due* | 2 | u16: one bit per pH buffer due (bit 0 pH 4, 1 pH 7, 2 pH 10); §12 |
| `0x08` | *Control event* | 2 | u8 sensor · u8; controller models only |
| `0x10` | RequestResult | 2 | ack for MeasurementRequest |
| `0x11` | **Measurement** | **5** | u8 sensor (§10) · Float24 value · u8 status (§10; 0 = healthy) |
| `0x18` | RequestResult | 2 | ack for CalibrationRequest |
| `0x19` | *Calibration record* | **10** | u8 buffer (0 = pH 4, 1 = pH 7, 2 = pH 10) · u16 write counter · u8 valid · Float24 electrode volts · Float24 °C; §12 |
| `0x1A` | *Calibration state* | 2 | u8 · u8; not sent by the Monitor and ignored by the stock module |
| `0x20` | RequestResult | 2 | ack for GetSettingsRequest |
| `0x21` | *Setting: device mode* | 1 | u8 mode |
| `0x22` | *Setting: u8* | 1 | u8; the stock module decodes it and discards it, meaning unknown |
| `0x23` | *Setting: "alarms set"* | 1 | u8 bool |
| `0x24` | *Setting: lockout* | 2 | u8 type · u8 enabled |
| `0x25` | *Setting: control mode* | 2 | u8 sensor · u8 mode (controllers) |
| `0x26` | *Setting: required value* | 4 | u8 sensor · Float24 set-point (controllers) |
| `0x27` | *Setting: required range* | 7 | u8 sensor · Float24 low · Float24 high (controllers) |
| `0x29` | *Setting: on-time* | 3 | u8 type · u16 |
| `0x2A` | *Setting: off-time* | 3 | u8 type · u16 |
| `0x2B` | *Setting: **alarm range*** | **7** | u8 sensor · Float24 low · Float24 high |
| `0x30` | *Pod id and version* | 5 | u8 × 5 (controllers) |
| `0x31` | *Pump setting* | variable | u8 · u8 · varint-prefixed bytes (controllers) |
| `0x32` | *Pump speed* | 1 | u8 (controllers) |
| `0x41`–`0x5F` | RequestResult | 2 | ack for the Set… request with the same id |
| `0x80`–`0x8F` | RequestResult | 2 | ack for a system request with the same id |
| `0xFF` | *Unsupported request* | 1 | the board's reply to a request id it doesn't implement: `AA 03 01 FF 01 56` |

Ids `0x24`–`0x27`, `0x29`, `0x2A`, `0x30`, `0x31` and `0x32` belong to the
controller models; a Guardian Monitor (no pumps or dosing) does not send them,
so their layouts are unconfirmed. Id `0x14`, with a 1-byte body, also exists;
its meaning is unknown.

> **Get the `0x11` size right.** The Measurement body is **5** bytes: the
> trailing status byte is easy to miss. With a 4-byte body the walk stays in
> step *by accident*: the status byte `0x00` is read as a `DeviceVersion` id,
> and the values come out silently wrong. A decoder should check that the walk
> lands exactly on the payload end, and sanity-check decoded values.

> **Decode leniently.** Different units may run different firmware generations.
> On an unknown id, stop walking that payload (you can't know its length), but
> keep the frame's earlier messages and keep the receive buffer.

---

## 9. Host → board messages

The ones relevant to reading and setting the Guardian's values:

| Id | Message | Body | Notes |
|---|---|---|---|
| `0x00` | VersionRequest | 0 or 1 | the board answers with `0x00` DeviceVersion |
| `0x01` | VersionResponse | 5 | u8 app_id · u8 board_rev · u8 major · u8 minor · u8 patch: the host's own version (stock handshake, §12) |
| `0x02` | StateRequest | 0 | → `0x03`–`0x08` |
| `0x03` | Mode | 1 | the host's mode; **7 = "mediating"**, sent in every poll (§12) |
| `0x04` | Status | 1 | host status, informational; send `0` |
| `0x10` | MeasurementRequest | 0 | → `0x11` × 4 |
| `0x18` | CalibrationRequest | 0 | → `0x19` × 3 |
| `0x20` | GetSettingsRequest | 0 | → settings including `0x23` and `0x2B` × 3 |
| `0x4B` | **SetAlarmRangeRequest** | **7** | u8 sensor · Float24 low · Float24 high (same layout as `0x2B`) |
| `0x83` | SecurityCodeRequest | 2 | u16 LE: part of the stock handshake only; **not needed** (§12) |

Other `Set…` requests exist in `0x41`–`0x5F`, for example brightness (`0x42`),
alarms (`0x43`), lockout (`0x44`), control mode (`0x45`), set-point (`0x46`),
required range (`0x47`), on/off times (`0x49`, `0x4A`) and pumps (`0x51`,
`0x52`). They mostly apply to the controller models and were **not tested**.
System requests in `0x80`–`0x8F` (restart, factory reset and the like) are out of
scope and not documented here.

---

## 10. Enumerations

**Sensor** (the first byte of `0x11`, `0x19`, `0x2B`, `0x4B`):

| Value | Sensor | Unit |
|---|---|---|
| 0 | EC (conductivity) | mS/cm |
| 1 | Temperature | °C |
| 2 | pH | — |
| 3 | *Other*: present in measurement frames (4 decimals), no front-panel channel; ignore | — |

The alarm ranges confirm the order independently. EC 1.8–2.4, temperature 0–50
and pH 5.9–6.5 make physical sense only in this order. Values are always
**native units** (mS/cm, °C), whatever unit the Guardian's display is set to: a
95 °F alarm set on the front panel reads back as 35.

**DeviceMode** (`0x03`, `0x21`): 0 = monitor, 1 = control, 2 = settings,
3 = calibration, 4 = update. The host-side `Mode` value 7 ("mediating") is
outside this range (§12).

**Version kind** (`0x00` byte 1): 3 values. The sensor board's application
reports 0. The other two denote a bootloader and the Wi-Fi module ("mediator");
their ordinals are unconfirmed.

**RequestResult `result`**: 4 values; every acknowledgement seen carried 0
alongside valid data, so 0 is almost certainly success. The other ordinals are
unknown (§15).

**Measurement status** (`0x11` byte 5): the state of that sensor's reading.

| Value | Status |
|---|---|
| 0 | healthy |
| 1 | under range |
| 2 | over range |
| 3 | disconnected |
| 4 | sensor fault |
| 5 | dependent-sensor fault (the sensor it depends on, e.g. temperature for EC, is faulty) |

The order is taken from the stock Wi-Fi module's firmware, which maps the
byte through a table onto its named fault states. A healthy reading is the
only value seen on the wire so far; the fault values have not yet been observed
on a live unit.

---

## 11. Worked examples

### A measurement request

```
AA 02 01 10 B9
│  │  │  │  └─ XOR checksum
│  │  │  └──── id 0x10  MeasurementRequest
│  │  └─────── message count = 1
│  └────────── payload length = 2
└───────────── sync
```

### The poll the ESPHome component sends (§12)

```
AA 06 03 03 07 04 00 10 BF
      │  └──┬┘ └──┬┘ └─ MeasurementRequest
      │     │     └──── Status(0)
      │     └────────── Mode(7 = mediating)
      └──────────────── 3 messages
```

### Four readings in one frame (real capture)

```
AA 1C 05 11 00 28 00 02 00 11 01 6B 08 02 00
      11 02 2F 02 02 00 11 03 34 03 04 00 10 00 00 F4

length 0x1C (28), 5 messages:
  11 00 28 00 02 00   Measurement  EC           40 / 10^2 =  0.40 mS/cm   status 0
  11 01 6B 08 02 00   Measurement  Temperature 2155 / 10^2 = 21.55 °C     status 0
  11 02 2F 02 02 00   Measurement  pH           559 / 10^2 =  5.59         status 0
  11 03 34 03 04 00   Measurement  Other        820 / 10^4 =  0.0820       status 0
  10 00 00            RequestResult (ack for 0x10), context 0, result 0
```

The XOR of all 31 bytes is `0x00`.

### Alarm ranges (from a GetSettings reply)

```
2B 00 12 00 01 18 00 01   EC           1.8 .. 2.4 mS/cm
2B 01 00 00 00 32 00 00   Temperature  0   .. 50  °C
2B 02 3B 00 01 41 00 01   pH           5.9 .. 6.5
```

### Setting the EC alarm to 1.8 .. 2.5 and reading it back in one frame

```
AA 0E 04 03 07 04 00 4B 00 50 46 04 A8 61 04 20 14
         Mode(7) Status(0)
                       4B 00 50 46 04 A8 61 04    SetAlarmRange EC, 18000/10^4 .. 25000/10^4
                                               20 GetSettingsRequest
```

The board applies the write and replies, on its next frame, with a settings dump
whose `0x2B 00 …` carries the new range.

---

## 12. Driving the link

How a host should talk to the board. This is the discipline the ESPHome
component follows, and each point was verified on hardware.

**1. No handshake is needed.** The stock Wi-Fi module opens with a "mediation"
handshake, sent once when it sees the board's `DeviceVersion`:

```
[SecurityCodeRequest(<code>), GetSettingsRequest, VersionResponse(1.0.18), StateRequest]
= AA 0C 04 83 <code u16 LE> 20 01 01 00 01 00 12 02 <xor>
```

`<code>` is a per-unit security code. A host does **not** need it: plain
`MeasurementRequest`, `GetSettingsRequest` and `SetAlarmRangeRequest` (with
read-back) all work without any handshake, and a handshake with a wrong code has
no effect. What mediation changes on the board is unknown, apart from the next
point.

**2. Send `Mode(7)` and `Status(0)` with every request.** A board that *has*
been mediated (for example, by the stock module before you took over) ignores
requests that lack `Mode(7)`. Sending it on an un-mediated board is harmless. So
every poll is:

```
[Mode(7), Status(0), <request>]
```

**3. Reply 3–100 ms after each board frame. This timing is critical.** The
board sends a frame about once a second, and it only honours a request that
arrives within a window after its own frame:

| Request sent after the board's frame | Result |
|---|---|
| 0–2 ms | `GetSettingsRequest` ignored |
| **3–100 ms** | **the request is honoured: the data set switches on the next frame** |
| ≥ 150 ms | `GetSettingsRequest` ignored; `MeasurementRequest` ignored in some phases |

The board's period is very close to 1.000 s. So a host that polls on **its own**
1-second timer locks onto one point in the board's cycle. If that point is
outside the window, requests are ignored for minutes, until clock drift moves
the phase. **Send each request a fixed delay (about 20 ms)
after a board frame completes.** Use a randomised timer only when the board is
silent. The stock module also replies within milliseconds of each frame. The
window's cause is unknown.

**4. One frame of latency.** With correct timing, the reply to a request arrives
on the board's next frame, about 1 s later.

**5. Confirm writes by reading them back, never by the acknowledgement.**
Because the board repeats its last data set (§7), an acknowledgement may be a
stale copy. Send the write and a `GetSettingsRequest` in **one** frame,
`[Mode(7), Status(0), SetAlarmRange, GetSettings]`, and treat the write as done
when the next `0x2B` for that sensor carries the new values. A
`SetAlarmRangeRequest` always carries **both** bounds. To change one, resend the
other unchanged.

**6. Rotate what you ask for.** One request per frame. The ESPHome component
asks for measurements most of the time, for settings and for state every 5 s,
and for the calibration records (`0x18`) every 60 s; the state and the records
are also re-read whenever the device mode changes. Each refresh is timed from
the request, not from the answer, so a request the board ignores cannot block
the measurements. The state stream carries the device mode and the status
words. Every settings, state or calibration request costs one measurement
frame.

**Where each value comes from:**

| Value | Request | Carried in |
|---|---|---|
| EC / pH / temperature reading | `0x10` | `0x11` |
| Reading status, per sensor | `0x10` | `0x11` status byte (§10) |
| Alarm range, per sensor | `0x20` | `0x2B` |
| "Alarms set" | `0x20` | `0x23` |
| pH calibration due | `0x02` (or on mode change) | `0x07` status word: any of the low bits set |
| pH 4 / 7 / 10 calibration state and voltage | `0x18` (every 60 s, or on mode change) and `0x02` | `0x19` record (valid flag, voltage) and the buffer's bit in `0x07` |
| Device mode | `0x02` (or unsolicited) | `0x03` |
| Alarm status (low / ok / high) | `0x02` | the sensor's bit in the `0x04` (low) and `0x05` (high) status words |
| *Board status words* (diagnostic) | `0x02`, `0x10` | `0x04`, `0x05`, `0x07` (and `0x06`, `0x08` if ever sent), plus the status byte of each `0x11` |
| *Board calibration records* (diagnostic) | `0x18` | `0x19` × 3 (and `0x1A` if ever sent) |

**The status words and the calibration records.** Each status word is a u16
of which the low 4 bits are used.

- `0x04`, *low alarm state*: one bit per sensor whose reading is below its low
  alarm threshold, in sensor order (bit 0 EC, bit 1 temperature, bit 2 pH).
- `0x05`, *high alarm state*: one bit per sensor whose reading is above its
  high alarm threshold, same order. These two words are what the stock module
  reports as its low and high alarm states, and the ESPHome alarm status
  entities are taken straight from them. The bits follow the reading within a
  second or two of it crossing a threshold. The board compares its **displayed**
  reading, rounded to one decimal, and a reading that rounds to the threshold
  counts as in alarm: EC 1.98 against a high alarm of 2.0 sets the high bit,
  although the `0x11` reading carries 1.98.
- `0x06`, *lockout state* (u8 sensor, u16 set of six lockout conditions) and
  `0x08`, *control event* (u8 sensor, u8) belong to the controller models; a
  Guardian Monitor does not send them.
- `0x07`, *calibration due*: one bit per pH buffer whose calibration is due,
  in the order pH 4 (bit 0), pH 7 (bit 1), pH 10 (bit 2); a set bit is a
  flashing lamp on the panel. A fresh unit reports `0x0003`. A successful
  pH 7 calibration clears the whole word.
- `0x19`, *calibration record*: three records, one per pH buffer, with the
  buffer in the first byte (0 = pH 4, 1 = pH 7, 2 = pH 10). Then a u16 **write
  counter**, incremented every time the record is written, including when it
  is invalidated; a u8 **valid** flag (1 = the buffer holds a real
  calibration, shown as a steady lamp); the **electrode voltage** at
  calibration as a Float24 in volts (about +0.16 for pH 4, 0 for pH 7 and
  -0.16 for pH 10 with a healthy probe); and the **temperature** at
  calibration as a Float24 in °C. A never-calibrated buffer holds a factory
  default with valid = 0.
- `0x1A` is defined but not sent by the Guardian Monitor, and the stock module
  ignores it.

**Calibration behaviour.** A pH 7 calibration invalidates the pH 4 and pH 10
records (valid → 0, counters + 1), so pH 7 is done first and then pH 4 and/or
pH 10; either slope buffer alone satisfies the Guardian. A failed calibration
(display `ERR`) wipes all three records to zero. The panel lamps follow the
data: flashing = the buffer's `0x07` bit is set; steady = a valid record (the
pH 4/10 lamp is steady if either record is valid); off = neither. The valid
flag is not a calibration-due indicator: it stays at 1 until the record is
rewritten, while the `0x07` reminder can come back on. Bluelab's user manual
describes the same lamps: both off = factory defaults; pH 7 steady with 4/10
off = pH 7 calibrated, factory default slope; both steady = full calibration;
flashing = 30 days since the last full calibration, calibration due. It also
requires pH 7 first and pH 4 or pH 10 within one hour of it, which is what the
invalidation enforces, and shows `ERR` for a slope calibration outside that
hour or with a bad solution.

The ESPHome component's *pH calibration due* is `0x07` with any bit set, so it
reports a flashing lamp; the per-buffer *pH 4 / 7 / 10 calibration* entities
combine the bit and the record into `due`, `calibrated` or `none`.

---

## 13. Board behaviour

Observed behaviour of the tested units.

- **It talks without being asked.** After power-up the board sends
  `DeviceVersion` about once a second: `AA 08 01 00 01 00 00 07 14 00 B1` on
  the tested unit. After a request it repeats that request's data set at the
  same rate, indefinitely (§7).
- **Only a power cycle restarts it.** A reset that isn't a power cycle leaves
  the board repeating whatever it was last asked for. Only removing power brings
  back the boot `DeviceVersion` beacon.
- **No EC probe, no link.** With the EC probe (which also measures temperature)
  unplugged, the board transmits **nothing** and answers nothing, at any baud,
  although the display keeps running (with dashes for temperature). Plugging the
  probe back in restores the link at once on a running system. Treat silence as
  "probe absent" as well as "link fault". An unplugged **pH** probe is different:
  the link stays up, the pH reading floats (and is shown on the display), and
  the reading's status byte stays 0.
- **Settings mode pauses the link.** While someone is changing settings with
  the Guardian's buttons, the board is in settings mode (DeviceMode 2) and
  answers no requests. It sends only `AA 03 01 03 02 A9` (DeviceMode = settings)
  at irregular 1.5–12 s intervals, then `AA 03 01 03 00 AB` (monitor) when it
  leaves settings mode. Normal data resumes after that. Alarm changes made in
  settings mode appear in the next `GetSettings` reply. Calibration (mode 3,
  below) pauses the link the same way. Treat modes 2–4 as "busy", not
  "link lost".
- **Unsupported requests get a fixed reply.** Any request id the board doesn't
  implement is answered with `AA 03 01 FF 01 56` (id `0xFF`), which it then
  repeats like any other reply until a valid request arrives.
- **"Alarms set" flickers after alarm edits.** `0x23` read 0 right after each
  alarm change in settings mode and 1 again some time later.
- **Calibration from the front panel.** Each calibration takes 15–40 s in
  mode 3 (calibration), after which the board sends mode 0 and answers
  requests again; the new `0x19` record is readable at once (§12). The `0x04`
  and `0x05` words follow the reading.

---

## 14. Debugging the link

**Log the bytes.** In `guardian.yaml`, uncomment the `debug:` block under
`uart:` and update over Wi-Fi (see ESPHome's
[UART debugging](https://esphome.io/components/uart/#debugging)). Every frame
is then logged in hex, `>>>` sent and `<<<` received. Check:

1. **The frames.** A measurement poll is `AA 06 03 03 07 04 00 10 BF`; the
   board answers `AA 1C 05 11 …` (four readings and an acknowledgement). In
   settings mode it sends only `AA 03 01 03 02 A9` (§13).
2. **The timing.** Each `>>>` should follow the preceding `<<<` by
   **3–100 ms**; the board ignores requests outside that window (§12).
3. **Silence.** No `<<<` at all means the board isn't transmitting: check the
   EC probe lead and the wiring (§13).

Comment the block out again afterwards; it logs about two lines a second.

**Listen with a USB-serial adapter.** To see the traffic independently of the
ESP32, connect **only the RX pin** of a 3.3 V USB-serial adapter (FT232R,
CP2102, …), plus GND, to one of the lines, and open a terminal at 57600 baud:

```bash
picocom -b 57600 /dev/ttyUSB0      # exit: Ctrl-A Ctrl-X
```

- On the sensor board's TX line you see what the board sends; on the
  Feather's TX line, what the Feather sends. The output is binary;
  `stty -F /dev/ttyUSB0 57600 raw` then `cat /dev/ttyUSB0 | xxd` shows it as
  hex.
- **Never connect the adapter's TX while the Feather is connected**: only one
  device may drive the sensor board's RX.
- **To test the sensor board on its own**, disconnect the Feather and connect
  the adapter's TX to the board's RX, its RX to the board's TX, and GND. A
  powered board with the EC probe lead plugged in sends a frame about once a
  second: after a power cycle `AA 08 01 00 01 00 00 07 14 00 B1`
  (DeviceVersion), otherwise whatever it was last asked for. Sending it
  requests by hand rarely works, because a request only takes effect 3–100 ms
  after one of the board's frames. If the board answers here but not with the
  Feather, the problem is on the Feather side (pins, wiring, power). If the
  adapter can't pull the board's RX line low, see "Drive strength" in §1.

---

## 15. Open questions

- `RequestResult` `result` values other than 0, and the `0xFF` reply's body byte.
  The stock module knows three error results, *unsupported setting*,
  *unsupported type* and *invalid argument*, but which byte is which is not
  established.
- Confirmation of the measurement status fault values (§10) on a live unit.
  Unplugging the pH probe does not produce one.
- The meaning of the `0x22` setting byte: the stock module decodes it as a
  plain u8 and then discards it.
- Which `0x07` bits the 30-day reminder sets, in particular whether the pH 10
  bit is ever set.
- What a controller sends in `0x06` (lockout state) and `0x08` (control event),
  and the meaning of `0x1A`, which the Monitor does not send and the stock
  module ignores.
- Id `0x14`.
- `0x31` pod chain layout, and all the controller-model settings (`0x24`–`0x32`)
  as sent by a real controller.
- Why `0x23` drops to 0 after an alarm change in settings mode.
- Whether other Guardian models and sensor-board firmware versions behave the
  same.
