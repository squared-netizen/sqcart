// SPDX-License-Identifier: MIT
//
// The exploded form (§6.1): pack a folder, unpack it again, and get the same
// folder and the same content digest; what is never an entry; links refused
// both ways; names Windows cannot create.

import std;
import sq.cart;
import sq.cart.write;
import sq.cart.testing;

using namespace sq::cart;
using namespace sq::cart::testing;
namespace fs = std::filesystem;

namespace {

fs::path root() {
    static const fs::path d = fs::temp_directory_path() /
                              std::format("sqcart-exploded-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    return d;
}

void write(const fs::path& p, std::string_view text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

WriteOptions quick() {
    WriteOptions o;
    o.sync = false;
    o.level = 3;
    o.commit_time = 0;
    return o;
}

/// Every regular file under `dir`, relative, with its content.
std::map<std::string, std::string> tree(const fs::path& dir) {
    std::map<std::string, std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file()) out[e.path().lexically_relative(dir).generic_string()] = read(e.path());
    }
    return out;
}

}  // namespace

int main() {
    const fs::path src = root() / "world";
    write(src / "SQ-INF/manifest.json", text(manifest()));
    write(src / "README.md", "# a world\n");
    write(src / "art/tile.png", "not really a png");
    write(src / "regions/north/settlement.json", record_json);
    write(src / ".squared/journal", "working state");
    write(src / ".git/HEAD", "ref: refs/heads/main");
    write(src / ".anytool/cache", "another tool's working state");
    write(src / ".profile", "a dot file at the top: an entry");
    write(src / "art/.keep", "a dot file deeper down: an entry");
    write(src / "SQ-INF/entries.json",
          R"({"README.md":{"title":"Read me"},"regions/north/settlement.json":{"flags":"rw","media_type":"application/json","created":1794960000000,"attributes":{"population":41250,"name":"Northford"}}})");

    // Pack.
    const fs::path cart = root() / "world.sq";
    auto c = pack(src, cart, quick());
    check(c.has_value(), c ? "pack" : c.error().message);
    auto a = Archive::open(cart);
    check(a.has_value(), "the packed cartridge opens");
    if (!a) return report("exploded");
    check(a->entries()->size() == 6, "manifest, README, tile, settlement, .profile, art/.keep: six entries");
    check(!a->find(".squared/journal") && !a->find(".git/HEAD") && !a->find(".anytool/cache"),
          "a top-level folder named with a dot is never entries");
    check(a->find(".profile") && a->find("art/.keep"), "a dot file is an entry like any other");
    check(!a->find("SQ-INF/entries.json"), "entries.json is not an entry");
    check(a->find("README.md")->title == "Read me", "a title from entries.json");
    const auto s = a->find("regions/north/settlement.json");
    check(s && s->flags == (flag::r | flag::w) && s->created == 1794960000000, "flags and times from entries.json");
    check(s && s->attributes == R"({"name":"Northford","population":41250})", "attributes, canonical");
    check(check(cart).ok(), "check passes");

    // The folder read where it is: the same entries, metadata and bytes as
    // the packed cartridge.
    {
        auto f = Folder::open(src);
        check(f.has_value(), f ? "a folder opens" : f.error().message);
        if (f) {
            check(f->manifest().id == a->manifest().id && f->manifest().kind == a->manifest().kind, "the same manifest");
            const auto packed = a->entries();
            bool same = packed && packed->size() == f->entries().size();
            for (std::size_t i = 0; same && i < packed->size(); ++i) {
                const Entry &p = (*packed)[i], &e = f->entries()[i];
                same = p.path == e.path && p.flags == e.flags && p.title == e.title && p.created == e.created &&
                       p.attributes == e.attributes && p.media_type == e.media_type && p.size == e.size &&
                       [&] { auto x = a->read(p), y = f->read(e); return x && y && *x == *y; }();
            }
            check(same, "the same entries, metadata and bytes as packed");
            check(!f->find(".git/HEAD") && !f->find("SQ-INF/entries.json") && f->find(".profile"), "the same entries left out");
        }
        const fs::path old = root() / "old";
        write(old / "SQ-INF/manifest.json", R"({"format":"squared-cartridge","format_version":2,"kind":"template","id":"a.b","version":"1.0.0"})");
        auto o = Folder::open(old);
        check(!o && o.error().code == Code::unsupported, "a 2.0 folder is refused");
        check(!Folder::open(root() / "outside"), "a folder with no manifest is refused");
    }

    // --keep .git: a kept dot folder packed, its executable files flagged
    // x; unpacked with executable, a hook runs again; without, it does not.
    {
        const fs::path g = root() / "with-git";
        write(g / "SQ-INF/manifest.json", text(manifest()));
        write(g / "README.md", "# with history\n");
        write(g / ".git/HEAD", "ref: refs/heads/main");
        write(g / ".git/hooks/pre-commit", "#!/bin/sh\nexit 0\n");
        fs::permissions(g / ".git/hooks/pre-commit", fs::perms::owner_exec, fs::perm_options::add);
        write(g / ".squared/lock", "square's working state");
        const fs::path gc = root() / "with-git.sq";
        auto kept = pack(g, gc, quick(), {.keep = {".git"}});
        check(kept.has_value(), kept ? "pack --keep .git" : kept.error().message);
        auto ga = Archive::open(gc);
        check(ga && ga->find(".git/HEAD") && !ga->find(".squared/lock"), ".git kept by name, .squared still left out");
        check(ga && ga->find(".git/hooks/pre-commit") && (ga->find(".git/hooks/pre-commit")->flags & flag::x),
              "an executable file in a kept folder is flagged x");
        check(!pack(g, root() / "bad.sq", quick(), {.keep = {"git"}}).has_value(), "keep refuses a name without a dot");
        check(!pack(g, root() / "bad.sq", quick(), {.keep = {".nothing"}}).has_value(), "keep refuses a folder that is not there");
        if (ga) {
            const fs::path plain = root() / "unpacked-plain", exec = root() / "unpacked-exec";
            check(unpack(*ga, plain).has_value() && (fs::status(plain / ".git/hooks/pre-commit").permissions() & fs::perms::owner_exec) == fs::perms::none,
                  "unpack makes nothing executable by default");
            check(unpack(*ga, exec, {.executable = true}).has_value() &&
                      (fs::status(exec / ".git/hooks/pre-commit").permissions() & fs::perms::owner_exec) != fs::perms::none,
                  "unpack --executable restores the hook's executable bit");
        }
    }

    // A file past 64 MiB: packed chunked from the file (never read whole),
    // unpacked a verified chunk at a time, through NAME.part, the same bytes.
    {
        const fs::path big = root() / "big";
        write(big / "SQ-INF/manifest.json", text(manifest()));
        const std::uint64_t size = (std::uint64_t{64} << 20) + 3;
        {
            fs::create_directories(big / "assets");
            std::ofstream out(big / "assets/world.bin", std::ios::binary);
            std::vector<char> block(1 << 20);
            for (std::uint64_t at = 0; at < size; at += block.size()) {
                for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<char>((at + i) * 2654435761u >> 24);
                out.write(block.data(), static_cast<std::streamsize>(std::min<std::uint64_t>(block.size(), size - at)));
            }
        }
        const fs::path bc = root() / "big.sq";
        auto packed = pack(big, bc, quick());
        check(packed.has_value(), packed ? "pack a file past 64 MiB" : packed.error().message);
        auto ba = Archive::open(bc);
        const auto e = ba ? std::optional<Entry>(ba->find("assets/world.bin").value_or(Entry{})) : std::nullopt;
        check(e && e->codec == Codec::chunked && e->size == size, "stored chunked");
        if (ba) {
            const fs::path again = root() / "big-unpacked";
            auto u = unpack(*ba, again);
            check(u.has_value(), u ? "unpack it" : u.error().message);
            check(fs::file_size(again / "assets/world.bin") == size && read(again / "assets/world.bin") == read(big / "assets/world.bin"),
                  "the same bytes, unpacked a chunk at a time");
            check(!fs::exists(again / "assets/world.bin.part"), "no .part left");
        }
    }

    // Unpack, and pack again: the same folder, the same digest.
    const fs::path out = root() / "unpacked";
    auto n = unpack(*a, out);
    check(n && *n == 6, n ? "unpack" : n.error().message);
    auto want = tree(src);
    want.erase(".squared/journal");
    want.erase(".git/HEAD");
    want.erase(".anytool/cache");
    auto got = tree(out);
    check(got.erase("SQ-INF/entries.json") == 1, "unpack writes entries.json");
    want.erase("SQ-INF/entries.json");
    check(got == want, "the same files, the same bytes");
    const fs::path again = root() / "again.sq";
    auto c2 = pack(out, again, quick());
    check(c2 && c->content_digest == c2->content_digest, "pack, unpack, pack: the same content digest");
    check(!unpack(*a, out), "unpack refuses a folder that is not empty");

    // Links: pack refuses one; unpack never writes through one.
    {
        const fs::path linked = root() / "linked";
        write(linked / "SQ-INF/manifest.json", text(manifest()));
        fs::create_directories(root() / "outside");
        write(root() / "outside/secret", "do not pack me");
        fs::create_symlink(root() / "outside/secret", linked / "secret");
        auto r = pack(linked, root() / "linked.sq", quick());
        check(!r && r.error().code == Code::path, "pack refuses a symbolic link (R12)");
        auto rf = Folder::open(linked);
        check(!rf && rf.error().code == Code::path, "and so does the folder reader");

        const fs::path trap = root() / "trap";
        fs::create_directories(trap);
        fs::create_directory_symlink(root() / "outside", trap / "regions");
        auto u = unpack(*a, trap, {.merge = true});
        check(!u, "unpack refuses to write through a planted link (R12)");
        check(!fs::exists(root() / "outside/north"), "and wrote nothing outside");
    }

    // A Mac's decomposed name is packed as NFC (§6.1).
    {
        const fs::path mac = root() / "mac";
        write(mac / "SQ-INF/manifest.json", text(manifest()));
        write(mac / "cafe\u0301.txt", "coffee");
        write(mac / "SQ-INF/entries.json", R"({"cafe\u0301.txt":{"title":"Coffee"}})");
        auto r = pack(mac, root() / "mac.sq", quick());
        check(r.has_value(), r ? "pack a decomposed name" : r.error().message);
        auto m = Archive::open(root() / "mac.sq");
        check(m && m->find("caf\u00e9.txt") && m->find("caf\u00e9.txt")->title == "Coffee", "stored as NFC, its metadata found");
    }

    // Orphans.
    {
        const fs::path orphan = root() / "orphan";
        write(orphan / "SQ-INF/manifest.json", text(manifest()));
        write(orphan / "SQ-INF/entries.json", R"({"gone.txt":{"title":"renamed away"}})");
        check(!pack(orphan, root() / "orphan.sq", quick()), "an orphan in entries.json is refused");
        check(pack(orphan, root() / "orphan2.sq", quick(), {.drop_orphans = true}).has_value(), "or dropped when asked");
    }

    // Names Windows cannot create.
    {
        auto b = simple({{.path = "aux/CON.txt", .content = bytes("x")}});
        auto w = Archive::open_memory(b.file());
        check(w.has_value(), "a cartridge with CON.txt opens");
        if (w) check(!unpack(*w, root() / "windows"), "but does not unpack (§4.1)");
    }

    fs::remove_all(root());
    return report("exploded");
}
