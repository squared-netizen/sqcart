# Squared Cartridge Specification, Version 3.0

**Status: draft, 2026-11-18.** Nothing here is implemented. Version 2.0
(`sq-cartridge-2.0.md`) stays the normative format until this one is
accepted and sqcart reads and writes it.

Version 3.0 replaces the container. A cartridge is no longer a ZIP
archive: it is a single file in a format of its own, built for three
things ZIP was never built for:

- **fast reads of one entry among millions**, by binary search over a
  memory-mapped index, with no parsing and no allocation;
- **changes without rewriting the file**, by appending entries and a new
  index, which is what the Virtual Cartridge Drive (§10) needs;
- **metadata on every entry**: a title independent of the file name,
  r/w/x flags, an author, created and updated times, a media type.

The manifest, its envelope and `consumers` (2.0 §5) carry over almost
unchanged, so most of what a consumer knows survives. What changes is
listed below.

| Area | 2.0 | 3.0 |
|---|---|---|
| Container | ZIP (APPNOTE 6.3.x profile) | own format: header, data blocks, sorted index, commit records (§3) |
| Compression | Stored or Deflate | Stored, zstd, or zstd with a shared dictionary (§4.3) |
| Entry metadata | a path and a size | path, title, flags (r, w, x), author, created, updated, media type, attributes (§4.2) |
| Changing a cartridge | rewrite the archive | append a commit; compact later (§10) |
| Integrity | `SQ-INF/hashes.json` | a SHA-256 per entry in the index; the content digest in each commit (§9) |
| Signatures | reserved | defined: Ed25519 over the content digest (§9.3) |
| Dependencies | not defined | `requires` in the envelope, resolved offline by consumers (§8) |
| Structured data | files | also record entries: flat JSON arrays of objects (§5) |
| Exploded form | `SQ-INF/manifest.json` | the same, plus `SQ-INF/entries.json` for entry metadata (§6) |
| Workspaces | squared-pg's `.squared/metadata.json` | a workspace is an exploded cartridge of kind `workspace` (§6.2) |

## 0. Scope

This document specifies the container, the entries in it, the manifest
envelope, integrity, signatures, dependencies as data, and the on-disk
rules for changing a cartridge. It does not specify any consumer's
section, any kind's meaning, or a trust policy: those stay with the
tools that read them (2.0 §5.6, kept).

The independence requirement of 2.0 §0.1 stands: a cartridge is usable
by any tool that implements this document, with no squared-pg, square
or framework present.

## 1. Design goals

In priority order, where they conflict:

1. **Correct and recoverable.** A crash at any moment leaves a cartridge
   that opens as its last completed commit. Nothing a reader is given can
   make it read outside the file or allocate without bound.
2. **Fast, small reads.** Opening a cartridge reads a fixed header and
   one commit record. Finding an entry is a binary search in a mapped
   index. Reading it decompresses that entry alone.
3. **Small files.** zstd at high levels, shared dictionaries for many
   small similar entries, and already-compressed data (PNG, JPEG, zstd)
   stored as it is.
4. **Cheap changes.** A commit costs what it changed, not the size of the
   cartridge.
5. **Reproducible.** The content digest depends on entries and their
   metadata, never on compression or layout (§9.2).

Non-goals: random in-place writes inside an entry (an entry is replaced
whole), concurrent writers (one writer at a time, enforced by a lock
outside this format), and network resolution of anything.

## 2. Terminology

RFC 2119 key words. **Cartridge**, **reader**, **writer**, **payload**
and **kind** as in 2.0 §2, and:

- **Entry**: one named item in a cartridge: a path, metadata, and content
  (or a tombstone, §3.6).
- **Commit**: one append of entries, an index and a commit record (§3.4).
  A cartridge is the state named by its latest valid commit.
- **Live entry**: an entry in the current state, after deltas and
  tombstones are applied.
- **Compaction**: rewriting a cartridge as a single commit holding only
  live entries (§10.4).

## 3. Container format

All integers are little-endian. Offsets and sizes are unsigned 64-bit,
counted in bytes from the start of the file. Appendix A gives every
structure byte by byte.

### 3.1 Layout

```text
offset 0      header (4096 bytes): magic, version, two superblock slots
4096          commit 1: data blocks, index, string table, commit record
...           commit 2: the same, for what commit 2 changed
...           commit n
```

