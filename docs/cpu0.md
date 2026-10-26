# CPU0 RAM candidate

This local candidate uses XNU10063.141.1/4KiB, one CPU, fresh private entropy,
DarwinBoot, an explicit high-RAM publication app and a read-only trace viewer.
It has no UFS, Data, touch, GPU, ramdisk, trustcache or addon payload. Host/target
build results do not prove execution on the phone. 

The default menu only reads the trace. There is no automatic timeout boot.
The validation entry and XNU entry use fixed different command lines; other
arguments are rejected. A candidate tag binds profile, execution mode, kernel
SHA and private ADT SHA. Entropy values never enter logs or source.

## Trial A: validate and recover

After a new scoped USB/RAM boot, select diagnostics, then Prepare high RAM
ownership (512MiB). Publication is explicit, fresh and guarded. Return to
UniXNU and select Validate CPU0 memory and load, then return. The loader repeats
current FDT/SMEM/GOP/UEFI facts, allocates exactly0x100004000+256MiB as LoaderCode,
checks the held WB/!runtime/!RO/!RP map and live permissions, writes/reads both
full-range complementary patterns, clears/cleans, then copies kernel/ADT/args.
It releases the pages and returns before EBS. The legacy diagnostic probe is
not an authorization for this shifted range.

Read the trace: CRC/ABI and raw pair valid, candidate MATCH, C/raw stage5,
status0, held range0x100004000/0x10000000, record/current boot stamps equal.
Collect all visible/log evidence and confirm it is complete BEFORE requesting
a return to Android. The operator then chooses Reboot to Android. Verify Android
startup and record the new boot identity. No partition flash or storage write.

## Trial B: recover, then XNU

Perform another scoped RAM boot of the SAME image. Read the previous stage5
trace first: candidate MATCH, record/current boot stamps DIFFERENT, CRC/raw pair
and held-pass fields intact. Stop if missing/corrupt/foreign; do not bypass it.
Then publish high RAM through diagnostics again and select Boot XNU CPU0.
The loader still repeats all current facts and the full held-memory probe.
Previous logs qualify only recovery, never current RAM ownership.

Pre-EBS C records include EL, SCTLR, TCR, stack, entry, args and exact owned
range. The EBS retry loop uses only GetMemoryMap/EBS. Raw0x20 is loader assembly,
0x21 precedes MMU off,0x22 follows MMU off before the branch. Only0x40 (__start)
or0x41 (start_first_cpu) in the actual source-built kernel proves XNU code entry.
A green bar alone proves only loader progress. These markers do not prove VM,
platform initialization, userspace or iOS.

If the kernel stops, record the observation first. Use the accepted long-Power
recovery: hold until an actual restart, then release. If Fastboot is reached,
refresh the scoped cas identity before a reboot command. Check Android startup,
then use a separate recovery RAM boot to read the trace. Never repeat an intent
without first resolving its actual outcome. A lost new trace slot is an unknown
XNU outcome, not a PASS. The accepted cas-complete-001 rescue image remains
separate and unchanged.

## Local reproduction and evidence

Build the diagnostic FD with scripts/build_firmware.py --diagnostics; package
that new base with scripts/package_boot.py --diagnostics --efi-ram-fat. Build
the CPU0 overlay with scripts/cas_cpu0.py --adt-audit PRIVATE_AUDIT --mode execute.
Use scripts/check_cpu0_host.py for the entire loader C and actual held-probe
core with explicit host platform/EBS/handoff doubles. Use scripts/package_cpu0.py
to create the private32MiB FAT and independently decoded Android boot image.
Every output path is new. Keep the private ADT/image permissions and all SHA,
source, target IR, map, input and restoration records. No build tool calls a phone.

Candidate002 records actual MPIDR_EL1 in Reserved[5] and masked affinity in[6], rejects affinity other than0 before allocation, and displays both. The direct EBS/Entry.S/kernel branch path contains no CPU transfer; bind the recovered XNU marker to this pre-EBS CPU identity. The tag now includes all three clean source commits.

## Candidate003 retained-log layout and menu scrolling

The diagnostic volatile prefix ends at offset0x20000. The0x20000..0x30000
handoff slot and0x30000..0x40000 legacy diagnostic slot are retained by the
production BootShim clear helper and excluded from the diagnostic serial buffer.
Normal firmware keeps its full-log clear behavior. The real clear helper is
executed under QEMU at EL1/EL2 for both profiles; serial flood checks both slots.
This fixes a local destructive path. Actual survival through Android still
requires a new validation/Android/recovery pair on the SAME003 image.

The menu now keeps the selected logical entry in its visible row window. The
original entry numbers, physical input, default/timeout rules and reset actions
remain. A source-hash-guarded patch is staged only in the build output; the
shared framework checkout and ABI baseline are unchanged. Host tests execute
the actual patched menu loop and text/portrait/landscape draw coordinates.
Phone display/navigation acceptance is separate from those host checks.

The CPU0 packager requires both the scrolling overlay identity and the actual
diagnostic clear-helper symbol. Trace ABI assertions bind its address/size to
the shared retained-slot definitions. Source commits and fresh private ADT SHA
change the candidate tag, so002 records cannot authorize003 execution.
