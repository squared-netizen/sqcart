# Squared Cartridge 3.0: red-team review

**2026-11-18.** A hostile reading of the first 3.0 draft
(`sq-cartridge-3.0.md`, commit b92935a), alongside the published
problems of formats and stores that solve the same problems: LMDB and
Bolt (two-slot commits), SQLite (atomic commit), Electron's asar
(an indexed archive with integrity), Godot's PCK (game data packs and
patches), zstd (frames and dictionaries), ZIP and its extractors (zip
bombs, Zip Slip), Android's APK signing (Janus), git (paths on
case-insensitive systems), and the crash-consistency research on
POSIX file systems. Every finding below has been fixed in the draft or
written into it as a stated limit. The section numbers are the revised
draft's.

Severity: **High** means a reader can be made to return wrong data,
lose a commit, or be compromised. **Medium** means a crash or a hostile
file costs data or resources, or two implementations disagree.
**Low** means a portability or usability fault.

## Summary

| # | Finding | Severity | Resolution |
|---|---|---|---|
| R1 | Both superblock slots in one 4096-byte page | High | slots on separate pages; header 8192 bytes (§3.1) |
| R2 | Feature flags rewritten in place in the header | High | flags move into each commit record (§3.9) |
| R3 | A sequence number can be reused after a fallback | Medium | next = higher slot + 1, valid or not (§10.3) |
| R4 | Locking the cartridge file loses commits across compaction | High | lock a separate `<file>.lock` (§10.2) |
| R5 | zstd frame sizes and windows trusted | High | one frame, declared size required and checked, window capped (§4.3) |
| R6 | Partial reads unverified; a big entry rewritten whole for any change | High | chunked entries and a tree hash (§4.5, §9.1) |
| R7 | Dictionaries named by entry index | High | named by zstd dictionary ID, immutable (§4.3) |
| R8 | Content digest depends on JSON serialisation | Medium | length-prefixed framing, RFC 8785 canonical JSON (§9.2) |
| R9 | Commit chain can loop or point anywhere | High | ordering and region checks per commit, overflow checks (§3, §3.4) |
| R10 | Entries may share or overlap bytes | Medium | identical or disjoint runs only (§3.6) |
| R11 | Unsigned trailing bytes make polyglots possible | High | trailing bytes ignored and reported; format chosen by magic only (§3.3, §9.4, §13) |
| R12 | Symbolic links in pack and unpack (Zip Slip) | High | never followed; `O_EXCL` creation (§6.1) |
| R13 | Case collisions allowed, a regression from 2.0 | High | collisions forbidden under Unicode case folding (§4.1) |
| R14 | A workspace's `.squared/` would be packed | Medium | `.squared/` and `.git/` are never entries (§6.1, §6.2) |
| R15 | `mappable` aligned to 4096 on 16 KiB-page devices | Medium | 16384 alignment (§3.6) |
| R16 | "fsync" underspecified | Medium | per-system flush, folder flush, Windows rename (§10.3, §10.4) |
| R17 | Android shared storage and FAT32 | Medium | private storage advised; 4 GiB refusal (§10.6) |
| R18 | Titles can carry bidi and control characters | Low | shown visibly or stripped (§4.2) |
| R19 | JSON duplicate members, big integers | Medium | I-JSON everywhere (§5) |
| R20 | Version ranges "as npm and Cargo write them" disagree | Medium | one grammar, defined (§8) |
| R21 | Signatures: no algorithm field, unsigned signer name | Medium | algorithm per signature, name signed, strict Ed25519 (§9.3) |
| R22 | Rollback to an older signed state | Medium | stated; consumers refuse downgrades (§9.4) |
| R23 | Verification bypassed through another load path | High | runtimes load only verified cartridges (§11) |
| R24 | A file changes after it was verified | Medium | stated; copy to private storage (§9.1) |
| R25 | 3.0 cannot be streamed | Low | stated (§3.3) |
| R26 | Output not reproducible | Low | fixed layout order, `SOURCE_DATE_EPOCH` (§3.10) |
| R27 | A metadata-only change rewrites content | Low | records may name earlier runs (§3.6) |
| R28 | Exploded form: no signatures, stale `entries.json` | Low | signatures carried across pack; orphans reported (§6.1) |
| R29 | The Drive's staging beside the cartridge | Low | staging in the Drive's own storage (§10.5) |

