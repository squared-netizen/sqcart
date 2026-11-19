# Squared Cartridge Specification, Version 3.0

**Status: draft, 2026-11-18, revised the same day after a red-team
review (`sq-cartridge-3.0-red-team.md`, which explains every change and
its evidence), and with the hash, signature library and chunk size
decided (Appendix B.1).** Nothing here is implemented. Version 2.0
(`sq-cartridge-2.0.md`) stays the normative format until this one is
accepted and sqcart reads and writes it.

Version 3.0 replaces the container. A cartridge is no longer a ZIP
archive: it is a single file in a format of its own, built for four
things ZIP was never built for:

- **fast reads of one entry among millions**, by binary search over a
  memory-mapped index, with no parsing and no allocation;
- **changes without rewriting the file**, by appending entries and a new
  index, which is what the Virtual Cartridge Drive (§10.5) needs;
- **big entries read and changed piece by piece**: an entry of many
  gigabytes is stored as chunks, each compressed, verified and replaced
  on its own (§4.5);
- **metadata on every entry**: a title independent of the file name,
  r/w/x flags, an author, created and updated times, a media type.

The manifest, its envelope and `consumers` (2.0 §5) carry over almost
unchanged, so most of what a consumer knows survives. What changes is
listed below.

| Area | 2.0 | 3.0 |
|---|---|---|
| Container | ZIP (APPNOTE 6.3.x profile) | own format: header, data blocks, sorted index, commit records (§3) |
| Compression | Stored or Deflate | Stored, zstd, zstd with a shared dictionary, or chunked (§4.3, §4.5) |
| Entry metadata | a path and a size | path, title, flags (r, w, x), author, created, updated, media type, attributes (§4.2) |
| Changing a cartridge | rewrite the archive | append a commit; compact later (§10) |
| Integrity | `SQ-INF/hashes.json` | a SHA-256 tree hash per entry in the index; the content digest in each commit (§9) |
| Signatures | reserved | defined: Ed25519 over the content digest (§9.3) |
| Dependencies | not defined | `requires` in the envelope, resolved offline by consumers (§8) |
| Structured data | files | also record entries: flat JSON arrays of objects (§5) |
| Exploded form | `SQ-INF/manifest.json` | the same, plus `SQ-INF/entries.json` for entry metadata (§6) |
| Workspaces | squared-pg's `.squared/metadata.json` | a workspace is an exploded cartridge of kind `workspace` (§6.2) |
| Streaming | a ZIP can be read front to back | a 3.0 cartridge needs random access: it cannot be read from a pipe (§3.3) |

## 0. Scope

This document specifies the container, the entries in it, the manifest
envelope, integrity, signatures, dependencies as data, and the on-disk
rules for changing a cartridge. It does not specify any consumer's
section, any kind's meaning, or a trust policy: those stay with the
tools that read them (2.0 §5.6, kept). The Virtual Cartridge Drive is a
separate tool built on sqcart; §10.5 says where the line between them
runs.

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
   index. Reading part of an entry decompresses and verifies that part
   alone.
3. **Small files.** zstd at high levels, shared dictionaries for many
   small similar entries, and already-compressed data (PNG, JPEG, zstd)
   stored as it is.
4. **Cheap changes.** A commit costs what it changed, not the size of the
   cartridge, and not the size of a big entry it changed a little of.
5. **Reproducible.** The content digest depends on entries and their
   metadata, never on compression or layout (§9.2), and one writer given
   the same inputs and settings writes the same bytes (§3.10).

Non-goals: concurrent writers (one writer at a time, §10.2), byte-level
in-place writes (a change to an entry replaces whole chunks, §4.5),
reading from a stream, and network resolution of anything.

## 2. Terminology

RFC 2119 key words. **Cartridge**, **reader**, **writer**, **payload**
and **kind** as in 2.0 §2, and:

- **Entry**: one named item in a cartridge: a path, metadata, and content
  (or a tombstone, §3.5).
- **Commit**: one append of data, an index and a commit record (§3.4).
  A cartridge is the state named by its latest valid commit.
- **Live entry**: an entry in the current state, after deltas and
  tombstones are applied.
- **Chunk**: one 1 MiB piece of a chunked entry's content (§4.5).
- **Compaction**: rewriting a cartridge as a single commit holding only
  live entries (§10.4).
- **The Drive**: the Virtual Cartridge Drive, a separate tool that gives
  applications a mutable view of a cartridge (§10.5).

## 3. Container format

All integers are little-endian. Offsets and sizes are unsigned 64-bit,
counted in bytes from the start of the file. Appendix A gives every
structure byte by byte.

Every sum and product a reader forms from values in the file (an offset
plus a size, a count times a record size) MUST be checked for overflow
before it is compared or used.

### 3.1 Layout

```text
offset 0      header page 0 (4096 bytes): magic, version, superblock slot A
4096          header page 1 (4096 bytes): superblock slot B
8192          commit 1: data blocks, index, string table, commit record
...           commit 2: the same, for what commit 2 changed
...           commit n
```

A cartridge grows only at its end, except for the superblock slots,
which a commit rewrites last (§10.3). The two slots are on separate
4096-byte pages, so that a torn write of one page, the unit storage
writes, can damage at most one slot.

### 3.2 Header

The first 8192 bytes. Page 0 begins with the magic
`89 53 51 43 0D 0A 1A 0A` (`\x89SQC\r\n\x1a\n`). Like PNG's, it catches
a file mangled by a text-mode transfer, and it is never valid UTF-8. The
magic is followed by the format's major and minor version (3, 0), the
header size, and **superblock slot A**. Page 1 holds **superblock slot
B**. Each slot names a commit: its sequence number, its record's offset
and length, the file length when it was made, and a checksum over the
slot.

