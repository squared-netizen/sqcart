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
    write(src / "SQ-INF/entries.json",
          R"({"README.md":{"title":"Read me"},"regions/north/settlement.json":{"flags":"rw","media_type":"application/json","created":1794960000000,"attributes":{"population":41250,"name":"Northford"}}})");

    // Pack.
    const fs::path cart = root() / "world.sq";
    auto c = pack(src, cart, quick());
    check(c.has_value(), c ? "pack" : c.error().message);
    auto a = Archive::open(cart);
    check(a.has_value(), "the packed cartridge opens");
    if (!a) return report("exploded");
    check(a->entries().size() == 4, "manifest, README, tile, settlement: four entries");
    check(!a->find(".squared/journal") && !a->find(".git/HEAD"), ".squared/ and .git/ are never entries");
    check(!a->find("SQ-INF/entries.json"), "entries.json is not an entry");
    check(a->find("README.md")->title == "Read me", "a title from entries.json");
    const auto s = a->find("regions/north/settlement.json");
    check(s && s->flags == (flag::r | flag::w) && s->created == 1794960000000, "flags and times from entries.json");
    check(s && s->attributes == R"({"name":"Northford","population":41250})", "attributes, canonical");
    check(check(cart).ok(), "check passes");

    // Unpack, and pack again: the same folder, the same digest.
    const fs::path out = root() / "unpacked";
    auto n = unpack(*a, out);
    check(n && *n == 4, n ? "unpack" : n.error().message);
    auto want = tree(src);
    want.erase(".squared/journal");
    want.erase(".git/HEAD");
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

        const fs::path trap = root() / "trap";
        fs::create_directories(trap);
        fs::create_directory_symlink(root() / "outside", trap / "regions");
        auto u = unpack(*a, trap, {.merge = true});
        check(!u, "unpack refuses to write through a planted link (R12)");
        check(!fs::exists(root() / "outside/north"), "and wrote nothing outside");
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
