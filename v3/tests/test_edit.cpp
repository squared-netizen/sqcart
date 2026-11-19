// SPDX-License-Identifier: MIT
//
// What the Virtual Cartridge Drive asks of the writer: many ranges and
// resizes of one entry in one commit, renames that name the same runs
// (§3.6), entries streamed from a file, compaction under a writer's own
// lock, and Code::busy for a second writer. A model (a plain byte vector)
// checks random ranges and resizes against what the reader reads back.

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
                 std::format("sqcart-edit-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir / name;
}

WriteOptions quick_options() {
    WriteOptions o;
    o.sync = false;
    o.commit_time = 1794960000000;
    o.level = 1;
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

std::optional<std::vector<std::byte>> read_back(const std::filesystem::path& p, std::string_view path) {
    auto a = Archive::open(p);
    if (!a) return std::nullopt;
    auto e = a->find(path);
    if (!e) return std::nullopt;
    auto bytes = a->read(*e);
    if (!bytes) return std::nullopt;
    return *bytes;
}

std::uint64_t bytes_on_disk(const std::filesystem::path& p) { return std::filesystem::file_size(p); }

struct Start {
    std::string path;
    std::vector<std::byte> content;
    Store store = Store::automatic;
};

/// A cartridge with a manifest and `entries`, committed.
void start(const std::filesystem::path& p, std::initializer_list<Start> entries) {
    std::filesystem::remove(p);
    auto w = Writer::create(p, quick_options());
    check(w.has_value(), w ? "create" : w.error().message);
    if (!w) return;
    static_cast<void>(w->put("SQ-INF/manifest.json", manifest()));
    for (const auto& e : entries) static_cast<void>(w->put(e.path, e.content, {.store = e.store}));
    auto c = w->commit();
    check(c.has_value(), c ? "commit" : c.error().message);
}

}  // namespace