Nothing else in the header ever changes after the file is created.
Feature flags live in each commit record (§3.9), not in the header, so
that setting one is part of a commit and as crash-safe as the rest of
it.

### 3.3 Opening a cartridge

A 3.0 cartridge needs random access: the reader starts at the header and
then reads wherever the commit points. A reader MUST be given a file or
a buffer it can seek in; a tool that receives a cartridge as a stream
(a pipe, a download in progress) stores it whole first.

A reader:

1. checks the magic at offset 0 and the major version (it MUST refuse a
   major version it does not know, and MUST open any minor version of
   one it knows). A file whose first bytes are the 3.0 magic is a 3.0
   cartridge and nothing else: a reader MUST NOT also try to read it as
   ZIP or as any other format (§13);
2. reads both superblock slots and keeps the ones whose checksum is
   correct, whose sequence number is not zero, and whose commit record
   lies inside the file;
3. takes the slot with the higher sequence number, and reads and checks
   that commit record (§3.4); if it is damaged (its magic, checksum or
   bounds fail), it falls back to the other slot. A commit that is intact
   but that the reader cannot or may not read (a required feature it
   lacks, a limit of §3.8) is refused as it is, never passed over: the
   older commit is a state the cartridge has replaced;
4. refuses the cartridge when neither slot names a valid commit.

The slot the reader settles on is the **current slot**; the other is
the **spare slot**. Bytes after the end of the current commit are
**trailing bytes**: a commit that did not finish, or something appended
by another tool. A reader MUST ignore them and a validator MUST report
them (§12); a writer MUST truncate them before appending (§10.3). They
are covered by no checksum, digest or signature (§9.4).

### 3.4 Commits

A commit is written in this order:

1. its data blocks: the content of every entry, or every chunk, that it
   adds or replaces (§3.6);
2. its index: one record for every entry it adds, replaces or deletes
   (§3.5);
3. its string table (§3.7);
4. its chunk tables, if it writes chunked entries (§4.5);
5. its commit record: sequence number, the previous commit's offset,
   where its data, index, strings and chunk tables are, whether its index
   is full or a delta, the number of live entries, its feature flags
   (§3.9), the content digest (§9.2), an optional signature block
   (§9.3), and a SHA-256 over the record.

Only then is the spare superblock slot overwritten to name it (§10.3).

A reader checks a commit record before using it:

- its magic and its SHA-256;
- its sequence number is greater than its previous commit's, and its
  previous commit record lies before its own data;
- every region it names (data, index, strings, chunk tables, signature
  block) lies after the previous commit record's end and before its own
  record, and no two regions overlap;
- its delta chain length is 0 for a full index and the previous
  commit's plus one for a delta, and is at most the limit (§3.8).

A reader that walks the chain of previous commits (§3.5) applies the
same checks to each, and so can neither loop nor run off the file.

### 3.5 The index: full and delta

An index is an array of fixed-size **entry records** (128 bytes,
Appendix A.3), sorted by path: byte-wise ascending over the UTF-8 bytes.
Because records are fixed-size and sorted, a reader finds a path by
binary search over the mapped array, reading strings only to compare.

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

An entry's content, or a chunk's (§4.5), is one contiguous **run** of
bytes, compressed or stored as its codec says (§4.3). A run starts on a
64-byte boundary. An entry with the `mappable` flag is stored, not
chunked, and starts on a **16384-byte** boundary, so that a reader can
map it straight into memory (a font, a texture, an audio buffer) on a
system with 4 KiB or 16 KiB pages. Padding between runs is zero bytes
and belongs to no entry.

A record or a chunk table MAY name a run written by an earlier commit:
renaming an entry, changing only its metadata, or changing some chunks
of a big entry then costs no copy of the unchanged content. A run MUST
lie inside the data region of its own commit or of an earlier one, never
in an index, string table, chunk table or commit record, and never after
its own commit. Two references to stored content either name exactly the
same run (the same offset and stored size) or runs that do not overlap.
A reader MUST refuse partial overlaps: an archive whose entries overlap
is the "better zip bomb" construction (red-team R10).

Content belongs to the commit that wrote it. A replaced or deleted
entry's old content stays in the file, unreachable, until compaction.

### 3.7 String table

Paths, titles, authors, media types and attribute objects are stored
in a commit's string table: each string is a 32-bit length and its
UTF-8 bytes, with no terminator. An entry record names a string by its
offset in the table. Offset `0xFFFFFFFF` means "none". A string's
length and bytes MUST lie inside the table.

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
| Unchunked entry size | 64 MiB | anything larger is chunked (§4.5) |
| Chunk size | 1 MiB | fixed (§4.5) |
| zstd window | 2^27 bytes (128 MiB) | readers MUST support up to 2^23; a writer MUST NOT exceed the window its profile allows (§4.3) |
| Delta chain | 32 | §3.5 |
| JSON nesting | 64 levels | the manifest, `entries.json`, attributes and record entries (§5) |
| Signatures in one block | 16 | §9.3 |

A reader MUST refuse a cartridge whose index, strings, chunk tables or
data reach outside the file or outside the regions of §3.4 and §3.6,
before using them.

### 3.9 Feature flags

Each commit record carries 64 feature bits for the state it names. A
bit in the low 32 is **required**: a reader that does not implement the
feature MUST refuse the cartridge. A bit in the high 32 is
**optional**: a reader MAY ignore it. 3.0 defines:

| Bit | Name | Set when |
|---|---|---|
| 0 | `zstd-dictionaries` | some live entry or chunk uses codec 2 (§4.3) |
| 1 | `chunked` | some live entry uses codec 3 (§4.5) |
| 2 | `zstd-window-27` | some frame needs a window larger than 2^23 (§4.3) |

Every other bit is reserved and MUST be zero. Compaction recomputes the
bits from the live entries.

This replaces 2.0's `requires_features` for container features. The
envelope member stays for features of the payload (2.0 §5.4).