A cartridge grows only at its end, except for the superblock slots in
the header, which a commit rewrites last (§3.3).

### 3.2 Header

The first 4096 bytes. It begins with the magic
`89 53 51 43 0D 0A 1A 0A` (`\x89SQC\r\n\x1a\n`). Like PNG's, it catches
a file mangled by a text-mode transfer, and it is never valid UTF-8.

The magic is followed by the format's major and minor version (3, 0),
feature flags (§3.9), and two **superblock slots**, A and B. Each slot
names a commit: its sequence number, its record's offset and length,
the file length when it was made, and a checksum over the slot.

### 3.3 Opening a cartridge

A reader:

1. checks the magic and the major version (it MUST refuse a major
   version it does not know, and MUST open any minor version of one it
   knows);
2. reads both superblock slots and keeps the ones whose checksum is
   correct and whose commit record lies inside the file;
3. takes the slot with the higher sequence number, and reads and checks
   that commit record (§3.4); if it is not valid, it falls back to the
   other slot;
4. refuses the cartridge when neither slot names a valid commit.

Bytes after the end of the chosen commit are a commit that did not
finish. A reader MUST ignore them, and a writer MUST truncate them
before appending (§10.3).

### 3.4 Commits

A commit is written in this order:

1. its data blocks: the content of every entry it adds or replaces;
2. its index: one record for every entry it adds, replaces or deletes
   (§3.5);
3. its string table (§3.7);
4. its commit record: sequence number, the previous commit's offset,
   where its index and strings are, whether its index is full or a
   delta, the number of live entries, the content digest (§9.2), an
   optional signature block (§9.3), and a SHA-256 over the record.

Only then is the inactive superblock slot overwritten to name it
(§10.3). A commit record therefore never points forward, and every
commit's data, index and strings lie before its record.

### 3.5 The index: full and delta

An index is an array of fixed-size **entry records** (128 bytes,
Appendix A.3), sorted by path: byte-wise ascending over the UTF-8 bytes,
with no case folding and no normalisation beyond §4.1. Because records
are fixed-size and sorted, a reader finds a path by binary search over
the mapped array, reading strings only to compare.

A commit's index is either:

- **full**: every live entry, and no tombstones. The first commit's
  index is always full, and so is every compaction's;
- **delta**: only what this commit added, replaced or deleted. A
  deletion is a **tombstone**, a record with the deleted flag (§4.2) and
  no content.

To find a path, a reader searches the newest commit's index, then the
one before it, until it finds the path or reaches a full index. A
tombstone ends the search: the entry does not exist. To list entries, a
reader merges the indexes the same way.

A chain of delta indexes MUST NOT be longer than 32. A writer MUST write
a full index, or compact, before a 33rd delta. This bounds a lookup to
33 binary searches in the worst case and to one after compaction.

### 3.6 Data blocks

An entry's content is one contiguous run of bytes, compressed or stored
as its codec says (§4.3). It starts on a 64-byte boundary. An entry
with the `mappable` flag starts on a 4096-byte boundary and is always
stored, so a reader can map it straight into memory (a font, a texture,
an audio buffer). Padding between runs is zero bytes and belongs to no
entry.

Content belongs to the commit that wrote it. A replaced or deleted
entry's old content stays in the file, unreachable, until compaction.

### 3.7 String table

Paths, titles, authors, media types and attribute objects are stored
once each in a commit's string table: each string is a 32-bit length
and its UTF-8 bytes, with no terminator. An entry record names a string
by its offset in the table. Offset `0xFFFFFFFF` means "none".

A string table belongs to its commit. A delta commit's records name
strings in that commit's own table only.

### 3.8 Limits

A reader enforces limits, for every cartridge, before it allocates or
maps anything (2.0 §3.4, kept and extended):

| Limit | Default | Notes |
|---|---|---|
| Entries in one index | 2^32 − 1 | fixed by the record format |
| Path length | 1024 bytes | |
| String length | 1 MiB | an attribute object larger than this belongs in an entry |
| Entry size, uncompressed | 64 GiB | a reader MAY lower it |
| zstd window | 2^27 bytes (128 MiB) | readers MUST support up to 2^23; a writer MUST NOT exceed the window the reader's profile allows (§4.3) |
| Delta chain | 32 | §3.5 |

