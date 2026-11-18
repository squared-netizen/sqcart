// SPDX-License-Identifier: MIT
//
// The writer (§10): commits that a reader opens and check() passes,
// deltas, metadata-only changes, ranges of chunked entries that write only
// the chunks they touch, signatures, the chain limit, the lock, trailing
// bytes, sequence numbers after a torn slot, and compaction.

import std;
import sq.cart;
import sq.cart.write;
import sq.cart.testing;

using namespace sq::cart;
using namespace sq::cart::testing;

namespace {

std::filesystem::path scratch(std::string_view name) {
    static const auto dir = [] {
        auto d = std::filesystem::temp_directory_path() /
                 std::format("sqcart-write-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir / name;
}

WriteOptions quick_options() {
    WriteOptions o;
    o.sync = false;
    o.commit_time = 1794960000000;
    o.level = 3;
    return o;
}

std::vector<std::byte> pattern(std::size_t n, unsigned seed) {
    std::vector<std::byte> b(n);
    std::uint32_t x = seed;
    for (auto& v : b) {
        x = x * 1664525u + 1013904223u;
        v = static_cast<std::byte>(x >> 24);
    }
    return b;
}

bool checks(const std::filesystem::path& p) {
    const auto r = check(p);
    for (const auto& f : r.findings) std::println(std::cerr, "  {}: {} {}", f.severity == Finding::Severity::error ? "error" : "warning", f.entry.value_or(""), f.message);
    return r.ok() && r.findings.empty();
}

}  // namespace

int main() {
    const auto text = [] {
        std::string t;
        for (int i = 0; i < 200; ++i) t += "The quick brown fox jumps over the lazy dog. ";
        return bytes(t);
    }();

    // A new cartridge.
    const auto path = scratch("world.sq");
    {
        auto w = Writer::create(path, quick_options());
        check(w.has_value(), w ? "create" : w.error().message);
        if (!w) return report("write");
        check(w->put("SQ-INF/manifest.json", manifest()).has_value(), "put the manifest");
        check(w->put("text/fox.txt", text, {.title = "The fox", .media_type = "text/plain"}).has_value(), "put text");
        check(w->put("art/tile.png", pattern(5000, 1), {.media_type = "image/png"}).has_value(), "put a png");
        check(w->put("fonts/mono.ttf", pattern(3000, 2), {.flags = flag::r | flag::mappable}).has_value(), "put a mappable font");
        check(w->put("empty", {}).has_value(), "put an empty entry");
        check(w->put("data/settlement.json", bytes(record_json), {.attributes = R"({"name":"Northford","population":41250})", .created = 1794960000000}).has_value(), "put with attributes");
        check(!w->put("a/../b", text), "a bad path is refused at put");
        check(!w->put("x", text, {.attributes = R"({"b":1,"a":2})"}), "attributes not in canonical form are refused");
        auto c = w->commit();
        check(c && c->sequence == 1 && c->index_kind == IndexKind::full, c ? "commit 1" : c.error().message);
        check(c && c->time == 1794960000000, "the commit time given");
    }
    {
        auto a = Archive::open(path);
        check(a.has_value(), "the reader opens it");
        if (a) {
            check(a->find("text/fox.txt")->codec == Codec::zstd, "text is compressed");
            check(a->find("art/tile.png")->codec == Codec::stored, "a png is stored");
            const auto font = a->find("fonts/mono.ttf");
            check(font->codec == Codec::stored && font->offset % 16384 == 0, "a mappable entry is stored at 16384");
            check(a->read(*a->find("text/fox.txt")).value() == text, "text reads back");
            check(a->find("data/settlement.json")->attributes == R"({"name":"Northford","population":41250})", "attributes read back");
        }
        check(checks(path), "check finds nothing");
    }

    // A second commit: a delta. Remove, retitle, add a chunked entry.
    const auto big = pattern(chunk_size * 3 + 1000, 3);
    {
        auto w = Writer::open(path, quick_options());
        check(w.has_value(), w ? "open for writing" : w.error().message);
        if (!w) return report("write");
        auto other = Writer::open(path, quick_options());
        check(!other, "a second writer is refused while the lock is held (§10.2)");
        check(w->remove("empty").has_value(), "remove");
        check(!w->remove("nothing"), "removing nothing is refused");
        check(w->set_metadata("text/fox.txt", {.title = "The quick fox", .media_type = "text/plain"}).has_value(), "retitle");
        check(w->put("world/map.bin", big, {.store = Store::chunked}).has_value(), "put a chunked entry");
        auto c = w->commit();
        check(c && c->sequence == 2 && c->index_kind == IndexKind::delta && c->chain_length == 1, c ? "commit 2, a delta" : c.error().message);
    }
    std::vector<Chunk> before;
    std::uint64_t fox_offset = 0;
    {
        auto a = Archive::open(path);
        check(a && !a->find("empty"), "the removed entry is gone");
        check(a && a->find("text/fox.txt")->title == "The quick fox", "the new title");
        if (a) {
            fox_offset = a->find("text/fox.txt")->offset;
            const auto map = a->find("world/map.bin");
            check(map && a->read(*map).value() == big, "the chunked entry reads back");
            before = a->chunks(*map).value();
        }
        check(checks(path), "check finds nothing after a delta");
    }
    {
        // The retitle named the old run: no copy (§3.6).
        Builder unused;
        auto a = Archive::open(path);
        check(a && a->chain().size() == 2, "two commits in the chain");
        if (a) {
            const auto first = a->chain().back();
            check(fox_offset < first.offset, "the retitled entry's run is commit 1's");
        }
    }

    // put_range: change bytes in chunk 1 and grow the entry; chunks 0 and 2 are named again.
    auto changed = big;
    const auto patch = bytes("PATCHED");
    std::ranges::copy(patch, changed.begin() + static_cast<std::ptrdiff_t>(chunk_size + 10));
    const auto tail = pattern(500, 9);
    {
        auto w = Writer::open(path, quick_options());
        check(w->put_range("world/map.bin", chunk_size + 10, patch).has_value(), "put_range inside");
        auto c = w->commit();
        check(c.has_value(), c ? "commit 3" : c.error().message);
        check(w->put_range("world/map.bin", changed.size(), tail).has_value(), "put_range past the end");
        changed.insert(changed.end(), tail.begin(), tail.end());
        c = w->commit();
        check(c.has_value(), c ? "commit 4" : c.error().message);
    }
    {
        auto a = Archive::open(path);
        const auto map = a->find("world/map.bin");
        check(map && map->size == changed.size() && a->read(*map).value() == changed, "the patched, grown entry reads back");
        const auto after = a->chunks(*map).value();
        check(after[0].offset == before[0].offset && after[2].offset == before[2].offset, "untouched chunks are named again, not written");
        check(after[1].offset != before[1].offset, "the touched chunk is written again");
        check(after[3].offset != before[3].offset, "the last chunk, grown, is written again");
        check(checks(path), "check finds nothing after put_range");
    }

    // Signatures, on the next commit.
    std::array<std::byte, 32> seed{};
    seed[5] = std::byte{77};
    {
        auto w = Writer::open(path, quick_options());
        w->sign_with(SigningKey::from_seed(seed, "Jesse"));
        check(w->put("signed.txt", bytes("signed")).has_value(), "put");
        auto c = w->commit();
        check(c && c->signatures_length > 0, "a signed commit");
    }
    {
        auto a = Archive::open(path);
        auto sigs = a->signatures();
        check(sigs && sigs->size() == 1 && (*sigs)[0].state == SignatureState::valid && (*sigs)[0].signature.signer == "Jesse",
              "the signature verifies");
    }

    // Compaction: one commit, the same state, the same signatures, smaller.
    const auto digest_before = Archive::open(path)->commit().content_digest;
    const auto size_before = std::filesystem::file_size(path);
    {
        auto r = compact(path, quick_options());
        check(r.has_value(), r ? "compact" : r.error().message);
    }
    {
        auto a = Archive::open(path);
        check(a && a->commit().sequence == 1 && a->chain().size() == 1, "one commit after compaction");
        check(a && a->commit().content_digest == digest_before, "the same content digest");
        check(std::filesystem::file_size(path) < size_before, "a smaller file");
        auto sigs = a->signatures();
        check(sigs && sigs->size() == 1 && (*sigs)[0].state == SignatureState::valid, "the signature still verifies");
        check(a->read(*a->find("world/map.bin")).value() == changed, "the chunked entry reads back");
        check(checks(path), "check finds nothing after compaction");
    }

    // Trailing bytes are truncated when a writer opens (§10.3).
    {
        { std::ofstream(path, std::ios::binary | std::ios::app) << "half a commit"; }
        auto w = Writer::open(path, quick_options());
        check(w->put("after.txt", bytes("x")).has_value(), "put");
        check(w->commit().has_value(), "commit after trailing bytes");
        check(Archive::open(path)->trailing_bytes() == 0, "no trailing bytes after it");
    }

    // The chain limit: a full index before the chain would pass it.
    {
        auto p = scratch("chain.sq");
        auto o = quick_options();
        o.limits.max_delta_chain = 2;
        auto w = Writer::create(p, o);
        w->put("SQ-INF/manifest.json", manifest());
        w->commit();
        for (int i = 0; i < 3; ++i) {
            w->put(std::format("f{}", i), bytes("x"));
            w->commit();
        }
        auto a = Archive::open(p);
        check(a && a->commit().sequence == 4 && a->commit().index_kind == IndexKind::full, "the fourth commit is full again");
        check(a && a->entries().size() == 4, "and holds every entry");
    }

    // A torn current slot: the writer continues from the commit the reader
    // falls back to, with a sequence number above both slots (§10.3).
    {
        auto p = scratch("torn.sq");
        {
            auto w = Writer::create(p, quick_options());
            w->put("SQ-INF/manifest.json", manifest());
            w->put("a", bytes("one"));
            w->commit();
            w->put("a", bytes("two"));
            w->commit();
        }
        {
            std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
            f.seekp(4096 + 60);
            f.put('\x55');  // slot B, commit 2: its checksum now fails
        }
        auto w = Writer::open(p, quick_options());
        check(w.has_value(), "a writer opens a cartridge with a torn slot");
        w->put("b", bytes("three"));
        auto c = w->commit();
        check(c && c->sequence == 3, "its commit is numbered above both slots");
        auto a = Archive::open(p);
        check(a && a->commit().sequence == 3 && a->find("a") && *a->read(*a->find("a")) == bytes("one"), "on commit 1's state");
        check(checks(p), "check finds nothing");
    }

    // Refusals.
    {
        auto p = scratch("refused.sq");
        auto w = Writer::create(p, quick_options());
        check(!Writer::create(p, quick_options()), "create refuses a file that is there");
        w->put("README", bytes("x"));
        check(!w->commit(), "no manifest: refused");
        w->put("SQ-INF/manifest.json", manifest());
        w->put("readme", bytes("y"));
        auto c = w->commit();
        check(!c && c.error().code == Code::path, "README and readme collide: refused");
    }

    // The lock without flock (§10.2): the owner file.
    {
        auto p = scratch("owner.sq");
        auto o = quick_options();
        o.lock = LockMode::owner_file;
        auto owner = p;
        owner += ".lock.owner";
        {
            auto w = Writer::create(p, o);
            check(w.has_value(), "create under an owner file");
            check(std::filesystem::exists(owner), "the owner file names its holder");
            check(!Writer::open(p, o), "a second writer is refused while it is held");
            w->put("SQ-INF/manifest.json", manifest());
            check(w->commit().has_value(), "commit under an owner file");
        }
        check(!std::filesystem::exists(owner), "released: the owner file is gone");
        std::string here;
        { std::ifstream("/proc/sys/kernel/hostname") >> here; }  // gethostname's, on Linux and Android
        // A holder gone from this host: taken over. (Process 2^22 + 7 is not running.)
        { std::ofstream(owner) << std::format("{} {} 0\n", here, 4194311); }
        auto taken = Writer::open(p, o);
        check(taken.has_value(), taken ? "a dead holder's lock is taken over" : taken.error().message);
        taken = std::unexpected(Error{});
        // A holder on another host: never taken.
        { std::ofstream(owner) << "some-other-host 1 0\n"; }
        auto refused = Writer::open(p, o);
        check(!refused, "a lock held on another host is never taken");
        std::filesystem::remove(owner);
    }

    // Durable commits (sync on), for the flush calls themselves.
    {
        auto p = scratch("sync.sq");
        WriteOptions o;
        auto w = Writer::create(p, o);
        w->put("SQ-INF/manifest.json", manifest());
        check(w->commit().has_value(), "a synced commit");
    }

    std::filesystem::remove_all(scratch("").parent_path());
    return report("write");
}
