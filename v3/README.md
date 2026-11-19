# sq.cart: the 3.0 reader and writer

The reader and writer for the [Squared Cartridge Specification
3.0](../docs/developer/specs/sq-cartridge-3.0.md), as C++23 modules, in
namespace `sq::cart`:

- `sq.cart`: open, look up, read and verify, check signatures, validate.
- `sq.cart.write`: commit, compact, sign. Its own module, so a program that
  only reads carries no compressor.

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

```cpp
import sq.cart.write;

auto w = sq::cart::Writer::open("world.sq");     // takes world.sq.lock (§10.2)
w->put("world/settlements/9001.json", bytes, {.title = "Northford", .flags = sq::cart::flag::r | sq::cart::flag::w});
w->put_range("world/map.bin", offset, patch);    // a chunked entry: only the chunks it touches
w->set_metadata("README.md", {.title = "Read me"});  // no content written again (§3.6)
w->remove("old.txt");
w->sign_with(sq::cart::SigningKey::from_seed(seed, "Jesse"));
auto commit = w->commit();                       // append, flush, spare slot, flush (§10.3)
sq::cart::compact("world.sq");                   // §10.4

auto id = w->train_dictionary(samples);          // many small, similar entries (§4.3)
w->put("world/settlements/9001.json", record, {.dictionary = *id});
```

The writer stores what is compressed already (by media type) or would not
shrink 5%, chunks entries past 64 MiB, stores the manifest and mappable
entries, writes a full index first and before the delta chain would pass its
limit, and reads every commit back through the reader before it reports it
done.

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
| `zstd-1.5.7` | zstd: its decoder alone for readers, the whole library for the writer |
| `monocypher-4.0.3` | Ed25519, from its optional file (§9.3, Appendix B.1) |

## The pieces

| Partition | What |
|---|---|
| `:error` | `Code`, `Error`, `Result<T>` (std::expected) |
| `:bytes` | where the bytes are (pread on a file, or memory), little-endian fields, overflow-checked sums |
| `:sha256` | SHA-256, and the entry digest's tree over 1 MiB leaves (§9.1) |
| `:json` | I-JSON and RFC 8785, through sq.data |
| `:fold` | Unicode for paths (§4.1): full case folding, for paths that collide, and Normalization Form C, which every path must be in (the reader refuses others; `pack` converts a Mac's decomposed names). `casefold.inc` and `nfc.inc` are generated from Python's `str.casefold` and `unicodedata` (`tools/gen_casefold.py`, `tools/gen_nfc.py`); NFC was checked against Python for every code point and 20,000 mixed strings |
| `:zstd` | one frame, its size and window and dictionary checked before it is decoded (§4.3) |
| `:ed25519` | verification that refuses non-canonical encodings (§9.3) |
| `:archive` | `Archive`, `Entry`, `Commit`, signatures, and `check` |

`sqcart3` is both on the command line: `check`, `ls`, `cat`, `info` and
`unpack` to read; `new`, `put`, `rm`, `compact`, `keygen` and `pack` to write
(`--sign KEYFILE` signs a commit). `make` also builds `sqcart3-read`, the
read commands linked against the reading archives alone: it holds no zstd
compressor.

| Archive | Holds | Link for |
|---|---|---|
| `libsqcart.a` | sq.cart, sq.data, yyjson, Monocypher | both |
| `libsqcart-unzstd.a` | zstd's decoder | reading only |
| `libsqcart-write.a` | sq.cart.write | writing |
| `libsqcart-zstd.a` | all of zstd (its decoder too: never with `-unzstd`) | writing |

## Opening, and what is checked when

Open reads the header, the superblocks, the commit records down to the full
index, and the manifest, and maps the indexes and string tables in place
(`mmap`, read-only) rather than reading them. Each record `find()` returns
is checked as it is returned; each run must lie in a data region of its own
commit or an earlier one, which the reader learns by walking every commit's
record once, on first need. The whole state (every index's order, every
record, the live count, collisions, features) is checked the first time
something needs all of it: `entries()`, the content digest, `check()`
(§12). On a cartridge of 100,000 entries: open 8 ms, a `find` 9 µs, the
whole state 107 ms.

`find()` returns the entry, or `Code::not_found`, or the error that stops
it; `entries()` returns the live entries, or the error that stops them.

## The tests

`tests/testing.cppm` (`sq.cart.testing`) writes 3.0 cartridges byte by byte
for the tests, including the broken ones a writer must never write, with
real zstd frames and a trained dictionary (`tests/zstd_fixtures.inc`).
`test_redteam.cpp` has one case for each finding of the red-team review
that a reader can see.

## Not yet

- **Staging** is in memory: a commit's content is held until it is written.
  Staging large changes on disk is the Drive's (§10.5).

