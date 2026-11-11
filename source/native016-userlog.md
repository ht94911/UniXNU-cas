# Native016 consumed UserLog record

Native016 fixes the record comparison that failed actual Native015
run43038356255. The original report consumed SHA was
`e30bf2326b3ca410c643b0cc39e7a265c5ecdff8a45f337474c350966c678389`;
final `subr_prf.o.json` was
`fdb77412a36228398c5dac625e019ecbd2bccdc9cdea423be83afbfb13df2784`.
The 10865-byte records differ only at byte6067: the bootstrap's
`XNU_BOOT_PROGRESS=0` becomes final1. The original CI remains failure.

The standalone builder now reads the actual BSD record bytes once, derives
both SHA and compile flags from those bytes, and saves the raw
`userlog-compile-record.json` inside the same archive attempt. The existing
`userlog_compile_record` and `userlog_compile_record_sha256` report fields
remain; `userlog_compile_record_snapshot` and its SHA identify the durable
snapshot. Failed attempts retain it with their existing evidence.

Qualification requires that snapshot's exact SHA and the complete original
standalone command, including the unchanged selected recipe after the BSD
flags. The final record is still collected and may be byte-identical or differ
only by one exact quoted `XNU_BOOT_PROGRESS=0` to1 token. Directory, source,
output, compiler, arch/target, include/ABI flags and all other bytes remain
exact. Missing/wrong snapshots, other changes or extra overriding command
options are rejected. Existing source/tree/object/member/strong/import/OID and
native initializer checks continue.

The original collection already retains `.json` evidence. Artifact mode maps
the snapshot to
`usb-attempts/<original evidence_directory.parent.name>/userlog-compile-record.json`
without changing the original report, command, `-o` or source directory.
`objects/bsd/DEVELOPMENT/subr_prf.o.json` remains the final original record.

`source/native016-userlog.patch` records the actual new overlay against the
same base framework/USB origins. The complete original Native015 patch and
record remain available separately. The framework wrapper and handler remain
identical to015; only USB `scripts/build.py` changes. The same handler SHA
`4cade6842439ef27afe914d7135c6bc9b99b42afbdeb4f035fcebc17d7a60815`,
48-byte wire, FIFO, actor CDHash, UID/157 permissions and boot/archive sequence
are retained. The private `user_oslog` capability is still unsupported.

The focused record tests use actual015 read-only metadata and a clearly marked
SHA-verified derived consumed record. They test the lifecycle, bad snapshots,
paths/metadata/flags/commands and the actual builder's single-read behavior
with explicit fake compiler/archive outputs. They do not turn the failed015
CI into success or claim Native016 target compilation. The unchanged10
handler gates reuse actual015 source/hash evidence.

Native016 compilation and runtime receipt remain pending the new dedicated
CI/target qualification. Root owns dispatch, signing/package and device work.