A reader MUST refuse a cartridge whose index, strings or data reach
outside the file or outside their commit, before using them.

### 3.9 Feature flags

The header carries 64 feature bits. A bit in the low 32 is
**required**: a reader that does not implement the feature MUST refuse
the cartridge. A bit in the high 32 is **optional**: a reader MAY ignore
it. 3.0 defines one required bit, `zstd-dictionaries` (§4.3), set only
when some entry uses a dictionary. Every other bit is reserved and MUST
be zero.

This replaces 2.0's `requires_features` for container features. The
envelope member stays for features of the payload (2.0 §5.4).

## 4. Entries

### 4.1 Paths

Paths follow 2.0 §3.3 unchanged: relative, `/`-separated, no `.` or
`..` segment, no empty segment, no control characters, at most 1024
bytes, UTF-8 in Normalization Form C. Two entries MUST NOT have the
same path in one index, and two paths that differ only by case are
distinct.

`SQ-INF/` stays reserved (2.0 §4). `SQ-INF/manifest.json` MUST be a live
entry in every commit's state.

### 4.2 Metadata

Every entry record carries:

| Field | Type | Meaning |
|---|---|---|
| path | string | the entry's name; the key |
| title | string or none | a human title, independent of the path ("Northern Province, 1802"); readers SHOULD show it where they would show a name |
| author | string or none | who made this entry; free text, by convention `Name <email>` |
| media type | string or none | an IANA media type (`image/png`, `application/json`, `text/markdown`), or a `vnd.` one (§5) |
| created | int64 | milliseconds since 1970-01-01 UTC; 0 means unknown |
| updated | int64 | the same, for the last change; 0 means unknown |
| flags | 16 bits | below |
| codec | 16 bits | §4.3 |
| size | uint64 | uncompressed bytes |
| stored size | uint64 | bytes in the file |
| offset | uint64 | where the content starts (§3.6) |
| dictionary | uint32 | for codec 2, the index of the entry holding the dictionary (§4.3) |
| sha256 | 32 bytes | over the uncompressed content (§9.1) |
| attributes | string or none | a JSON object of further metadata, defined by consumers |

**Flags.**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `r` | readable: offered to the runtime and to consumers. An entry without `r` is kept for tools only (sources, build notes) and a runtime reader MUST NOT hand it to application code |
| 1 | `w` | writable: the Virtual Cartridge Drive and other writers may replace or delete it. A writer MUST refuse to change an entry without `w` unless it is rebuilding the cartridge from its sources |
| 2 | `x` | executable: a runtime may run it (Lua source, 2.0 §7). It never makes a file executable on disk (2.0 §3.1, kept), and native code stays forbidden (2.0 §8) |
| 3 | `deleted` | a tombstone (§3.5) |
| 4 | `mappable` | stored, 4096-aligned, for direct mapping (§3.6) |
| 5 | `dictionary` | this entry is a zstd dictionary (§4.3); it carries no `r` |
| 6–15 | | reserved, zero |

The flags describe intent, not access control: a cartridge is a file,
and anyone with the file can read every byte of it. They let readers,
the Drive and tools agree on what an entry is for.

`created` and `updated` are metadata like a title, not file times: a
writer copies them from where the entry came from, or sets them from a
caller-supplied epoch for reproducible builds. They are covered by the
content digest (§9.2).

### 4.3 Codecs and compression

| Codec | Name | Content |
|---|---|---|
| 0 | stored | the bytes as they are |
| 1 | zstd | one zstd frame |
| 2 | zstd-dict | one zstd frame compressed with a dictionary; `dictionary` names the entry that holds it |

