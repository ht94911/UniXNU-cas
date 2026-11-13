# Nezha27 real ALS front end

This is a separate original-backed `IOHIDEventService` for the board's actual
TCS3760 sensor. It uses Sensor Ambient Light usage `0x20/0x41` and original
type12 integer-lux events. The touchscreen descriptor and reports are independent.

The unique backend owner prepares the boot-lifetime service with a real provider,
control callback and epoch. Preparation suppresses `registerService`. Backend
readiness alone produces no event or capability; the first valid, sequenced real
sample is cached before service publication. Lux0 is valid darkness when it is
actually measured. Epoch, sequence, validity and reception-time checks reject
stale or invalid inputs. `copyEvent(type12)` returns the latest valid real sample.

The backend supplies calibrated **integer lux** and actual Darwin absolute
reception ticks. It retains SSC ticks separately and documents fractional-lux
conversion. The front end does not infer clock epochs, ADC gain, color or
photometric calibration. `supports-float-lux` and True Tone are not enabled here.

`ReportInterval` is microseconds. Interval control is submitted to the prepared
owner even while sample delivery is unready, allowing a paused subscription to
restart. Accepted interval0 invalidates old copies and pending delivery. Accepted
positive intervals do not restore an old sample; new ready/valid data is required.
The driver stores an accepted requested interval before publishing the registry
property. If publication fails, it returns `kIOReturnNoMemory`, retains that
accepted interval and pause state, and does not claim a backend rollback.

The exact24A446 fixture records a200-byte base,219 authenticated virtual entries,
original constructor/destructor and the integer ambient-event constructor. The
dedicated native recipe reuses the existing input header and binding helpers;
normal native/PAC/class verification is still required. Host qualification
compiles this actual class against a kernel-interface model under ASan/UBSan.
These models do not qualify sensor transport or automatic brightness on hardware.

ALS011 on the board read the retained registry/config files, discovered the
default ambient-light SUID and received the enable ACK. Its next indication
failed wire decoding before any valid lux. The QRTR adapter now records that
single terminal frame in offset-labelled hex rows before its borrowed buffer
is reused. This preserves the failure for production-codec replay; sensor
publication and the existing failure behavior still require real valid data.

ALS012 captured the rejected69-byte indication and identified1022 as the
standard factory-calibration event. The codec validates its packed/unpacked
coefficient fields, required accuracy and optional calibration ID as metadata.
It continues waiting for1025 sensor data; calibration coefficients neither
publish lux nor rescale the physical sensor's already calibrated samples.

ALS013 produced four real39/30/37/26 lux samples in the closed local trial.
Original CoreBrightness added the service and read26 lux, but the user's later
cover/uncover intervention produced no new sample. The correct Display & Text
Size page had no Auto-Brightness switch. Automatic brightness is not accepted.

ALS014 adds static atomic progress observations, read through provider8 without
entering the HID gate or state lock. It records the existing owner loop and
shared/mapper/GLINK/frontend calls, gate entry/return, timer delivery stages,
copyEvent call/return, actual interval acceptance and cached sample flags. These
fields do not add class members, virtual slots, hardware reads or retries.
The existing100-word transport snapshot stays seqlocked; the live progress
fields are independent observations, not an atomic view of all driver state.

Provider8 `run` arrays use these orders:

- `worker`: loop count, worker point, transport point (the enums in the header).
- `gate`: calls, returns, point (1 waiting,2 action,3 action returned,4 gate returned).
- `delivery`: point, completed dispatch count, actual delivered sample sequence.
  Points3/4 surround registration,5/6 dispatch and7 release;8 is completed,
  9 has no current pending sample and10 is event allocation failure.
- `copy`: calls, returns.
- `interval`: calls, returns, last request, accepted interval in microseconds,
  actual returned IOReturn. An in-flight call has no new completed result yet.
- `sample`: latest sequence, integer lux, flags (1 published,2 ready,4 paused,
  8 stopped,16 pending,32 valid). Lux without the valid bit is not a new sample.
