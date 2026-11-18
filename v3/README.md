# sq.cart: the 3.0 reader

The reader for the [Squared Cartridge Specification
3.0](../docs/developer/specs/sq-cartridge-3.0.md), as C++23 modules: module
`sq.cart`, namespace `sq::cart`. Phase 1: open, look up, read and verify,
check signatures, validate. The writer (commits, compaction) is phase 2.
Nothing here shares code with the 2.0 library in `../src`; 3.0 does not have
to be compatible with it.

```cpp
import std;
import sq.cart;

auto cart = sq::cart::Archive::open("world.sq");
if (!cart) { std::println(std::cerr, "{}", cart.error().message); return 1; }
if (auto e = cart->find("world/settlements/9001.json")) {
    auto bytes = cart->read(*e);                 // verified against its digest
    auto part = cart->read_range(*e, 0, 4096);   // a chunked entry: only those chunks
}
auto report = sq::cart::check("world.sq");       // §12, in full
```

## Building

```sh
make test                          # the library, sqcart3 and the tests, then run them
make CXX=/usr/lib/llvm-23/bin/clang++ test
```

Needs clang and libc++ with `import std;`: 23 is what sqterm's packages and
CI use. The std module is built once from the libc++ the compiler names
(`clang++ -stdlib=libc++ -print-library-module-manifest-path`). In sqterm,
`pkg install llvm` brings clang, the sysroot and the runtimes (libc++ with
its module sources).

Offline: everything else is in `../third_party`:

| | |
|---|---|
| `squared-data` | squared's JSON facade, `sq.data` (`../tools/vendor_squared_data.sh` copies it): sq.cart reads and writes JSON only through it |
| `yyjson-0.12.0` | what sq.data parses with |
| `zstd-1.5.7` | zstd's single-file decoder (codecs 1 and 2) |
| `monocypher-4.0.3` | Ed25519, from its optional file (§9.3, Appendix B.1) |

## The pieces

| Partition | What |
|---|---|
| `:error` | `Code`, `Error`, `Result<T>` (std::expected) |
| `:bytes` | where the bytes are (pread on a file, or memory), little-endian fields, overflow-checked sums |
| `:sha256` | SHA-256, and the entry digest's tree over 1 MiB leaves (§9.1) |
| `:json` | I-JSON and RFC 8785, through sq.data |
| `:fold` | Unicode full case folding, for paths that collide (§4.1); `casefold.inc` is generated from Python's `str.casefold` |
| `:zstd` | one frame, its size and window and dictionary checked before it is decoded (§4.3) |
| `:ed25519` | verification that refuses non-canonical encodings (§9.3) |
| `:archive` | `Archive`, `Entry`, `Commit`, signatures, and `check` |

`sqcart3 check|ls|cat|info FILE` is the reader on the command line.

## The tests

`tests/testing.cppm` (`sq.cart.testing`) writes 3.0 cartridges byte by byte
for the tests, including the broken ones a writer must never write, with
real zstd frames and a trained dictionary (`tests/zstd_fixtures.inc`).
`test_redteam.cpp` has one case for each finding of the red-team review
that a reader can see.

## Not yet

- **Normalization.** Paths must be NFC (§4.1); like the 2.0 library, this
  reader does not check it (it would need the NFC tables).
- **Old data regions.** A run may name data an earlier commit wrote
  (§3.6). The reader checks it lies in the file, past the header, and in
  no index, string table or record of the commits it walks; commits before
  the full index are not read, so a run pointing into one of their indexes
  is not caught (its content still has to match its digest).
- **Opening costs the whole index.** Open checks every index the chain
  holds (order, paths, collisions, runs) and the manifest, so it reads
  every entry record once: §1's "opening reads a fixed header and one commit
  record" holds for the header and records, not for validation.
- **Memory.** Indexes and string tables are read into memory, and an
  unchunked entry is read whole to be verified. Mapping them is later.
