# Source attribution

The locked `btqca.c` and `btqca.h` originate from
[ianchb/sm8550-mainline](https://github.com/ianchb/sm8550-mainline), commit
`1fb56e66091e71160d7c7f8e13b065d5b6ab8b8c`. The generated copies retain their
original notices and GPL-2.0-only license.

GENI register and DMA sequences follow `qcom_geni_serial.c`, `qcom-geni-se.c`
and `geni-se.h` at that same revision. Their identities are recorded in
`tools/source-lock.json`.

`src/qbt_transport.c` derives from
[Renoir XNU Bluetooth](https://github.com/daimile2/renoir_xnu_bluethooth/tree/2b9d59c921c9ec803462b2be8deb6980f7297c22),
`kernel/files/bsd/dev/renoir_bluetooth_transport.inc`, by daimile2 and
contributors. Its APSL-2.0 notice accompanies the file. The WCN7850 adapter
adds channel lifetime coordination, power completion requests, range checks,
queue copies and nonfatal packet allocation handling.

The sheng board references are
[sm8550-mainline](https://github.com/ianchb/sm8550-mainline) and
[debian-sheng](https://github.com/ianchb/debian-sheng).

The separately locked WCN7860 initializer is byte-identical to
`yzddmr6/linux-sm8750-piano` commit `9321086f2e2234bfdf073ac3326bd88cb5053e4f`.
Its GPL notices remain in `vendor/linux-wcn7860`; the closed PR4 FC48 changes
are not treated as merged or hardware-verified code.

`src/qbt_peri.c` is an independent implementation of the N0 QTI HAL wire
protocol, verified against the local HAL SHA256
`d6d6c883ecb3f904fa3907d85f67b47396ee7e9d25231e7016754742d4519844`.
The command/event, PERI/TME and activation call chains are documented in
the consuming device's `SOURCE.md`. No HAL code or firmware bytes are copied
into this component. `qbt_firmware_prepare_peri_nvm()` reuses this component's
original TLV validator and the actual HAL tag17 baud contract. The existing
executor, H4/IBS and GENI implementation retain their original licenses.
