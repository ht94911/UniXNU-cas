# CPU0 VM observation candidate004

Candidate003 reached the actual XNU start_first_cpu on this cas and its bound
record survived Android recovery. Candidate004 uses the authorized macos-15 /
Xcode16.4 build40217312445, kernel SHA-256
b5e428922606300c5f81b49884146f2a74939995d29bc148d6319c5a1d73a83e.
The new MMU/arm_vm_init/early-PE observations have not been tested on the phone.

Use Prepare high RAM ownership, then Boot XNU CPU0. A separate prior-boot
Validate is optional. Every Boot still requires current FDT/SMEM/UEFI facts,
CPU affinity0, exact256MiB ownership, two complete address-pattern readbacks,
cache/clear and the kernel/ADT/args checks before ExitBootServices.

The original256-byte ABI is followed by a128-byte terminal exception record.
The viewer accepts ESR/FAR/ELR/SPSR/TTBR/SP, vector and previous phase only after
stable double reads, boot/candidate identity and XOR/inverse checks. EL1 SP0
exceptions recover the interrupted SP_EL0. The final TTBR switch to permanent
mailbox IO remapping has a declared capture gap; phase56 alone is not a fault
location. Panic strings are not captured.

Phase53 means arm_vm_init returned and the mailbox IO map was acquired.
Phases54/55 are early PE(TRUE)/diagnostic stop, before clocks, IOKit,
machine_startup and storage. They do not establish ArmDTPlatform::configure or
complete X09 platform acceptance. No UFS, Data, SMP or userland is enabled.

Unchanged003 firmware/shim/DTB/menu bytes are reused with an explicit source
compatibility receipt. They are not claimed to have been rebuilt. The new
kernel/EFI/ADT/FAT/boot image has a new identity. GitHub is used only for the
specifically authorized Mac source build; phone images remain private.
