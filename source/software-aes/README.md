# SoftwareAES

Software AES-CBC through IOAESAccelerator.

built into the kernel and used by the sheng crypto path.

The ordinary ARM64 bindings target their qualified iOS17 kernelcache. They must
not be reused for arm64e. iPhone14,4/24A446 has a 0x120-byte AES class, a
0x88-byte crypto parent and 181 authenticated virtual entries. On arm64e,
`getAlgorithmName` is declared by `IOCryptoAccelerator`; declaring it first in
`IOAESAccelerator` produces the wrong pointer-authentication discriminator.

The nezha iPhone integration qualifies every compiled slot against that donor,
uses the existing prelink `.original` adapters, and registers the source class
after loading the genuine parent family. CBC and memory-descriptor/completion
self-tests must pass before service publication. Hardware-key selectors remain
unsupported. This interface preparation is not a hardware result.

Build and interface details: [documentation](https://unixnu.com/drivers/software-aes). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).
