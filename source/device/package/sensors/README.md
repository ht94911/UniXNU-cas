# nezha iOS27 sensors on GPU051

Selected integration candidate over GPU051 device76c5952 and original Metal051.
`ns=1` starts the existing ALS017 ADSP worker and enables these sensor clients.
The normal archive plan builds the owner, input, ALS and sensors before GPU.

The board owns one unadvertised QRTR client, the IOHIDEventService/FastPath
frontend and bounded commands on that existing worker. `components/QcomSscSensors`
is a fixed source export of the separate reusable wire/client/math/fusion component.
TCS3760 physical ALS/proximity and LSM6DSV buses remain with sensorPD firmware.

Sleep retires nonwakeup demands and keeps the real wakeup proximity subscription.
A normal USB shutdown requests per-SUID disable once and lets the same worker
receive actual ACKs. No sensor code starts PAS, GLINK, a second IRQ reader or bus.

`userland/` builds an ordinary CoreMotion/UIKit test app, a launcher adapted
from Camera071's measured LaunchServices interface, and a root-only exporter
using the existing QcomUserLog transport. Sensor003 omits the exporter job after
the actual container-directory policy denial; the ordinary app's own log is
read by original MCM metadata after Android/root postflight. The app uses its own container and
NSMotionUsageDescription. GPU051's original729 jobs and runtime files are kept.

Host production integration, target compile/PAC, strict prelink, package, loader
and Windows113 entry checks passed. Physical six-face/three-axis/near-far and
ordinary CoreMotion/UIDevice consumption still need this candidate's phone trial.

`scripts/analyze-nezha27-sensors.py --run <run> --userlog-decoder
<selected-QcomXnuUsbDebug>/scripts/decode-user-logs.py` verifies the selected USB
frames and separates invalid app callbacks from advancing samples. Sparse SSC
console values, completed board callbacks and normal-app observations are
reported independently. `tests/sensors-analysis.py` covers measurement validity.
See `docs/sensors/GPU051-INTEGRATION.zh-CN.md` for current source and evidence.

The Windows113 evidence wrapper calls `scripts/run-nezha27-sensors-auto.py` with
the fixed qualified entry. It completes empty/short captures and takes the final
retained console after verified normal return, collector exit and release.
`tests/sensors-auto.py` checks the two authorized automatic closure paths.

`scripts/replay-nezha27-sensors.py --run <run>` preserves the first rejected
normal packet and invokes production report/event decoders under ASan/UBSan.
This offline replay generates no sensor samples and performs no phone actions.