Deflate is gone. zstd decompresses several times faster than Deflate and
compresses smaller at its high levels, and squared already carries a
zstd decoder (sqterm's packages).

**Choosing.** A writer SHOULD store an entry whose content is already
compressed, or would shrink by less than 5% (PNG, JPEG, WebP, zstd,
audio), and SHOULD compress everything else.

**Maximum compression.** A writer offers profiles, as zstd does:

| Profile | zstd level | Window | For |
|---|---|---|---|
| `fast` | 3 | ≤ 2^23 | the Drive's commits, development builds |
| `small` (default) | 19 | ≤ 2^23 | distributed cartridges |
| `max` | 22, with long-distance matching | ≤ 2^27 | archives where size matters most and readers have memory to spare |

A reader MUST support windows up to 2^23 bytes (8 MiB) and MAY refuse a
larger one. A `max` cartridge therefore says so in its manifest
(`requires_features: ["zstd-window-27"]`), and a phone-sized reader can
refuse it cleanly rather than run out of memory.

**Dictionaries.** Many small, similar entries (thousands of JSON records,
a manual's pages) compress poorly one at a time. A writer MAY train a
zstd dictionary on them, store it as an entry under
`SQ-INF/dictionaries/` with the `dictionary` flag, and compress those
entries with codec 2. A reader loads a dictionary once and keeps it while
the cartridge is open. A dictionary entry MUST NOT itself use a
dictionary.

### 4.4 Images and other binary data

Binary entries need nothing special. A PNG is an entry with media type
`image/png`, codec 0, and, if a renderer will map it, the `mappable`
flag. A title and attributes describe it (`{"width": 256, "height":
256}`), so a tool can list a cartridge's images without decoding one.

## 5. Record entries

A cartridge often holds data rather than files: a game world's
settlements, a manual's index, a package's file list. 3.0 gives that
data one shape every reader can parse, and leaves its meaning to
consumers.

A **record entry** has media type `application/vnd.squared.records+json`
and content that is a JSON array whose every element is a JSON object:

```json
[
  { "id": 1802, "name": "Northern Province", "population": 41250 },
  { "id": 1803, "name": "Riverlands", "population": 18740 }
]
```

- A reader of records MUST check that the array is flat: its elements
  are objects. What an object's members mean is the consumer's; this
  specification gives no member any meaning.
- A consumer says which records it reads, and what they mean, in its own
  section: `"consumers": { "my_game": { "records": { "world/settlements/": "settlement@1" } } }`
  (a path prefix and a schema name of the consumer's choosing).
- **Granularity is a choice with a cost.** The Drive replaces entries
  whole (§10). One record entry holding 100,000 objects is small and
  fast to read, and every change to one object rewrites all of them; one
  entry per object makes every change cheap, and a dictionary (§4.3)
  keeps 100,000 small entries small. A consumer whose data changes at
  run time SHOULD split it so that one change touches one small entry.

## 6. Exploded form, and workspaces

### 6.1 Exploded form

A folder holding `SQ-INF/manifest.json` is a cartridge (2.0 §3.5, kept).
Its entries are the folder's files; there is no index, no codec and no
commit, since the file system holds them.

Entry metadata that a file system cannot hold goes in an optional
`SQ-INF/entries.json`: a JSON object from path to the metadata of §4.2,
by name, with `flags` written as a string of letters (`"rw"`, `"rx"`):

```json
{
  "world/regions/north.json": {
    "title": "Northern Province",
    "author": "Jesse",
    "flags": "rw",
    "media_type": "application/vnd.squared.records+json",
    "created": 1794960000000
  }
}
```

A file with no record there has flags `r`, no title or author, a media
type a reader MAY guess from its extension, and created and updated of
0. File system times are never used: they would make two copies of one
cartridge differ.

Converting between the forms (`sqcart pack`, `sqcart unpack`) carries
every field across, so an archive packed from a folder and unpacked
again gives the same folder.

### 6.2 Workspaces

A **workspace** (a project created by sqpg and managed by square) is an
exploded cartridge of kind `workspace`: its root holds
`SQ-INF/manifest.json`. That replaces squared-pg's
`.squared/metadata.json` with the same format every other manifest uses.

- The envelope's `provenance` (2.0 §5.5) names the template, kits,
  packages and framework the workspace was made from.
- `consumers.square` holds what square keeps: the layers, the user
  working directory, and the per-file provenance table (path, ownership
  class, origin, hash). Its shape is square's to define, as every
  section's is its consumer's.
- Working state that is not part of the workspace's identity (square's
  journal, staging folder and lock) stays out of `SQ-INF/`, in
  `.squared/`.

An archived workspace is a snapshot: a backup, or a workspace shared as
one file. It is valid, and the Drive (§10) can work on it like any other
cartridge.

## 7. The manifest

The envelope of 2.0 §5.1 carries over with these changes:

- `format_version` MUST equal `3`.
- `requires` is added (§8).
- `tree` stays, with 2.0's rules.
- `kind` stays opaque (2.0 §5.2.1). Kinds in use, for information:
  `application` (2.0's `cartridge`, renamed so it no longer shares the
  format's own name), `template`, `kit`, `package`, `asset_bundle`,
  `plugin`, and new in 3.0, `workspace` (§6.2), `manual` (Manuals'
  documentation) and `data` (a cartridge of records, such as a game
  world).
- `SQ-INF/hashes.json` is removed: integrity lives in the index and the
  commit record (§9). A 3.0 writer MUST NOT write it, and a reader MUST
  ignore one.

Consumers keep their sections. For example, sqterm's packages already
use the envelope (kind `package`). A package's description and the
commands it installs belong in `consumers.sqterm`, as pkg's design
needs: that is sqterm's data, not the format's.

## 8. Dependencies

A cartridge can name the cartridges it needs:

```json
"requires": [
  { "id": "package.squared-core", "version": "^1.1.0" },
  { "id": "kit.lua", "version": ">=0.3.0 <0.5.0", "optional": true }
]
```

| Member | Type | Meaning |
|---|---|---|
| `id` | string | the needed cartridge's identity (2.0 §5.2) |
| `version` | string | a range: an exact version, `^`, `~`, or comparisons joined by spaces, as npm and Cargo write them |
| `kind` | string, optional | the kind it must have |
| `optional` | boolean, optional | default false |

A reader validates the shape and nothing more: it MUST NOT resolve,
download or open anything because of `requires`. Resolution belongs to
consumers (square and pkg, against the cartridges installed on the
machine), offline as everything else. A consumer that cannot satisfy a
requirement that is not optional MUST say which and why, and MUST NOT
proceed as if it had.

Dependencies are on identity and version, never on a path or a file
name (2.0 §1.2, kept).

## 9. Integrity and signatures

### 9.1 Per-entry digests

Every entry record holds a SHA-256 over the entry's uncompressed
content. A reader MUST check it on the first full read of an entry, and
MAY check it on later ones. A mismatch is an error for that entry, never
a silently accepted read.

A tombstone's digest is all zero bytes.

### 9.2 Content digest

Each commit record holds the **content digest** of the cartridge's state
at that commit:

1. Take every live entry.
2. Sort by path, as the index is sorted.
3. For each, emit a line: `<path> LF <hex sha256> LF <metadata> LF`,
   where `<metadata>` is the JSON object of the entry's title, author,
   media type, created, updated, flags (as letters) and attributes, with
   members in that order, absent ones left out, no whitespace.
4. The content digest is the SHA-256 of those lines.

As in 2.0, the digest depends on content and metadata, never on
compression, codec, alignment, dictionaries or layout. A cartridge
compacted, recompressed at another level, or unpacked and packed again
has the same digest. Adding, removing or changing any entry or any of
its metadata changes it.

`offset`, `size`, `stored size`, `codec` and the `mappable` and
`dictionary` flags are storage, not content, and are left out.

### 9.3 Signatures

A commit record MAY point to a **signature block**: a list of signatures
over that commit's content digest.

- Algorithm: Ed25519 (RFC 8032).
- Signed message: the ASCII string `squared-cartridge/3 content-digest`,
  a zero byte, the manifest's `id`, a zero byte, its `version`, a zero
  byte, and the 32-byte content digest.
- Each signature: the 32-byte public key, a key identifier (the first 8
  bytes of the SHA-256 of the public key), an optional signer name, and
  the 64-byte signature.

A signature covers a state, not a commit history. A later commit that
changes anything has a different digest and carries no signature until
someone signs it again. A reader MUST report a cartridge whose latest
commit is unsigned as unsigned, even when an earlier commit was signed.

This specification defines how to verify a signature, not whom to
trust. Trust policy belongs to consumers: pkg's list of trusted keys, a
launcher's, square's. A reader MUST NOT report a cartridge as trusted,
only as signed by a key, or unsigned.

## 10. Changing a cartridge, and the Virtual Cartridge Drive

### 10.1 What this section fixes

The Virtual Cartridge Drive is a library that gives applications a
mutable view of a cartridge. sqcart defines what a valid cartridge is;
the Drive decides how to work with one efficiently. This section makes
normative only what touches the file: how a change is committed, how a
crash is survived, and how a cartridge is compacted. How the Drive
stages, caches and schedules is implementation policy (§10.5).

### 10.2 One writer

Only one process writes a cartridge at a time. A writer MUST hold an
exclusive lock (on the file itself where the system allows it, or a
`<file>.lock` beside it) for as long as it may append. Readers need no
lock: they see the commit a superblock named when they opened the file,
and a commit appended later does not disturb them, because nothing a
commit record reaches is ever overwritten.

### 10.3 Committing

1. Truncate the file to the end of the current commit, discarding any
   unfinished one (§3.3).
2. Append the commit's data blocks, index and string table, then its
   commit record (§3.4).
3. Flush the file to storage (`fsync`).
4. Write the inactive superblock slot to name the new commit, with the
   next sequence number.
5. Flush again.

A crash before step 4 completes leaves the old slot in charge, and the
appended bytes are ignored. A crash during step 4 leaves a slot whose
checksum fails, and the other slot is still in charge. Only after step 5
is the commit durable, and a writer MUST NOT report a commit as done
before then.

### 10.4 Compaction

Compaction writes a new file holding only the live entries, as a single
commit with a full index, to a temporary name beside the old one,
flushes it, checks it, and renames it over the old file. Its content
digest is unchanged (§9.2), so signatures over the state still verify.

A writer SHOULD compact when unreachable data exceeds half the file, or
when the delta chain nears its limit (§3.5). Compaction is also the only
time a cartridge shrinks.

### 10.5 The Drive (informative)

The Drive's design, from its working notes, in this format's terms:

- **Reads** resolve against staged changes first, then the cartridge
  (§3.5's lookup).
- **Writes** go to a staging area on disk beside the cartridge
  (`<file>.vcd/`), so memory stays bounded however much is pending. A
  write to an entry without `w` is refused (§4.2).
- **Coalescing**: an entry changed many times before a commit is written
  once.
- **Commits** are appends (§10.3), triggered by the application's own
  choice: on request, at a loading screen, past a staging-size or time
  budget, at a clean shutdown. A commit is cheap enough that the
  loading screen need not be the only chance.
- **Backpressure**: when staging grows faster than commits drain it, the
  Drive commits before accepting more, rather than growing without bound.
- **Recovery**: staged changes not yet committed are the Drive's own to
  recover or discard after a crash; the cartridge itself is always at a
  valid commit.

## 11. Code in cartridges

2.0 §7 (Lua) and §8 (native code) carry over unchanged. The `x` flag
(§4.2) marks what a runtime may run, and does not relax §8: a cartridge
holding loadable native code is still non-conforming.

## 12. Validation order

A reader validates in this order, and stops at the first failure:

1. Header: magic, major version, required feature bits (§3.2, §3.9).
2. Superblocks and the chosen commit record (§3.3, §3.4).
3. Bounds of every index, string table and data run the commit reaches,
   against the file and the limits (§3.8).
4. The index's order, uniqueness and paths (§3.5, §4.1).
5. The manifest: a live `SQ-INF/manifest.json`, then the envelope as in
   2.0 §12.1, with `format_version` 3.
6. Per-entry digests, as entries are read (§9.1).
7. Signatures, when the caller asks (§9.3).

## 13. From 2.0

- `sqcart convert` reads a 2.0 cartridge (ZIP or exploded) and writes a
  3.0 one: the same entries, `format_version` 3, created and updated of
  0, flags `r` (and `x` on Lua source), media types guessed from
  extensions, `SQ-INF/hashes.json` dropped. A `kind` of `cartridge`
  becomes `application`.
- A 3.0 reader MAY read 2.0 ZIP cartridges too, to ease the move, and
  MUST report them as 2.0. A 3.0 writer writes 3.0 only.
- squared-pg workspaces move from `.squared/metadata.json` to
  `SQ-INF/manifest.json` once, through square (§6.2).
- sqterm's packages (`.tar.zst` with an `SQ-INF/manifest.json` inside)
  can become 3.0 cartridges, read by pkg through sqcart's reader. That
  is a separate decision, recorded in Appendix B.

## Appendix A: byte layouts

All little-endian. `u32`, `u64` unsigned; `i64` signed.

### A.1 Header (4096 bytes at offset 0)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `89 53 51 43 0D 0A 1A 0A` |
| 8 | 2 | major version: 3 |
| 10 | 2 | minor version: 0 |
| 12 | 4 | header size: 4096 |
| 16 | 8 | feature flags (§3.9) |
| 24 | 40 | reserved, zero |
| 64 | 64 | superblock slot A |
| 128 | 64 | superblock slot B |
| 192 | 3904 | reserved, zero |

### A.2 Superblock slot (64 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | commit sequence number (u64; 0 means empty slot) |
| 8 | 8 | commit record offset |
| 16 | 8 | commit record length |
| 24 | 8 | file length at that commit (the record's end) |
| 32 | 24 | reserved, zero |
| 56 | 8 | first 8 bytes of SHA-256 over bytes 0–55 |

### A.3 Entry record (128 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | path: string offset |
| 4 | 4 | title: string offset or `0xFFFFFFFF` |
| 8 | 4 | author: string offset or `0xFFFFFFFF` |
| 12 | 4 | media type: string offset or `0xFFFFFFFF` |
| 16 | 4 | attributes: string offset or `0xFFFFFFFF` |
| 20 | 2 | flags (§4.2) |
| 22 | 2 | codec (§4.3) |
| 24 | 4 | dictionary: entry index, or `0xFFFFFFFF` |
| 28 | 4 | reserved, zero |
| 32 | 8 | content offset |
| 40 | 8 | stored size |
| 48 | 8 | size (uncompressed) |
| 56 | 8 | created (i64, ms since the epoch, UTC) |
| 64 | 8 | updated (i64) |
| 72 | 32 | SHA-256 of the uncompressed content |
| 104 | 24 | reserved, zero |

A dictionary's entry index refers to the full index the lookup ends in,
or to the commit's own delta index; the dictionary entry MUST be live.

### A.4 Commit record

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `SQCOMMIT` |
| 8 | 8 | sequence number |
| 16 | 8 | previous commit record offset (0 for the first) |
| 24 | 8 | index offset |
| 32 | 8 | index entry count |
| 40 | 8 | string table offset |
| 48 | 8 | string table length |
| 56 | 4 | index kind: 0 full, 1 delta |
| 60 | 4 | delta chain length (0 for a full index) |
| 64 | 8 | live entry count |
| 72 | 8 | commit time (i64, ms since the epoch; metadata only) |
| 80 | 32 | content digest (§9.2) |
| 112 | 8 | signature block offset, or 0 |
| 120 | 8 | signature block length |
| 128 | 32 | SHA-256 over bytes 0–127 |

### A.5 Signature block

A `u32` count, then per signature: 32-byte public key, 8-byte key
identifier, `u32` signer-name length and that many UTF-8 bytes, 64-byte
signature.

## Appendix B: open decisions

1. **SHA-256 or BLAKE3.** SHA-256 is used everywhere in the SDK today
   (sources.lock, pkg, apkpack's signing) and has hardware support on
   arm64. BLAKE3 is several times faster in software and tree-hashes
   large entries. Provisional: SHA-256, for one hash across the SDK.
2. **Ed25519's library.** Monocypher (CC0/BSD-2, small, audited) is the
   candidate; apkpack uses BearSSL, which has no Ed25519. Two small
   crypto libraries, or one larger one (mbedTLS) for both.
3. **The delta chain limit.** 32 bounds the worst lookup at 33 binary
   searches. A smaller limit compacts more often; a larger one commits
   more cheaply between compactions.
4. **sqterm's packages as 3.0 cartridges.** pkg would read `.sq`
   through sqcart's reader instead of tar and zstd, and a package would
   gain per-file metadata and a signature. The cost is that a package
   is no longer a tarball a person can open with `tar`.
5. **Per-kind extensions.** 2.0 Appendix B.2's question returns: one
   `.sq` for everything, or `.sqm` for manuals and others for
   readability, with the manifest still authoritative.
6. **Record schemas.** §5 names a schema by a consumer's string. Whether
   the SDK should share schemas (a JSON Schema per name, in a cartridge
   of its own) is open.