## Findings

### R1. Both superblock slots in one page (High)

The draft put slot A at offset 64 and slot B at 128, in the same
4096-byte page. Storage writes whole pages or sectors; a power cut
during the write of slot B can leave that page torn, and slot A, in the
same page, torn with it. The whole point of two slots is that one
survives.

LMDB keeps its two meta pages on separate pages for this reason, and
has argued about the case anyway: its maintainers declined a meta-page
checksum as a format change, and a corrupted meta page there needs
manual recovery from the other one. Bolt's developers rely on the meta
fitting in one sector, page-aligned. SQLite's documentation is explicit
that its atomic commit depends on "powersafe overwrite", the assumption
that a write does not damage bytes outside it, and that some storage
does not provide it.

**Fix.** Slot A on page 0, slot B on page 1; the header is 8192 bytes.
The draft already had a checksum per slot, which LMDB lacks.

### R2. Feature flags in the header (High)

The draft's header held the feature flags, and the `zstd-dictionaries`
bit was "set only when some entry uses a dictionary". The first commit
to use a dictionary therefore had to rewrite the header in place,
outside the slot protocol, with no crash safety: a torn write there
destroys the magic.

**Fix.** Feature flags live in each commit record, and describe that
commit's state. Nothing in the header changes after creation except the
two slots.

### R3. Sequence numbers after a fallback (Medium)

If the newer slot names a commit whose bytes never reached storage, a
reader falls back to the older one. The draft said the next commit
takes "the next sequence number", which could be the same number as the
invalid slot's. Two different commits with one sequence number make
"the higher slot wins" ambiguous after the next crash.

**Fix.** The next sequence number is one more than the higher of both
slots, whether or not that slot was valid (§10.3).

### R4. Locking the file that compaction replaces (High)

The draft allowed the writer's lock to be on the cartridge file itself.
Compaction renames a new file over the old one. A second writer blocked
on the old file's lock acquires it once the first releases, then
appends to the old, now unlinked, file: its commit is reported durable
and is gone. This is why SQLite and git use lock files rather than locks
on the data file they replace.

**Fix.** The lock is always `<file>.lock`; on systems without working
`flock` (some FUSE mounts) the lock file records host, PID and start
time, and a stale lock is taken over only when that process is gone
(§10.2).

### R5. Trusting zstd's frame header (High)

The draft compared nothing against the frame. A zstd frame may omit its
content size, may declare a wrong one, may need a window of up to
several GiB, and may be followed by skippable frames or another frame.
The zstd manual warns that for untrusted input the declared size "could
be wrong or intentionally modified"; RFC 9659 made a window limit a
requirement for HTTP for the same reason; services such as Vector cap
both the window and the output.

**Fix.** A run is exactly one frame; its declared content size is
required and must equal the record's; the reader sets
`ZSTD_d_windowLogMax` and stops at the declared size; a window larger
than 2^23 needs the required feature bit `zstd-window-27` (§4.3).

### R6. Partial reads, and big entries (High)

This is the finding that changed the draft most, and it is the same
question as the Virtual Cartridge Drive's.

The draft verified an entry's SHA-256 "on the first full read". A
reader that maps a big texture, or seeks inside a 4 GiB video, never
does a full read, and so never verifies anything: a signed cartridge
gave no assurance about the bytes actually used. Electron's asar has the
same problem and solved it with a hash per block of `blockSize` bytes
in its header. And because an entry was one zstd frame, a reader could
not start in the middle, and a writer had to rewrite all 4 GiB to change
one byte, which defeats "a commit costs what it changed" for exactly
the big files the Drive exists for.

