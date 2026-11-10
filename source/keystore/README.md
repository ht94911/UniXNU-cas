# AppleKeyStoreState

AppleKeyStore state and selector handling.

used with `sheng-aks=1`.

The selected upstream software wrapping format is retained. The board supplies
a stable ID through `native_aks_device_id(out, capacity)`, returning 1–32 bytes
or zero when unavailable. Publish that ID before registering the service and
keep it unchanged across boots that share stored items.

Key derivation uses call-local storage so simultaneous clients do not share a
mutable initialization cache. This does not provide hardware data protection;
device lock and reference-key operations remain unsupported.

Run `python3 test_closed_boundary.py` for the actual service with the host
IOKit double, independent format vectors, concurrent derivation and
ASan/UBSan. Target IOKit and persistent userland use require separate checks.

Build and interface details: [documentation](https://unixnu.com/reference/components). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).
