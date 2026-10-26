# Firmware build environment

`cas-env.sh` activates the existing venv and selects LLVM 18 for the board firmware
script `scripts/build_firmware.py`. It installs nothing and does not touch a device.
This directory is removed once the firmware builds through the standard
`./unixnu build firmware` step.
