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
using the existing QcomUserLog transport. The app uses its own container and
NSMotionUsageDescription. GPU051's original729 jobs and runtime files are kept.

Host production integration tests passed; target compile/PAC, strict prelink,
package qualification and physical six-face/three-axis/near-far are separate gates.
See `docs/sensors/GPU051-INTEGRATION.zh-CN.md` for current source and evidence.
