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