### 3.10 Reproducible output

A writer given the same entries, metadata, settings and commit time
SHOULD write the same bytes. To make that possible this specification
fixes the order of everything a writer lays out: runs in index order,
then chunks in content order; strings in order of first use while
writing the index; chunk tables in index order. A writer takes the
commit time from `SOURCE_DATE_EPOCH` when it is set, and zstd settings
that change its output (level, window, long-distance matching,
dictionary) from the profile, not from the machine (thread count, free
memory).

## 4. Entries

### 4.1 Paths

Paths follow 2.0 §3.3 unchanged: relative, `/`-separated, no `.` or
`..` segment, no empty segment, no NUL, backslash, `:` or control
character, at most 1024 bytes, UTF-8 in Normalization Form C.

Logical paths are case-sensitive, and, as in 2.0, a cartridge MUST NOT
hold two live entries whose paths would collide on a case-insensitive
file system. 3.0 widens 2.0's ASCII-only rule: two paths collide when
they are equal after Unicode full case folding (Unicode `CaseFolding.txt`,
status C and F) and NFC. Android's shared storage, exFAT, FAT, macOS
and Windows all fold case, and fold more than ASCII.

A writer SHOULD warn about, and `sqcart unpack` MUST refuse, a path that
cannot be created as a file on the target system: on Windows, a segment
named `CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9` or `LPT1`–`LPT9` (with
or without an extension), or ending in a dot or a space.

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
| stored size | uint64 | bytes of the run, or of the chunk table for codec 3 |
| offset | uint64 | where the run or the chunk table starts (§3.6, §4.5) |
| dictionary | uint32 | for codec 2, the zstd dictionary ID (§4.3); otherwise 0 |
| digest | 32 bytes | the entry's tree hash (§9.1) |
| attributes | string or none | a JSON object of further metadata, defined by consumers |

Titles, authors and attribute values are text meant for people, and a
tool that shows them MUST NOT let them deceive: it shows control
characters and Unicode bidirectional controls (U+202A–U+202E,
U+2066–U+2069) visibly, or strips them, rather than letting them reorder
or hide what is around them (red-team R18). A writer SHOULD refuse
titles and authors containing control characters.

**Flags.**

| Bit | Name | Meaning |
|---|---|---|
| 0 | `r` | readable: offered to the runtime and to consumers. An entry without `r` is kept for tools only (sources, build notes) and a runtime reader MUST NOT hand it to application code |
| 1 | `w` | writable: the Drive and other writers may replace or delete it. A writer MUST refuse to change an entry without `w` unless it is rebuilding the cartridge from its sources |
| 2 | `x` | executable: a runtime may run it (Lua source, 2.0 §7). It never makes a file executable on disk (2.0 §3.1, kept), and native code stays forbidden (2.0 §8) |
| 3 | `deleted` | a tombstone (§3.5) |
| 4 | `mappable` | stored, 16384-aligned, for direct mapping (§3.6) |
| 5 | `dictionary` | this entry is a zstd dictionary (§4.3); it carries no `r` |
| 6–15 | | reserved, zero |

The flags describe intent, not access control: a cartridge is a file,
and anyone with the file can read every byte of it. They let readers,
the Drive and tools agree on what an entry is for. `r`, `w` and `x` are
covered by the content digest (§9.2), so changing one is a change a
signature notices.

`created` and `updated` are metadata like a title, not file times: a
writer copies them from where the entry came from, or sets them from a
caller-supplied epoch for reproducible builds. They are covered by the
content digest (§9.2).

A tombstone has no title, author, media type or attributes, codec 0,
offset, sizes and dictionary of 0, and a digest of zero bytes. An entry
with no content (size 0) has codec 0, offset 0 and stored size 0.

### 4.3 Codecs and compression

| Codec | Name | Content |
|---|---|---|
| 0 | stored | the bytes as they are |
| 1 | zstd | one zstd frame |
| 2 | zstd-dict | one zstd frame compressed with a dictionary; `dictionary` holds its ID |
| 3 | chunked | a chunk table; the content is in chunks, each stored, zstd or zstd-dict (§4.5) |