**Fix.** Chunked entries (codec 3, §4.5): fixed 1 MiB chunks, each its
own run with its own codec, listed in a chunk table with a SHA-256 per
chunk. The entry's digest becomes a tree hash over 1 MiB leaves (§9.1),
the same for chunked and unchunked entries, so chunking stays a storage
choice the content digest ignores. A reader verifies the chunk table
against the digest once and then each chunk as it reads it. A writer
changes a range by writing the chunks it touches and a new table that
names the old runs for the rest (with R27). Fixed-size chunks favour
in-place edits; inserting bytes still rewrites what follows (content-
defined chunking would avoid that, at a cost in complexity and in
predictable offsets; not taken).

### R7. Dictionaries by entry index (High)

The draft's record named its dictionary by "the index of the entry
holding the dictionary", in whichever index the lookup ended in. With
delta indexes the same number means different entries in different
commits, and a later commit that replaced or deleted the dictionary
silently broke every entry compressed with it. zstd itself has a
mechanism for this: every frame carries the ID of its dictionary, and
decoders that hold several dictionaries (zstd-jni, the kernel's) pick
by it, and reject a mismatch.

**Fix.** The record holds the zstd dictionary ID; the dictionary is the
entry `SQ-INF/dictionaries/<id>.zdict`; the frame must carry the ID;
dictionaries are immutable while used; IDs below 32768 (reserved by
zstd) are not used (§4.3).

### R8. A digest over serialised JSON (Medium)

The draft hashed lines of `<path> LF <hex> LF <json> LF`, with the
metadata as JSON "with no whitespace". Two correct serialisers still
differ: number forms (`1e3`, `1000`), string escapes (`é`, `é`),
and member order inside `attributes`. An entry's attributes travel
through `entries.json` in exploded form and back, so a round trip
through another tool could change the digest of an unchanged cartridge
and break its signatures.

**Fix.** Length-prefixed fields instead of lines, and metadata in RFC
8785 canonical JSON; writers store attributes canonically and
validators report those that are not (§9.2).

### R9. The commit chain (High)

Commit records point back to their predecessors, and readers walk that
chain. The draft said a record "never points forward" but did not make
the reader check it, so a hostile file could loop (a record pointing to
itself), point into data, or claim regions that overlap another
commit's. Offsets plus sizes in 64 bits can also wrap.

**Fix.** §3.4 lists the checks a reader applies to every commit it
walks: sequence numbers strictly increase, the previous record lies
before this commit's regions, regions lie inside this commit and do not
overlap, the chain length increases by one. §3 requires overflow-checked
arithmetic.

### R10. Overlapping entries (Medium)

Nothing stopped two records from naming overlapping bytes. David
Fifield's "better zip bomb" builds a 42 kB file that inflates to
terabytes by overlapping ZIP entries; Info-ZIP's fix (CVE-2019-13232)
detects and rejects overlap. Overlap is also how two readers can be made
to disagree about what is where.

**Fix.** Runs are either identical (deliberate sharing: renames,
metadata-only changes, unchanged chunks) or disjoint; a reader refuses
partial overlap (§3.6). Total work stays bounded by §3.8's limits.

### R11. Trailing bytes and polyglots (High)

The draft ignored bytes after the current commit and signed only the
content digest. Anything can therefore be appended to a signed
cartridge, including a ZIP central directory: ZIP readers find an
archive from its end, so the file becomes a valid 3.0 cartridge and a
valid ZIP with different contents. Since 3.0 readers may also read 2.0
ZIP cartridges, one tool could see one cartridge and another a
different one, both "signed". This is the shape of Android's Janus flaw
(CVE-2017-13156): extra bytes the v1 signature did not cover let one
file be both a signed APK and a DEX.

**Fix.** The format is decided by the first bytes alone and a 3.0 file
is never tried as ZIP (§3.3, §13); trailing bytes are reported by
validators; §9.4 states that only the state is signed and that a
distributed cartridge should be compacted.

### R12. Symbolic links, and Zip Slip (High)

Exploded form packs "the folder's files". A symbolic link in the folder
would pack whatever it points to (a key, a password file) into a
cartridge someone then shares. In the other direction, extractors keep
being caught writing through links: Snyk's Zip Slip research, the Rust
`zip` crate's CVE-2025-29787 (a link earlier in the archive used by a
later entry), and `extract-zip` and `decompress` advisories that
bypassed their own earlier fixes. 3.0 has no link entries, but unpack
can still meet a link already in the target folder.

