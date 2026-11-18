# Receiver protocol

The original USB captures used Dell Peripheral Manager and a `413c:4503`
receiver. Both receivers use HID interface 2. Each USB product ID has its own
expected report descriptor. USB release does not restrict access.

Slot queries, pairing and removal were tested on `4503` releases `0240` and
`0244` and on `301c` release `0244`. Battery readings were tested on release
`0244` of both receivers.

## Transport and commands

Management uses 32-byte feature reports, including report ID `08`. Each exchange
sends SET_REPORT followed by GET_REPORT (`wValue=0308`, `wIndex=0002`). Requests
are padded with zeros. Values below are hexadecimal; slot numbers are 1-6.

| Request prefix | Purpose |
| --- | --- |
| `08 01` | Receiver information |
| `08 02` | Paired-device count |
| `08 03 <slot>` | Read a slot |
| `08 10 <slot>` | Read the device battery percentage |
| `08 06 <slot>` | Remove a device |
| `08 0b` | Begin discovery |
| `08 08` | Poll discovery |
| `08 04 01 <identifier[3]>` | Select a discovered device |
| `08 05` | Poll pairing completion |

Query replies echo the command. Discovery starts with an `08 00` reply on
the `4503` and `08 0b` on the `301c`, both zero-padded. Discovery and
selection acknowledgements on the `4503` use `00` or `01`. Their meaning is
unknown. The `301c` echoes the selection request, including the peripheral ID.
Removal acknowledges with `08 06` and zero padding.

## Slot records

Offsets include the report ID and start at zero.

| Offset | Field |
| --- | --- |
| 0-1 | `08 03` |
| 2 | Slot number |
| 3 | Kind: `01` keyboard, `02` mouse |
| 4 | Opaque per-device value; observed `04` |
| 5-7 | Three-byte peripheral identifier |
| 8-27 | Zero-padded ASCII model name |
| 28-29 | Unassigned bytes |
| 30 | Capability flags; flag `0x10` advertises battery reporting |
| 31 | Unassigned byte |

On the `4503`, an empty record echoes the slot, has `ff` at offsets 3-29, and
zeros at 30-31. When the receiver is empty, offsets 2-31 are all zero.
Count replies store the count at offset 2. Receiver information stores four
unassigned bytes at 2-5 and the receiver name at 6-29.

An empty `301c` slot echoes its slot number. After removal, bytes 8-29 can
retain old model data while bytes 3-7 and 30-31 are zero.

## Battery reports

A battery request is `08 10 <slot>` followed by zero padding. Its response
echoes the prefix and slot, with the battery value at offset 3. Dell Peripheral
Manager treats values from 0 to 100 as percentages and maps every other byte to
its `-1` sentinel. Its battery getter does not inspect subsequent response
bytes.

## Pairing

Discovery arrives on interrupt endpoint `83` as input report `01`, subtype `01`.
The device kind is at offset 2, an opaque per-device value is at 3, the ASCII
model is at 4-13, and its three-byte identifier is at 14-16. The opaque value
has been observed as `01`, `04`, and `07`; it is not interpreted or validated.
Remaining bytes are zero. The identifier is read from the device and copied
into the selection request.

Completion replies use `08 05 ff` while pending. An `08 05 01` reply contains the
slot at 3, kind at 4, the same opaque per-device value at 5, identifier at 6-8,
and model at 9-28. The last three bytes are unassigned. Polling runs roughly
once per second. In the `301c` MS5320W capture, endpoint `83` returned
`01 08 01 30` with zero padding before status showed completion. This report
triggers an early status poll.

Pairing and removal require confirmation and a fresh slot check. After a single
change request, the program verifies the expected change and remaining devices.
It never retries a change automatically. No receiver cancellation command is
implemented; once selected, a device can finish pairing independently.
