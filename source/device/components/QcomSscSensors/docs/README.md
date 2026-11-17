# SSC source responsibilities

QcomSscWire parses the normal service400 IDL1.7 QMI/protobuf wire format.
It copies scalar data and leaves report event views borrowed only for receive.
Attribute128 packets accumulate name/type/API/rates/available/stream metadata
before sensor identity is accepted; a normal-report split is not an empty device.

QcomSscClient owns a single local client ID, serialized requests, dynamic SUID
lookup and attribute queries, seven independent subscription states, rate/batch
aggregation across eight consumers and actual disable/flush completion.
A definitely unsubmitted packet may stay queued. Submitted/unknown packets are
not resent. The real transport owner is responsible for a new epoch/session
following loss; this component never restarts another processor.

Each report is fully parsed before delivery. All samples keep their SSC timestamp;
calibration streams carry timestamped bias snapshots. Physical SEE has already
applied factory scale, matrix and mounting. The client subtracts only the dynamic
bias in the calibrated accel/gyro flow, matching the measured stock qsh HAL.
Gravity1025 has six floats: gravity[3] and linear acceleration[3], in m/s2.
Near/far769 includes status and raw ADC; thresholds and compensation remain
inside the stock TCS3760 driver. No raw ADC is advertised as centimetres.

A QTimer/host pair supplies sample-time translation. Q32.32 intermediates,
Q16.16 output and overflow rejection use scalar integer arithmetic; no FP/SIMD
or128bit runtime division is required by the native kernel. Ordered calibration
wait queues and HID event queues have explicit capacity failures.

This is a source candidate. Executable tests, target compile/PAC, strict prelink,
image assembly and real device acceptance wait for the designated baseline.

The iPhone D16 CoreMotion six-axis backend selects FF0C/5; its original decoder
requires a60-byte type4 payload (clientID64@4,xyzw@12,userAccel@28,rotationRate@40).
QcomSscMotion pairs actual SEE game_rv/gravity/gyro samples, converts gravity to
Apple g semantics and encodes that defined payload. Per-source timestamps,
maximum40ms skew and missing-source rejection stay explicit. Quaternion
conjugation implements SEE world->device to Apple device->reference; physical
six-face/three-axis motion validation remains required before acceptance.

The board supplies ordinary IOHID event services and the defined FastPath
open/copy/properties/close lifecycle. Real users select rates/batches through
bounded owner commands. Priming discovers the service using one valid frame;
after publication the initial demand is released, leaving actual consumers.
The normal accel/gyro services and fusion users aggregate separately, so a
fusion client exiting cannot disable raw gyro data requested by another client.

Rebind accepts only an explicitly owner-established fresh remote session with a
larger epoch. It restores saved real demands and rediscovers identity/rates and
biases; this API never performs PAS/GLINK restart. The actual native caller must
connect the owner recovery path after the selected package is reviewed.

An opted-in board may use physical SEE accel samples when the acknowledged dynamic accel_cal lookup is empty, matching the measured nezha stock HAL. An unresolved lookup or advertised calibration provider still requires its bias stream; gyro always keeps its required dynamic calibration. Missing calibration is not replaced with a fabricated bias.