**Fix.** Pack refuses links and never reads through one; unpack refuses
a non-empty target unless merging, creates files with `O_EXCL` and
without following links (§6.1).

### R13. Case collisions (High)

The draft said "two paths that differ only by case are distinct". 2.0
§3.3 forbade that, for good reason: Android's shared storage, macOS,
Windows and FAT fold case, so two such entries unpack to one file, and
which one wins is up to the extractor. git's CVE-2021-21300 turned a
case collision on such a file system into code execution on clone. 2.0
folded ASCII only; those file systems fold far more (`É` and `é`).

**Fix.** Restore 2.0's rule and widen it to Unicode full case folding
after NFC. Pack converts macOS's NFD names to NFC first (git needed
`core.precomposeunicode` for the same thing). Unpack also refuses names
Windows cannot create (§4.1).

### R14. A workspace packs its own working state (Medium)

A workspace is an exploded cartridge, and the draft kept square's
journal, staging and lock in `.squared/` inside it. Exploded form's
entries were "the folder's files", so packing a workspace would have
packed its lock and journal, and `.git/` too.

**Fix.** The top-level `.squared/` and `.git/` are never entries (§6.1).

### R15. Mapping alignment on 16 KiB pages (Medium)

`mmap` needs a file offset that is a multiple of the page size. Android
15 supports 16 KiB pages and sqterm already links with
`max-page-size=16384`; a `mappable` entry aligned to 4096 could not be
mapped there.

**Fix.** 16384 alignment (§3.6). apkpack stores `.sq` assets
uncompressed and 16384-aligned so the same holds inside an APK (§10.6).

### R16. What "fsync" means (Medium)

Pillai et al.'s study of crash consistency (OSDI 2014, "All File
Systems Are Not Created Equal") found that applications' commit
protocols broke on file systems that reorder appends and renames, and
that a rename is not durable until its folder is flushed. On macOS
`fsync` does not flush the drive's cache; SQLite uses `F_FULLFSYNC`.
On Windows, a file open without `FILE_SHARE_DELETE` cannot be renamed
over.

**Fix.** §10.3 defines flush per system and flushes the folder when a
file is created; §10.4 flushes the folder after compaction's rename and
says what happens on Windows.

### R17. Android shared storage and FAT32 (Medium)