int main() {
    const std::uint64_t mib = chunk_size;

    // Many ranges on one chunked entry in one commit, in order; chunks no
    // range touches are named again, not written again.
    {
        const auto p = scratch("ranges.sq");
        auto model = pattern(mib * 4 + 500, 1);
        start(p, {{"map.bin", model, Store::chunked}});
        std::vector<Chunk> old;
        {
            auto a = Archive::open(p);
            old = a->chunks(*a->find("map.bin")).value();
        }
        auto w = Writer::open(p, quick_options());
        const auto one = pattern(100, 2), two = pattern(300, 3), three = pattern(50, 4);
        check(w->put_range("map.bin", 10, one).has_value(), "a first range");
        check(w->put_range("map.bin", 60, two).has_value(), "a second range on the same entry, over the first");
        check(w->put_range("map.bin", 3 * mib + 7, three).has_value(), "a third, in chunk 3");
        std::ranges::copy(one, model.begin() + 10);
        std::ranges::copy(two, model.begin() + 60);
        std::ranges::copy(three, model.begin() + static_cast<std::ptrdiff_t>(3 * mib + 7));
        check(w->size("map.bin").value() == model.size(), "size() before the commit");
        auto c = w->commit();
        check(c.has_value(), c ? "one commit takes them all" : c.error().message);
        check(read_back(p, "map.bin") == model, "they apply in the order given");
        auto a = Archive::open(p);
        const auto now = a->chunks(*a->find("map.bin")).value();
        check(now[1].offset == old[1].offset && now[2].offset == old[2].offset && now[4].offset == old[4].offset,
              "untouched chunks name their old runs");
        check(now[0].offset != old[0].offset && now[3].offset != old[3].offset, "touched chunks are written again");
        check(checks(p), "check passes");
    }

    // Renames: the new record names the old runs; nothing is copied.
    {
        const auto p = scratch("rename.sq");
        const auto big = pattern(mib * 5, 5), small = pattern(4000, 6);
        start(p, {{"big.bin", big, Store::chunked}, {"note.txt", small}, {"old.txt", bytes("in the way")}});
        const auto size_before = bytes_on_disk(p);
        std::uint64_t big_run = 0;
        {
            auto a = Archive::open(p);
            big_run = a->chunks(*a->find("big.bin")).value().front().offset;
        }
        {
            auto w = Writer::open(p, quick_options());
            check(w->rename("big.bin", "moved/big.bin").has_value(), "rename a chunked entry");
            check(w->rename("note.txt", "a.txt").has_value(), "rename a small entry");
            check(w->rename("a.txt", "b.txt").has_value(), "and again before the commit");
            check(w->rename("b.txt", "old.txt").has_value(), "onto an entry, replacing it");
            check(!w->rename("nothing", "x"), "renaming nothing is refused");
            check(!w->rename("SQ-INF/manifest.json", "m.json"), "the manifest keeps its path");
            check(!w->rename("old.txt", "../x"), "a bad path is refused");
            auto c = w->commit();
            check(c.has_value(), c ? "commit the renames" : c.error().message);
        }
        check(bytes_on_disk(p) - size_before < 4096, std::format("a 5 MiB rename writes records only ({} bytes)", bytes_on_disk(p) - size_before));
        auto a = Archive::open(p);
        check(a && !a->find("big.bin") && !a->find("note.txt") && !a->find("a.txt") && !a->find("b.txt"), "the old names are gone");
        check(read_back(p, "moved/big.bin") == big && read_back(p, "old.txt") == small, "the new names read the old content");
        check(a->chunks(*a->find("moved/big.bin")).value().front().offset == big_run, "and name the old runs");
        check(checks(p), "check passes");
        // A range after a rename, in the same commit.
        {
            auto w = Writer::open(p, quick_options());
            check(w->rename("moved/big.bin", "big.bin").has_value(), "rename back");
            check(w->put_range("big.bin", mib + 1, bytes("patched")).has_value(), "and patch it");
            check(w->set_metadata("big.bin", {.title = "Big"}).has_value(), "and retitle it");
            check(w->commit().has_value(), "commit");
        }
        auto expect = big;
        std::ranges::copy(bytes("patched"), expect.begin() + static_cast<std::ptrdiff_t>(mib + 1));
        check(read_back(p, "big.bin") == expect, "the patch lands at the new name");
        check(Archive::open(p)->find("big.bin")->title == "Big", "with its new title");
        check(checks(p), "check passes");
    }

    // Resizes: cut a chunked entry, grow it with zeros, empty it; a small
    // entry is read and put again.
    {
        const auto p = scratch("resize.sq");
        auto model = pattern(mib * 3 + 500, 7);
        const auto small = pattern(5000, 8);
        start(p, {{"map.bin", model, Store::chunked}, {"small.bin", small}});
        std::vector<Chunk> old;
        {
            auto a = Archive::open(p);
            old = a->chunks(*a->find("map.bin")).value();
        }
        {
            auto w = Writer::open(p, quick_options());
            check(w->resize("map.bin", mib + 100).has_value(), "cut");
            check(w->commit().has_value(), "commit the cut");
        }
        model.resize(mib + 100);
        check(read_back(p, "map.bin") == model, "the cut entry reads its first bytes");
        {
            auto a = Archive::open(p);
            check(a->chunks(*a->find("map.bin")).value().front().offset == old.front().offset, "chunk 0, below the cut, is named again");
        }
        {
            auto w = Writer::open(p, quick_options());
            check(w->resize("map.bin", 2 * mib).has_value(), "grow");
            check(w->resize("small.bin", 100).has_value(), "cut a small entry");
            check(w->put_range("small.bin", 200, bytes("tail")).has_value(), "and write past its end");
            check(w->commit().has_value(), "commit");
        }
        model.resize(2 * mib);
        check(read_back(p, "map.bin") == model, "growing adds zeros");
        auto s = small;
        s.resize(100);
        s.resize(204);
        std::ranges::copy(bytes("tail"), s.begin() + 200);
        check(read_back(p, "small.bin") == s, "a small entry is cut and written as asked, gaps zero");
        {
            auto w = Writer::open(p, quick_options());
            check(w->resize("map.bin", 0).has_value(), "empty a chunked entry");
            check(w->commit().has_value(), "commit");
        }
        check(read_back(p, "map.bin") == std::vector<std::byte>{}, "it reads empty");
        check(checks(p), "check passes");
    }

    // The model: random ranges and resizes over several commits.
    {
        const auto p = scratch("model.sq");
        auto model = pattern(mib * 2 + 77, 9);
        start(p, {{"m.bin", model, Store::chunked}});
        std::mt19937 rng(42);
        const auto pick = [&](std::uint64_t n) { return std::uniform_int_distribution<std::uint64_t>(0, n)(rng); };
        bool ok = true;
        for (int round = 0; round < 6 && ok; ++round) {
            auto w = Writer::open(p, quick_options());
            for (int k = 0; k < 12; ++k) {
                if (pick(4) == 0) {
                    const std::uint64_t n = pick(4 * mib);
                    ok = ok && w->resize("m.bin", n).has_value();
                    model.resize(static_cast<std::size_t>(n));
                } else {
                    const std::uint64_t at = pick(model.size() + mib / 2), n = pick(mib + 1000);
                    const auto data = pattern(static_cast<std::size_t>(n), static_cast<unsigned>(rng()));
                    ok = ok && w->put_range("m.bin", at, data).has_value();
                    if (at + n > model.size()) model.resize(static_cast<std::size_t>(at + n));
                    std::ranges::copy(data, model.begin() + static_cast<std::ptrdiff_t>(at));
                }
            }
            ok = ok && w->size("m.bin").value() == model.size();
            auto c = w->commit();
            ok = ok && c.has_value() && read_back(p, "m.bin") == model;
        }
        check(ok, "72 random ranges and resizes over 6 commits match the model");
        check(checks(p), "check passes");
    }

    // put_file: small read whole, big streamed as chunks.
    {
        const auto p = scratch("file.sq");
        start(p, {});
        const auto big = pattern(mib * 3 + 12345, 10), small = pattern(7000, 11);
        const auto big_file = scratch("big.input"), small_file = scratch("small.input");
        {
            std::ofstream(big_file, std::ios::binary).write(reinterpret_cast<const char*>(big.data()), static_cast<std::streamsize>(big.size()));
            std::ofstream(small_file, std::ios::binary).write(reinterpret_cast<const char*>(small.data()), static_cast<std::streamsize>(small.size()));
        }
        auto w = Writer::open(p, quick_options());
        check(w->put_file("big.bin", big_file, {.store = Store::chunked}).has_value(), "put_file, chunked");
        check(w->put_file("small.bin", small_file).has_value(), "put_file, small");
        check(!w->put_file("none.bin", scratch("no-such-file")), "a missing file is refused at once");
        check(!w->put_range("big.bin", 0, bytes("x")), "a range on a put_file before the commit is refused");
        check(w->size("big.bin").value() == big.size(), "size() of a put_file");
        auto c = w->commit();
        check(c.has_value(), c ? "commit" : c.error().message);
        check(read_back(p, "big.bin") == big && read_back(p, "small.bin") == small, "both read back");
        auto a = Archive::open(p);
        check(a->find("big.bin")->codec == Codec::chunked && a->find("big.bin")->digest == tree_hash(big), "streamed: chunked, its digest the whole content's");
        check(checks(p), "check passes");
    }

    // set_metadata after a put keeps the content put.
    {
        const auto p = scratch("meta.sq");
        start(p, {});
        auto w = Writer::open(p, quick_options());
        check(w->put("n.txt", bytes("content")).has_value(), "put");
        check(w->set_metadata("n.txt", {.title = "Titled"}).has_value(), "then retitle, before the commit");
        check(w->commit().has_value(), "commit");
        check(read_back(p, "n.txt") == bytes("content") && Archive::open(p)->find("n.txt")->title == "Titled", "content and title both");
    }

    // A second writer: Code::busy. Writer::compact keeps the lock and writes on.
    {
        const auto p = scratch("compact.sq");
        start(p, {{"a.bin", pattern(200000, 12)}});
        auto w = Writer::open(p, quick_options());
        auto other = Writer::open(p, quick_options());
        check(!other && other.error().code == Code::busy, "a second writer gets Code::busy");
        auto free_compact = compact(p, quick_options());
        check(!free_compact && free_compact.error().code == Code::busy, "and so does compact() from outside");
        for (int i = 0; i < 3; ++i) {
            static_cast<void>(w->put("a.bin", pattern(200000, static_cast<unsigned>(20 + i))));
            static_cast<void>(w->commit());
        }
        const auto grown = bytes_on_disk(p);
        check(!w->put("x", bytes("x")) || !w->compact(), "compact() refuses while something is staged");
        w->discard();
        auto c = w->compact();
        check(c.has_value(), c ? "compact on the open writer" : c.error().message);
        check(bytes_on_disk(p) < grown, std::format("the file shrank ({} to {})", grown, bytes_on_disk(p)));
        check(!Writer::open(p, quick_options()), "the lock is still held");
        check(w->put("b.txt", bytes("after")).has_value() && w->commit().has_value(), "and the writer writes on");
        check(read_back(p, "b.txt") == bytes("after") && read_back(p, "a.bin") == pattern(200000, 22), "to the compacted file");
        check(checks(p), "check passes");
    }
    // put_range_file: ranges read from a file at commit, mixed with ranges
    // in memory and resizes, against a model; a file that changes is refused.
    {
        const auto p = scratch("rangefile.sq");
        auto model = pattern(mib * 3 + 99, 30);
        start(p, {{"big.bin", model, Store::chunked}, {"small.bin", pattern(5000, 31)}});
        const auto source = pattern(mib * 2 + 4321, 32);
        const auto src = scratch("ranges.input");
        std::ofstream(src, std::ios::binary).write(reinterpret_cast<const char*>(source.data()), static_cast<std::streamsize>(source.size()));
        {
            auto w = Writer::open(p, quick_options());
            check(w->put_range_file("big.bin", 10, src, 0, mib + 7).has_value(), "a range from a file, across chunks");
            std::copy_n(source.begin(), mib + 7, model.begin() + 10);
            check(w->put_range("big.bin", 500, bytes("in memory")).has_value(), "a range in memory over it");
            std::ranges::copy(bytes("in memory"), model.begin() + 500);
            check(w->put_range_file("big.bin", 3 * mib + 50, src, 1000, mib).has_value(), "a range from the middle of the file, past the end");
            model.resize(4 * mib + 50);
            std::copy_n(source.begin() + 1000, mib, model.begin() + static_cast<std::ptrdiff_t>(3 * mib + 50));
            check(w->size("big.bin").value() == model.size(), "size() counts it");
            check(!w->put_range_file("big.bin", 0, src, mib * 2, mib), "a range past the file's end is refused at once");
            check(w->put_range_file("small.bin", 100, src, 7, 300).has_value(), "on an entry that is not chunked");
            auto c = w->commit();
            check(c.has_value(), c ? "commit" : c.error().message);
        }
        check(read_back(p, "big.bin") == model, "the entry is the model");
        auto small = pattern(5000, 31);
        std::copy_n(source.begin() + 7, 300, small.begin() + 100);
        check(read_back(p, "small.bin") == small, "and the small one");
        check(checks(p), "check passes");
        {
            auto w = Writer::open(p, quick_options());
            check(w->put_range_file("big.bin", 0, src, 0, mib).has_value(), "stage a range");
            std::filesystem::resize_file(src, 100);  // the file changes before the commit
            auto c = w->commit();
            check(!c && c.error().code == Code::io, "a file shorter at commit than when staged is refused");
            w->discard();
        }
        check(read_back(p, "big.bin") == model && checks(p), "and nothing changed");
    }

    // set_commit_time: the next commit records it.
    {
        const auto p = scratch("time.sq");
        start(p, {});
        auto w = Writer::open(p, quick_options());
        w->set_commit_time(1800000000123);
        static_cast<void>(w->put("t", bytes("t")));
        auto c = w->commit();
        check(c && c->time == 1800000000123 && Archive::open(p)->commit().time == 1800000000123, "set_commit_time: the commit records it");
    }
    return report("edit");
}