Deflate is gone. zstd decompresses several times faster than Deflate and
compresses smaller at its high levels, and squared already carries a
zstd decoder (sqterm's packages).

**Frames.** A run of codec 1 or 2 is exactly one zstd frame (RFC 8878)
and nothing else: no skippable frames, no second frame, no bytes after
it. The frame header MUST give the content size, and it MUST equal the
entry's or the chunk's size. A reader checks that before it allocates,
sets the decoder's window limit (`ZSTD_d_windowLogMax`) to what it
supports, and stops decoding at the declared size: a frame header can
lie, and an untrusted size must never drive an unbounded allocation
(red-team R5). A stored run's stored size equals its size.

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
larger one. A cartridge with a larger window sets the required feature
bit `zstd-window-27` (§3.9), so a phone-sized reader can refuse it
cleanly before it decodes anything. Chunks are 1 MiB (§4.5), so a
window larger than that gains a chunked entry nothing.

**Dictionaries.** Many small, similar entries (thousands of JSON records,
a manual's pages) compress poorly one at a time. A writer MAY train a
zstd dictionary on them and compress those entries with codec 2.

- A dictionary is an entry at `SQ-INF/dictionaries/<id>.zdict`, where
  `<id>` is its zstd dictionary ID in decimal, with the `dictionary` flag
  and without `r`. A dictionary entry is stored or zstd, never codec 2.
- A codec 2 frame MUST carry its dictionary ID in its header, equal to
  the entry's `dictionary` field. A reader finds the dictionary by path,
  in the same commit chain (§3.5), and checks that the ID in the
  dictionary's own header matches.
- A dictionary is immutable. A writer MUST NOT replace or delete a
  dictionary while any live entry or chunk uses it; a new dictionary
  takes a new ID. Compaction drops dictionaries no live entry uses.
- Dictionary IDs from 1 to 32767 are reserved by zstd for registered
  dictionaries; a writer picks an ID of 32768 or more, unused in the
  cartridge, and SHOULD derive it from the dictionary's content so that
  rebuilding gives the same ID.

A reader loads a dictionary once and keeps it while the cartridge is
open.

### 4.4 Images and other binary data

Binary entries need nothing special. A PNG is an entry with media type
`image/png`, codec 0, and, if a renderer will map it, the `mappable`
flag. A title and attributes describe it (`{"width": 256, "height":
256}`), so a tool can list a cartridge's images without decoding one.

### 4.5 Chunked entries

A big entry (a video, a disk image, a database, a world file of many
gigabytes) is read a piece at a time, and changed a piece at a time. One
zstd frame cannot be read from the middle, and one digest over the
whole content cannot be checked until the whole content has been read.
So an entry larger than 64 MiB MUST, and any entry MAY, be **chunked**
(codec 3):

- Its content is cut into chunks of exactly 1 MiB (2^20 bytes), the
  last one shorter. Chunking is fixed-size and starts at offset 0, so
  chunk *i* always holds bytes *i* × 2^20 onwards.
- Each chunk is its own run (§3.6), with its own codec: 0, 1 or 2.
- The entry record's offset and stored size name its **chunk table**,
  in the chunk-table region (§3.4) of its own commit or, when only its
  metadata changed, of an earlier one: one 64-byte record
  per chunk (Appendix A.6) giving the chunk's run, stored size, size,
  codec, dictionary ID and SHA-256.

To read bytes *a* to *b*, a reader decompresses only the chunks that
hold them, and verifies each chunk against its SHA-256 before handing
out any of its bytes. The chunk hashes are the leaves of the entry's
tree hash (§9.1), so a reader that has checked the chunk table against
the entry's digest has verified every byte it reads, without reading
the rest of the entry.

To change bytes *a* to *b*, a writer writes new runs for the chunks
that hold them, and a new chunk table that names the old runs for every
other chunk (§3.6). A commit changing one byte of a 10 GiB entry
therefore writes one chunk and a 640 KiB table, not 10 GiB. Inserting
or removing bytes moves every later chunk's content, and rewrites them:
fixed-size chunks favour content that is edited in place (images,
databases, disk images), which is what the Drive serves.

A mappable entry is never chunked; it is stored whole and verified whole
when mapped (§9.1).

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
- Records, like every JSON text in a cartridge (the manifest,
  `entries.json`, attributes), are I-JSON (RFC 7493): UTF-8 with no byte
  order mark, no duplicate member names in one object, no number a
  64-bit IEEE double cannot hold exactly (an integer beyond ±2^53 is
  written as a string), and nesting of at most 64 levels (§3.8). A
  reader MUST refuse JSON that breaks these rules rather than pick one
  interpretation of it: two parsers that read a duplicate member
  differently can be made to disagree about what a cartridge says.
- A consumer says which records it reads, and what they mean, in its own
  section: `"consumers": { "my_game": { "records": { "world/settlements/": "settlement@1" } } }`
  (a path prefix and a schema name of the consumer's choosing).
- **Granularity is a choice with a cost.** The Drive replaces entries,
  or chunks of them (§4.5), whole. One record entry holding 100,000
  objects is small and fast to read, and every change to one object
  rewrites it; one entry per object makes every change cheap, and a
  dictionary (§4.3) keeps 100,000 small entries small. A consumer whose
  data changes at run time SHOULD split it so that one change touches
  one small entry.

## 6. Exploded form, and workspaces

### 6.1 Exploded form

A folder holding `SQ-INF/manifest.json` is a cartridge (2.0 §3.5, kept).
Its entries are the folder's regular files; there is no index, no codec
and no commit, since the file system holds them. These are never
entries:

- symbolic links, devices, sockets and anything else that is not a
  regular file or a folder. `sqcart pack` MUST refuse a symbolic link
  rather than follow it, and MUST NOT read a file through a path whose
  folders include one: a link inside the folder would otherwise pack a
  file from outside it (red-team R12);
- the top-level `.squared/` and `.git/` folders (§6.2).

Entry metadata that a file system cannot hold goes in an optional
`SQ-INF/entries.json`: a JSON object from path to the metadata of §4.2,
by name (`title`, `author`, `media_type`, `created`, `updated`,
`attributes`, `flags`), with `flags` written as a string of letters: `r`,
`w`, `x`, and `m` (mappable) and `d` (dictionary), so that every flag a
packed entry can carry survives a round trip (`"rw"`, `"rx"`, `"rm"`). A
member left out has its default (flags `r`, times 0, the rest none);
`sqcart unpack` writes only what differs from those defaults, in RFC 8785
canonical form, so the same cartridge always unpacks to the same bytes:

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
cartridge differ. A record for a path that is not a file in the folder
(left behind when a file was renamed by another program) is an
**orphan**: a reader ignores it, a validator reports it, and
`sqcart pack` refuses until it is fixed or `--drop-orphans` is given.

Paths in an exploded cartridge are read from the file system, so they
follow its rules: on a case-insensitive file system two entries cannot
collide, and on macOS names come back decomposed (NFD). `sqcart pack`
converts every path to NFC and then applies §4.1.

Converting between the forms (`sqcart pack`, `sqcart unpack`) carries
every field across, so an archive packed from a folder and unpacked
again gives the same folder. `sqcart unpack`:

- refuses a target folder that is not empty, unless told to merge;
- creates every file itself, with `O_CREAT | O_EXCL` and without
  following symbolic links, so an archive cannot write through a link
  planted in the target (the Zip Slip family, red-team R12);
- refuses the whole cartridge, before writing anything, when a path
  would collide on the target file system (§4.1).

An exploded cartridge carries no signature: a signature lives in a
commit record (§9.3). A signed cartridge unpacked and packed again has
the same content digest, so its signatures can be copied onto the new
commit (`sqcart pack --signatures-from <file>`) and still verify.

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
  `.squared/`, which is never an entry (§6.1). Neither is `.git/`. Build
  output is the workspace tooling's to keep out of the tree, or out of
  the archive with its own exclusions.

An archived workspace is a snapshot: a backup, or a workspace shared as
one file. It is valid, and the Drive (§10.5) can work on it like any
other cartridge.

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
- The manifest is I-JSON (§5).

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
| `version` | string | a range, in the grammar below |
| `kind` | string, optional | the kind it must have |
| `optional` | boolean, optional | default false |

**Ranges.** npm and Cargo read the same range differently (a bare
`1.2.3` is exact in npm and `^1.2.3` in Cargo; they treat pre-releases
differently), so 3.0 fixes one grammar and its meaning:

- `1.2.3`: exactly that version. `=1.2.3` is the same.
- `^1.2.3`: `>=1.2.3 <2.0.0`; `^0.2.3`: `>=0.2.3 <0.3.0`; `^0.0.3`:
  `>=0.0.3 <0.0.4`.
- `~1.2.3`: `>=1.2.3 <1.3.0`.
- `>`, `>=`, `<`, `<=` followed by a full version; comparators separated
  by single spaces must all hold.
- No `||`, no `x` or `*` wildcards, no partial versions, no build
  metadata.
- Versions compare as SemVer 2.0.0 §11. A pre-release version satisfies
  a range only when the range itself names a pre-release of the same
  major, minor and patch (as npm does): `^1.2.3` never selects
  `1.3.0-beta.1`.

A reader validates the shape and the grammar, and nothing more: it MUST
NOT resolve, download or open anything because of `requires`.
Resolution belongs to consumers (square and pkg, against the cartridges
installed on the machine), offline as everything else. A consumer that
cannot satisfy a requirement that is not optional MUST say which and
why, and MUST NOT proceed as if it had.

Dependencies are on identity and version, never on a path or a file
name (2.0 §1.2, kept).

## 9. Integrity and signatures

### 9.1 Entry digests

Every entry record holds a **digest** of the entry's uncompressed
content, a SHA-256 tree hash with 1 MiB leaves:

- an entry of at most 1 MiB: its digest is the SHA-256 of its content,
  the same value `sha256sum` prints;
- a larger entry: cut the content into 1 MiB leaves (the last shorter),
  hash each with SHA-256, and take the SHA-256 of the string
  `squared-tree/1`, a zero byte, the content's size as a little-endian
  u64, and the leaf hashes in order.

The digest depends on the content alone, never on whether the entry is
chunked or how its chunks are compressed. A chunked entry's chunk
hashes are its leaves (§4.5).

A reader MUST NOT hand out bytes of an entry it has not verified:

- an unchunked entry is verified whole, on its first read, before any
  of it is returned; a mappable entry, when it is mapped;
- a chunked entry's chunk table is verified against the digest when the
  entry is opened, and each chunk against its hash when it is read.

A mismatch is an error for that entry, never a silently accepted read.
A reader MAY skip verification only when the caller asks it to, and
MUST then not report the data as verified.

Verification proves what was read at the time it was read. A cartridge
on storage other programs can write (Android's shared storage, a
removable card) can change after it was checked, under a mapping or
between reads. A runtime that relies on a cartridge being unchanged
copies it to storage only it can write (app-private storage) and
verifies the copy, or verifies every read.

### 9.2 Content digest

Each commit record holds the **content digest** of the cartridge's state
at that commit: the SHA-256 of

1. the string `squared-cartridge/3 state`, a zero byte, and the live
   entry count as a u64;
2. for every live entry, sorted by path as the index is sorted: its
   path, its 32-byte digest (§9.1), and its metadata, each as a u32
   length followed by its bytes.

`<metadata>` is a JSON object of the entry's metadata in the canonical
form of RFC 8785 (JSON Canonicalization Scheme), which orders members by
name:

| Member | Value | Present |
|---|---|---|
| `attributes` | the attributes object | when the entry has one |
| `author` | string | when the entry has one |
| `created` | number (ms) | always |
| `flags` | the `r`, `w` and `x` flags as letters, in that order (`"rw"`; `""` for none) | always |
| `media_type` | string | when the entry has one |
| `title` | string | when the entry has one |
| `updated` | number (ms) | always |

JCS writes every number as a double, so `created` and `updated` MUST lie
within ±(2^53 − 1); a validator reports one that does not. The
attributes object is canonicalised too: a writer MUST store attributes
in canonical form, and a validator MUST report attributes that are not,
so that every implementation, and every round trip through
`entries.json`, gives the same bytes.

As in 2.0, the digest depends on content and metadata, never on
compression, codec, chunking, alignment, dictionaries or layout. A
cartridge compacted, recompressed at another level, or unpacked and
packed again has the same digest. Adding, removing or changing any
entry or any of its metadata changes it.

`offset`, `size`, `stored size`, `codec`, `dictionary`, the chunk table
and the `mappable` and `dictionary` flags are storage, not content, and
are left out. So are the commit time and the feature flags.

### 9.3 Signatures

A commit record MAY point to a **signature block**: a list of signatures
over that commit's content digest.

- Algorithm: each signature names its algorithm (Appendix A.5). 3.0
  defines one: 1, Ed25519 (RFC 8032), verified as RFC 8032 §5.1.7
  says, rejecting a non-canonical S or encoding. A reader MUST refuse an
  algorithm it does not know rather than treat it as a valid signature,
  and MUST report it rather than treat it as no signature at all.
- Signed message: the ASCII string `squared-cartridge/3 signature`, a
  zero byte, the algorithm number as a u16, the manifest's `id`, a zero
  byte, its `version`, a zero byte, the signer name (or nothing), a zero
  byte, and the 32-byte content digest.
- Each signature: the algorithm, the public key, a key identifier (the
  first 8 bytes of the SHA-256 of the public key), an optional signer
  name, and the signature. The key identifier is a hint for finding the
  key; a reader compares the full public key with the one it trusts.

The signer name is signed, so it cannot be changed without breaking the
signature. It is still the signer's claim about themselves: a reader
shows it only with the key that made it, never as an identity on its own.

A signature covers a state, not a commit history. A later commit that
changes anything has a different digest and carries no signature until
someone signs it again. A reader MUST report a cartridge whose latest
commit is unsigned as unsigned, even when an earlier commit was signed.
Compaction copies the signature block, which still verifies (§10.4).

This specification defines how to verify a signature, not whom to
trust. Trust policy belongs to consumers: pkg's list of trusted keys, a
launcher's, square's. A reader MUST NOT report a cartridge as trusted,
only as signed by a key, or unsigned.

### 9.4 What a signature does not cover

A consumer's trust policy needs to know the limits:

- **Rollback.** An old cartridge, or an old state of this one (a file
  cut back to an earlier commit, with the newer slot cleared), still
  carries valid signatures for what it was. A consumer that installs or
  updates MUST compare `id` and `version` with what it already has and
  refuse to go backwards unless the user asks.
- **Bytes outside the state.** Trailing bytes (§3.3), old commits,
  padding, reserved fields and the header are not covered. A validator
  reports trailing bytes and non-zero padding or reserved fields, and a
  distributed cartridge SHOULD be compacted, so that it holds nothing but
  its state. Nothing outside the state is ever given meaning (§13).
- **Storage choices.** Codec, chunking and layout are unsigned. They
  cannot change what a reader returns, because every byte returned is
  checked against a digest that is signed, but a hostile cartridge can
  still cost a reader time and memory up to the limits of §3.8.

## 10. Changing a cartridge, and the Virtual Cartridge Drive

### 10.1 Who does what

sqcart defines what a valid cartridge is, and **sqcart is the only
writer**: its library provides every operation that changes a cartridge
file, so that every cartridge on disk was written by code that keeps
this section. These are normative:

- open for writing, which takes the lock (§10.2) and truncates trailing
  bytes;
- put an entry, put a range of a chunked entry (§4.5), delete an entry,
  set an entry's metadata;
- commit (§10.3), which is the only way a change becomes visible;
- compact (§10.4).

The Virtual Cartridge Drive is a separate tool, a consumer of
cartridges, built on those operations. It owns everything that is
policy rather than format: staging, coalescing, caching, scheduling,
backpressure, and recovery of staged changes (§10.5). Neither the Drive
nor any other tool writes cartridge bytes itself.

### 10.2 One writer

Only one process writes a cartridge at a time. A writer MUST hold an
exclusive lock on a separate lock file, `<file>.lock`, created beside the
cartridge, for as long as it may append. It MUST NOT lock the cartridge
file itself: compaction renames a new file over it (§10.4), and a
second writer waiting on a lock held on the old file would get that
lock, then append to a file no longer in the folder, and lose its
commit.

Where the system offers `flock` or `fcntl` locks on the lock file, the
writer uses them, and the lock dies with the process. Where it does not
(some FUSE and network file systems), the writer instead creates
`<file>.lock.owner` exclusively (`O_CREAT | O_EXCL`, atomic where `flock`
is not), writes the writer's host name, process ID and process start
time into it, and deletes it when it is done. A writer MAY take over an
owner file only when its process is gone from that host (it checks the
start time too, so a later process given the same ID is not taken for
it), and MUST otherwise refuse: an owner file from another host is never
taken over.

Readers need no lock: they see the commit a superblock named when they
opened the file, and a commit appended later does not disturb them,
because nothing a commit record reaches is ever overwritten.

### 10.3 Committing

1. Truncate the file to the end of the current commit, discarding
   trailing bytes (§3.3).
2. Append the commit's data blocks, index, string table and chunk
   tables, then its commit record (§3.4). Its sequence number is one
   more than the higher of the two slots' sequence numbers, whether or
   not that slot was valid, so that no sequence number is used twice.
3. Flush the file to storage.
4. Write the spare slot to name the new commit.
5. Flush again.

"Flush" means the call that reaches the storage itself, not only the
operating system's cache: `fsync` on Linux and Android, `fcntl(F_FULLFSYNC)`
on macOS (whose `fsync` does not flush the drive's cache), and
`FlushFileBuffers` on Windows. When a commit creates the file, the
writer also flushes the folder it is in, so the file's name survives.

A crash before step 4 completes leaves the current slot in charge, and
the appended bytes are trailing bytes. A crash during step 4 can damage
only the spare slot's page (§3.1); its checksum fails, and the current
slot is still in charge. Only after step 5 is the commit durable, and a
writer MUST NOT report a commit as done before then.

Storage that does not honour flushes (some USB and removable media,
some FUSE layers) can lose a commit that was reported durable, though
§3.3's fallback still opens the last commit whose bytes reached it.
Writers SHOULD keep cartridges they change on internal storage (§10.6).

### 10.4 Compaction

Compaction writes a new file holding only the live entries, as a single
commit with a full index, to a temporary name in the same folder:
flushes it, checks it (§12), renames it over the old file, then flushes
the folder, without which the rename itself may not survive a crash.
Its content digest is unchanged (§9.2), so the signature block is copied
and still verifies. Compaction recomputes the feature flags (§3.9) and
drops unused dictionaries (§4.3).

Readers with the old file open keep reading it: on POSIX systems the old
file lives until they close it. On Windows a file cannot be renamed over
while it is open without `FILE_SHARE_DELETE`; there a reader opens
cartridges with that sharing mode, and a compaction that still fails
leaves the old file in place and reports it.

A writer SHOULD compact when unreachable data exceeds half the file, or
when the delta chain nears its limit (§3.5). Compaction is also the only
time a cartridge shrinks.

### 10.5 The Drive (informative)

The Drive is a high-level interface for squared applications to work
with big files, whose backing store is a cartridge. It is its own tool
with its own design document; this section records only what it
expects of sqcart:

- **Reads** resolve against staged changes first, then the cartridge,
  through sqcart's reader (§3.5 lookups, §4.5 ranges).
- **Writes** go to a staging area the Drive owns, so memory stays
  bounded however much is pending. A write to an entry without `w` is
  refused (§4.2). A write inside a big file is staged as the chunks it
  touches, and committed with sqcart's range put (§4.5), so a small
  change to a big file costs a small commit.
- **Coalescing**: an entry or chunk changed many times before a commit
  is written once.
- **Renames and cuts** write records, not content: a renamed entry's new
  record names the same runs, and a cut chunked entry names again every
  chunk below the cut (§3.6).
- **Commits** are sqcart commits (§10.3), triggered by the application's
  own choice: on request, at a loading screen, past a staging-size or
  time budget, at a clean shutdown.
- **Backpressure**: when staging grows faster than commits drain it, the
  Drive commits before accepting more, rather than growing without
  bound.
- **Recovery**: staged changes not yet committed are the Drive's own to
  recover or discard after a crash; the cartridge itself is always at a
  valid commit.
- **Where staging lives** is the Drive's choice, but a cartridge may sit
  where the Drive cannot write beside it (inside an APK, on read-only
  storage), so staging belongs in the Drive's own storage, keyed by the
  cartridge's `id` and file, not in a `<file>.vcd/` folder beside it.

### 10.6 Android storage

A cartridge being changed SHOULD live in app-private storage
(`/data/data/<app>/` or the app's own folder under `Android/data/`):

- shared storage (`/sdcard`, `/storage/emulated/0`) is a FUSE file
  system that other apps with storage permission can write, is slower
  than private storage, folds case, and may not support `flock` (§10.2);
- a removable card may be FAT32, which cannot hold a file of 4 GiB or
  more. A writer that would grow a cartridge past 4 GiB − 1 on such a
  file system MUST refuse the commit rather than corrupt the file, and
  SHOULD say why.

Reading a cartridge from shared storage is fine, within §9.1's caution
about files other programs can change. A cartridge inside an APK is
read in place when the APK stores it uncompressed and 16384-aligned;
apkpack does both for `.sq` assets.

## 11. Code in cartridges

2.0 §7 (Lua) and §8 (native code) carry over unchanged. The `x` flag
(§4.2) marks what a runtime may run, and does not relax §8: a cartridge
holding loadable native code is still non-conforming.

A runtime that runs code from a cartridge loads it only through a reader
that verifies it (§9.1), and only from the cartridge it verified. Electron
learned this the hard way: its archive integrity check could be bypassed
while the app would still load code from an unchecked folder beside the
archive (CVE-2024-46992; Electron's `onlyLoadAppFromAsar`). A runtime
that also accepts the exploded form MUST NOT fall back to an exploded
folder when a packed cartridge fails verification.

## 12. Validation order

A reader validates what it uses before it uses it, and stops at the first
failure. Opening a cartridge costs what §1 promises (a fixed header and
the commit records), not the size of its index, so validation happens in
three places:

1. **At open.** The header: magic, major version (§3.2). The superblocks,
   the chosen commit record, its required feature bits, its regions'
   bounds and the commit chain down to the full index (§3.3, §3.4, §3.9).
   The manifest: a live `SQ-INF/manifest.json`, then the envelope as in
   2.0 §12.1, with `format_version` 3, as I-JSON (§7).
2. **When an entry is used.** Its record, before the reader returns it:
   its path (§4.1), flags, codec, sizes and strings (§4.2, §4.3, §3.8), and
   that its run or chunk table lies in a data or chunk-table region of its
   own commit or an earlier one (§3.6: the reader walks every commit's
   record, once, to know them). Its content against its digest, as it is
   read (§9.1).
3. **When the whole state is used.** Listing the live entries, computing
   the content digest, verifying signatures over it: every index's order
   and uniqueness (§3.5), every record as in 2, the live count, case
   collisions (§4.1) and the features the state uses (§3.9).

A binary search over an index that is not sorted can miss a path, so a
reader that finds nothing where an entry should be cannot tell a broken
index from an absent entry until 3. Nothing a reader returns is ever
unchecked: a missed entry is reported as absent, never as something else.
Signatures (§9.3) are verified when the caller asks, over the state that
3 has checked.

A validator (`sqcart check`) does all three for every entry, and
also reports what a reader may ignore: trailing bytes, non-zero padding
or reserved fields, attributes not in canonical form, unused
dictionaries, and, for an exploded cartridge, orphans in
`entries.json`.

## 13. From 2.0

- `sqcart convert` reads a 2.0 cartridge (ZIP or exploded) and writes a
  3.0 one: the same entries, `format_version` 3, created and updated of
  0, flags `r` (and `x` on Lua source), media types guessed from
  extensions, `SQ-INF/hashes.json` dropped. A `kind` of `cartridge`
  becomes `application`.
- A 3.0 reader MAY read 2.0 ZIP cartridges too, to ease the move, and
  MUST report them as 2.0. It decides which format a file is by its
  first bytes alone: the 3.0 magic means 3.0, a ZIP local file header
  means 2.0. A ZIP is found from its end, so one file can be both a valid
  3.0 cartridge and a valid ZIP; a reader that tried both could be shown
  two different cartridges in one file, as Android's Janus flaw
  (CVE-2017-13156) showed one file that was both an APK and a DEX.
- A 3.0 writer writes 3.0 only.
- squared-pg workspaces move from `.squared/metadata.json` to
  `SQ-INF/manifest.json` once, through square (§6.2).
- sqterm's packages (`.tar.zst` with an `SQ-INF/manifest.json` inside)
  can become 3.0 cartridges, read by pkg through sqcart's reader. That
  is a separate decision, recorded in Appendix B.2.

## Appendix A: byte layouts

All little-endian. `u16`, `u32`, `u64` unsigned; `i64` signed.

### A.1 Header (8192 bytes at offset 0)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `89 53 51 43 0D 0A 1A 0A` |
| 8 | 2 | major version: 3 |
| 10 | 2 | minor version: 0 |
| 12 | 4 | header size: 8192 |
| 16 | 48 | reserved, zero |
| 64 | 64 | superblock slot A |
| 128 | 3968 | reserved, zero |
| 4096 | 64 | superblock slot B |
| 4160 | 4032 | reserved, zero |

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
| 24 | 4 | dictionary ID (codec 2), else 0 |
| 28 | 4 | reserved, zero |
| 32 | 8 | offset: the run, or the chunk table (codec 3) |
| 40 | 8 | stored size: of the run, or of the chunk table |
| 48 | 8 | size (uncompressed) |
| 56 | 8 | created (i64, ms since the epoch, UTC) |
| 64 | 8 | updated (i64) |
| 72 | 32 | digest (§9.1) |
| 104 | 24 | reserved, zero |

### A.4 Commit record (224 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `SQCOMMIT` |
| 8 | 8 | sequence number |
| 16 | 8 | previous commit record offset (0 for the first) |
| 24 | 8 | data region offset |
| 32 | 8 | data region length |
| 40 | 8 | index offset |
| 48 | 8 | index entry count |
| 56 | 8 | string table offset |
| 64 | 8 | string table length |
| 72 | 8 | chunk table region offset |
| 80 | 8 | chunk table region length |
| 88 | 4 | index kind: 0 full, 1 delta |
| 92 | 4 | delta chain length (0 for a full index) |
| 96 | 8 | live entry count |
| 104 | 8 | commit time (i64, ms since the epoch; metadata only) |
| 112 | 8 | feature flags (§3.9) |
| 120 | 32 | content digest (§9.2) |
| 152 | 8 | signature block offset, or 0 |
| 160 | 8 | signature block length |
| 168 | 24 | reserved, zero |
| 192 | 32 | SHA-256 over bytes 0–191 |

### A.5 Signature block

A `u32` count (at most 16), then per signature: `u16` algorithm (1:
Ed25519), `u16` public key length (32 for Ed25519), the public key,
8-byte key identifier, `u32` signer-name length and that many UTF-8
bytes, `u16` signature length (64 for Ed25519), the signature.

### A.6 Chunk record (64 bytes)

A chunk table is one record per chunk, in content order. Its stored
size is 64 × ⌈size / 2^20⌉.

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | run offset |
| 8 | 4 | stored size |
| 12 | 4 | size (2^20, or less for the last chunk) |
| 16 | 2 | codec: 0, 1 or 2 |
| 18 | 2 | reserved, zero |
| 20 | 4 | dictionary ID (codec 2), else 0 |
| 24 | 8 | reserved, zero |
| 32 | 32 | SHA-256 of the chunk's uncompressed bytes |

## Appendix B: decisions

### B.1 Decided (2026-11-18)

1. **SHA-256, not BLAKE3.** Every digest in this format is SHA-256: entry
   digests with the 1 MiB tree of §9.1, chunk hashes, the content digest,
   commit records and superblock checksums. SHA-256 is what the SDK uses
   everywhere already (sources.lock, pkg's index, apkpack's signing),
   arm64 phones compute it in hardware (the SHA-2 instructions of ARMv8),
   and the one thing BLAKE3 would add, verifying part of an entry, §9.1
   builds from SHA-256 leaves. One hash across the SDK, and one less
   library.
2. **Monocypher for Ed25519.** Signatures (§9.3) are verified and made
   with Monocypher (CC0 or BSD-2-Clause, one C file and its header, and an
   independent audit), using its `optional/monocypher-ed25519` functions:
   Monocypher's default `crypto_eddsa_*` uses BLAKE2b and is not Ed25519,
   so a reader that called those would reject every valid signature. The
   library is vendored, as yyjson and zstd are, with a test of RFC 8032's
   vectors and of §9.3's rejections (a non-canonical S or point) before it
   is relied on. apkpack keeps BearSSL for the APK's own signature, which
   Android defines and Ed25519 is not part of.
3. **1 MiB chunks, fixed.** Chunks (§4.5) and the leaves of the digest's
   tree (§9.1) are 2^20 bytes for every entry. A chunk table costs 64 bytes
   a MiB (0.006%), a 10 GiB entry's table is 640 KiB, and changing one
   byte rewrites 1 MiB. It is fixed, not chosen per entry, so an entry's
   digest never depends on a writer's choice; a different size would be a
   new major version.

### B.2 Open

1. **The delta chain limit.** 32 bounds the worst lookup at 33 binary
   searches. A smaller limit compacts more often; a larger one commits
   more cheaply between compactions.
2. **sqterm's packages as 3.0 cartridges.** pkg would read `.sq`
   through sqcart's reader instead of tar and zstd, and a package would
   gain per-file metadata and a signature. The cost is that a package
   is no longer a tarball a person can open with `tar`.
3. **Per-kind extensions.** 2.0 Appendix B.2's question returns: one
   `.sq` for everything, or `.sqm` for manuals and others for
   readability, with the manifest still authoritative.
4. **Record schemas.** §5 names a schema by a consumer's string. Whether
   the SDK should share schemas (a JSON Schema per name, in a cartridge
   of its own) is open.