Since Android 11, `/sdcard` is a FUSE file system: slower than private
storage (Google's own guidance says so), writable by other apps with
storage permission, case-folding, and of uncertain `flock` support. A
removable card may be FAT32, where a file cannot reach 4 GiB, and a
growing cartridge would fail mid-commit.

**Fix.** §10.6: change cartridges in private storage; refuse a commit
that would pass FAT32's limit. This bears on the planned
`/sdcard/sqterm/packages` folder: fine for packages pkg reads, not for
cartridges the Drive changes.

### R18. Deceptive titles (Low)

Titles exist to be shown in place of paths. A title with bidirectional
override characters can display reversed or hide text, as the
"Trojan Source" attack did in code (CVE-2021-42574).

**Fix.** Tools show such characters visibly or strip them; writers
should refuse control characters in titles and authors (§4.2).

### R19. JSON that parses two ways (Medium)

Records, attributes, the manifest and `entries.json` are JSON. Parsers
disagree on duplicate member names (first wins, last wins, error) and on
integers beyond 2^53. A cartridge that two parsers read differently can
pass a check in one tool and act differently in another.

**Fix.** I-JSON (RFC 7493) for every JSON text, refused rather than
guessed; nesting limit of 64 (§5, §3.8).

### R20. Version ranges (Medium)

"As npm and Cargo write them" is two different languages: a bare
`1.2.3` is exact in npm and `^1.2.3` in Cargo, and they treat
pre-releases differently.

**Fix.** §8 defines one grammar and its meaning, with npm's pre-release
rule.

### R21. Signature details (Medium)

The draft's signature block had no algorithm field, so moving off
Ed25519 later would be a format break; its signer name was not signed,
so anyone could relabel a signature "Squared Netizen"; and Ed25519
implementations differ in which edge-case signatures they accept.

**Fix.** An algorithm number and key length per signature; the signer
name inside the signed message, and still never shown without its key;
RFC 8032 §5.1.7 verification with non-canonical encodings rejected; at
most 16 signatures (§9.3, A.5).

### R22. Rollback (Medium)

A signature covers a state. An old signed cartridge, or this one cut
back to an earlier commit, still verifies. Update systems call this a
rollback or freeze attack (TUF's threat model); signatures alone cannot
prevent it.

**Fix.** Stated in §9.4: consumers that install or update compare
`id` and `version` with what they have and refuse to go backwards.

### R23. A second, unverified load path (High)

Electron's asar integrity could be bypassed (CVE-2024-46992), and its
documentation says integrity checking is only meaningful with
`onlyLoadAppFromAsar`, because otherwise the app loads code from a
folder beside the archive. A squared runtime that accepts both packed
and exploded cartridges has the same exposure.

**Fix.** §11: code is loaded only through a verifying reader, and a
runtime never falls back to an exploded folder when a packed cartridge
fails verification.

### R24. Changed after verification (Medium)

A cartridge on shared storage can be rewritten by another app after a
reader has verified it, including under a mapping.

**Fix.** Stated in §9.1: a runtime that relies on a cartridge being
unchanged copies it to private storage and verifies the copy, or
verifies every read.

### R25. No streaming (Low)

A ZIP can be read from a pipe; a 3.0 cartridge needs its header and
then its last commit. That is inherent in an indexed, appendable file.

**Fix.** Stated (§3.3, the summary table): tools store a stream whole
first.

### R26. Reproducible output (Low)

Godot's documentation warns that re-exporting the same project can give
a slightly different PCK, which breaks delta patches. The draft's
commit time and unspecified layout order would have done the same.

**Fix.** §3.10 fixes the layout order and takes the commit time from
`SOURCE_DATE_EPOCH`.

### R27. Metadata-only changes (Low)

Retitling a 2 GiB entry had to rewrite 2 GiB, because records could
only name data in their own commit.

**Fix.** Records and chunk tables may name runs from earlier commits
(§3.6). This is also what makes R6's range writes cheap.

### R28. Exploded form loose ends (Low)

Where an exploded cartridge's signature goes was undefined, and a file
renamed by another program left a record in `entries.json` describing
nothing.

**Fix.** Exploded cartridges carry no signature; `pack
--signatures-from` copies one across since the digest is unchanged;
orphan records are reported, and pack refuses them unless told to drop
them (§6.1).

### R29. The Drive's staging (Low)

The draft put staging in `<file>.vcd/` beside the cartridge. A cartridge
inside an APK, or on read-only storage, has nowhere beside it to write.

**Fix.** Staging belongs in the Drive's own storage (§10.5).

## The Virtual Cartridge Drive

The Drive is its own tool, a consumer of cartridges, and the revised
§10 says so. The line between it and sqcart:

- **sqcart**, and only sqcart, writes cartridge bytes: open for writing
  (the lock, truncating trailing bytes), put an entry, put a range of a
  chunked entry, delete, set metadata, commit, compact. Every rule in
  this review that protects a file on disk (R1–R4, R9, R10, R16, R17)
  lives there, once.
- **The Drive** owns policy: staging, coalescing, caching, when to
  commit, backpressure, and recovering staged changes after a crash.

What sqcart did need to learn for the Drive is R6: a high-level API for
big files needs a format that can read and replace part of an entry.
Chunked entries give it that, and give every reader verified partial
reads as well.

## Not checked

The issue trackers of these projects were read through search results
and their own documentation, not exhaustively. In particular I found no
LMDB report of both meta pages torn by one write (R1 is reasoned from
the page layout, and SQLite's powersafe-overwrite discussion), and no
source on `flock` over Android's FUSE; §10.2 treats it as unreliable
rather than assume it works.

## Sources

- LMDB meta pages and torn writes: OpenLDAP lists, [thread](https://lists.openldap.org/hyperkitty/list/openldap-bugs@openldap.org/thread/GKA7N753XMOBSUD4FNNJJ24BNX76B63O/), [recovery](https://lists.openldap.org/hyperkitty/list/openldap-technical@openldap.org/thread/FNXZQZYAQZBHLGYHIG7KGNMBDS5R3ECC/); Bolt, [issue 548](https://github.com/boltdb/bolt/issues/548)
- SQLite: [Atomic Commit](https://www.sqlite.org/atomiccommit.html), [Powersafe Overwrite](https://sqlite.org/psow.html)
- Crash consistency: Pillai et al., [All File Systems Are Not Created Equal (OSDI 2014)](https://cs.uwaterloo.ca/~alkiswan/papers/alice-osdi14.pdf); [summary](https://blog.acolyer.org/2016/02/11/fs-not-equal/)
- Electron asar: [ASAR Integrity](https://electronjs.org/docs/latest/tutorial/asar-integrity), [CVE-2024-46992](https://www.wiz.io/vulnerability-database/cve/cve-2024-46992)
- Godot PCK: [Exporting packs, patches, and mods](https://docs.godotengine.org/en/stable/tutorials/export/exporting_pcks.html), [large PCK discussion](https://forum.godotengine.org/t/how-does-godot-handle-large-pck-files/142876)
- zstd: [manual](https://src.rcs.uwaterloo.ca/source/xref/netbsd-src/external/bsd/zstd/dist/doc/zstd_manual.html), [RFC 9659](https://datatracker.ietf.org/doc/rfc9659/), [Vector's decompression limits](https://rust-doc.vector.dev/vector_common/decompression/index.html), [zstd-jni dictionary selection](https://github.com/luben/zstd-jni/pull/262), [dictionary example](https://android.googlesource.com/platform/external/zstd/+/8b7e1b79/examples/dictionary_decompression.c)
- Zip bombs: [CVE-2019-13232](https://access.redhat.com/security/cve/CVE-2019-13232), [Alpine's patch](https://git-old.alpinelinux.org/aports/tree/main/unzip/CVE-2019-13232.patch?h=v3.10.5)
- Zip Slip and links: [Rust zip CVE-2025-29787](https://security.snyk.io/vuln/SNYK-RUST-ZIP-9460813), [extract-zip](https://security.snyk.io/vuln/SNYK-JS-EXTRACTZIP-19653641), [decompress](https://security.snyk.io/vuln/SNYK-JS-DECOMPRESS-16415209), [CodeQL guidance](https://codeql.github.com/codeql-query-help/go/go-unsafe-unzip-symlink)
- Janus: [The Hacker News](https://thehackernews.com/2017/12/android-malware-signature.html)
- git and case-insensitive file systems: [CVE-2021-21300](https://tenable.com/cve/CVE-2021-21300), [precompose patches](https://lore.kernel.org/r/20210124151306.23185-1-tboegi@web.de)
- Android storage: [FUSE passthrough](https://source.android.com/docs/core/storage/fuse-passthrough), [scoped storage](https://source.android.com/devices/storage/scoped?hl=en)
