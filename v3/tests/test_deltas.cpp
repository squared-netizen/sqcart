// SPDX-License-Identifier: MIT
//
// Commits after the first (§3.4, §3.5): delta indexes that add, replace and
// delete; a metadata-only change that names the old run (§3.6); the chain
// a lookup walks; and its limit.

import std;
import sq.cart;
import sq.cart.testing;

using namespace sq::cart;
using namespace sq::cart::testing;

int main() {
    auto b = simple({{.path = "a.txt", .content = bytes("first a")}, {.path = "b.txt", .content = bytes("first b")}});
    const std::uint64_t a_run = b.runs["a.txt"];
    // Commit 2: replace a, delete b, add c.
    b.commit({{.path = "a.txt", .content = bytes("second a")},
              {.path = "b.txt", .tombstone = true},
              {.path = "c.txt", .content = bytes("new c")}},
             {.full = false});
    // Commit 3: retitle c, naming its run again (no copy).
    const std::uint64_t c_run = b.runs["c.txt"];
    b.commit({{.path = "c.txt", .content = bytes("new c"), .title = "See", .reuse_run = c_run}}, {.full = false});

    auto cart = Archive::open_memory(b.file());
    check(cart.has_value(), cart ? "opens after three commits" : cart.error().message);
    if (!cart) return report("deltas");
    check(cart->commit().sequence == 3 && cart->commit().chain_length == 2, "commit 3, two deltas");
    check(cart->chain().size() == 3, "the chain reaches the full index");
    check(text(*cart->read(*cart->find("a.txt"))) == "second a", "a replaced entry reads its new content");
    check(!cart->find("b.txt"), "a tombstone deletes");
    const auto c = cart->find("c.txt");
    check(c && c->title == "See" && c->offset == c_run, "a metadata-only change names the old run");
    if (c) check(text(*cart->read(*c)) == "new c", "and reads it");
    check(cart->entries()->size() == 3, "manifest, a and c are live");
    check(check_memory(b.file()).ok(), "check passes");

    // Old content stays in the file, unreachable (§3.6).
    check(text(std::vector<std::byte>(b.file().begin() + static_cast<std::ptrdiff_t>(a_run),
                                      b.file().begin() + static_cast<std::ptrdiff_t>(a_run + 7))) == "first a",
          "replaced content is still in the file");

    // The chain limit: a reader with a limit of 1 refuses a chain of 2.
    ReadOptions strict;
    strict.limits.max_delta_chain = 1;
    auto refused = Archive::open_memory(b.file(), strict);
    check(!refused && refused.error().code == Code::limit, "a chain past the limit is refused");

    // A tombstone in a full index is refused (§3.5).
    Builder t;
    t.commit({{.path = "SQ-INF/manifest.json", .content = manifest()}, {.path = "gone", .tombstone = true}});
    {
        auto a = Archive::open_memory(t.file());
        check(!a || !a->entries(), "a tombstone in a full index is refused");
        check(!a || (!a->find("gone") && a->find("gone").error().code == Code::malformed), "and find() says why, not just not found");
    }

    // §3.6: a run may name data an earlier commit wrote, even one before the
    // full index a lookup stops at; never anything else of that commit.
    {
        auto f = simple({{.path = "a", .content = bytes("first a")}});
        const std::uint64_t a_run = f.runs["a"];
        f.commit({{.path = "SQ-INF/manifest.json", .content = manifest(), .reuse_run = f.runs["SQ-INF/manifest.json"]},
                  {.path = "a", .content = bytes("first a"), .reuse_run = a_run}});  // commit 2: full, naming commit 1's runs
        auto ok = Archive::open_memory(f.file());
        check(ok && ok->chain().size() == 1 && ok->find("a") && *ok->read(*ok->find("a")) == bytes("first a"),
              "a full index naming data from a commit before it reads");
        auto g = simple({{.path = "a", .content = bytes("first a")}});
        const std::uint64_t g_index = Archive::open_memory(g.file())->commit().index_offset;
        g.commit({{.path = "SQ-INF/manifest.json", .content = manifest(), .reuse_run = g.runs["SQ-INF/manifest.json"]},
                  {.path = "a", .content = bytes("first a"), .reuse_run = g_index}});
        auto bad = Archive::open_memory(g.file());
        check(bad && !bad->find("a") && bad->find("a").error().code == Code::malformed,
              "a run pointing into an older commit's index is refused (all commits' regions are walked)");
    }

    // A delta whose chain length does not follow its predecessor's.
    auto d = simple({{.path = "a", .content = bytes("x")}});
    d.commit({{.path = "b", .content = bytes("y")}},
             {.full = false, .edit_record = [](std::span<std::byte> r) { r[92] = std::byte{5}; }});
    check(!Archive::open_memory(d.file()), "a chain length out of step is refused");
    return report("deltas");
}
