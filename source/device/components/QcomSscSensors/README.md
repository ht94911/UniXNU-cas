# QcomSscSensors

Local source candidate for Qualcomm SEE/SSC normal `SNS_CLIENT` service400
(IDL1.7). Reuses nezha ALS017 wire and the existing QcomQrtr transport.
The component owns decoding, subscriptions, per-sample clock mapping and
calibration semantics. It owns no DSP, GLINK, IRQ, regulator or physical bus.

Sources are BSD-2-Clause-Patent. Protocol facts were independently transcribed
from the stock HAL and the fixed SNS API references; vendor binaries, generated
vendor protobufs, private calibration and Apple files remain outside Git.

The board supplies the established router, actual advertised endpoint, counter
and Darwin clock pair, and serialized consumer callbacks. No catalog, build or
phone selection is enabled before the human chooses a test baseline.
