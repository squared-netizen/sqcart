// SPDX-License-Identifier: MIT
//
// Chunked entries (§4.5): read in pieces, each verified (§9.1), a broken
// chunk caught when it is read and not before, a chunk table that does not
// match the digest refused.

import std;
import sq.cart;
import sq.cart.testing;

using namespace sq::cart;
using namespace sq::cart::testing;

int main() {
    // 3.5 MiB: three whole chunks and a half one.
    std::vector<std::byte> big(chunk_size * 7 / 2);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::byte>((i * 2654435761u) >> 13);
    auto b = simple({{.path = "world.bin", .content = big, .codec = Codec::chunked}});

    auto cart = Archive::open_memory(b.file());
    check(cart.has_value(), cart ? "opens" : cart.error().message);
    if (!cart) return report("chunks");
    check((cart->commit().features & feature::chunked) != 0, "the commit declares chunked");
    const auto e = cart->find("world.bin");
    check(e && e->codec == Codec::chunked && e->size == big.size(), "a chunked entry");
    if (!e) return report("chunks");
    check(e->digest == tree_hash(big), "its digest is the tree hash");
    auto all = cart->read(*e);
    check(all && *all == big, "reads whole");
    auto across = cart->read_range(*e, chunk_size - 3, 6);
    check(across && std::equal(across->begin(), across->end(), big.begin() + static_cast<std::ptrdiff_t>(chunk_size - 3)),
          "a range across a chunk boundary");
    auto tail = cart->read_range(*e, big.size() - 10, 10);
    check(tail && tail->size() == 10, "the last bytes");
    check(!cart->read_range(*e, big.size() - 1, 2), "past the end is refused");
    check(check_memory(b.file()).ok(), "check passes");

    // Break one byte of chunk 2: reads of chunk 0 still work, chunk 2 fails.
    auto broken = b.file();
    broken[b.chunk_runs["world.bin"][2] + 100] ^= std::byte{1};
    auto c2 = Archive::open_memory(broken);
    check(c2.has_value(), "a broken chunk does not stop the open");
    if (c2) {
        const auto e2 = c2->find("world.bin");
        check(c2->read_range(*e2, 0, 100).has_value(), "chunk 0 still reads");
        auto bad = c2->read_range(*e2, chunk_size * 2, 100);
        check(!bad && bad.error().code == Code::integrity, "chunk 2 fails its hash");
        check(!check_memory(broken).ok(), "check reports it");
    }

    // A chunk table that does not match the digest.
    auto lie = b.file();
    const std::uint64_t table = b.runs["world.bin"];
    lie[table + 32] ^= std::byte{1};  // chunk 0's hash
    auto c3 = Archive::open_memory(lie);
    if (c3) {
        auto r = c3->read_range(*c3->find("world.bin"), chunk_size * 3, 1);
        check(!r && r.error().code == Code::integrity, "a chunk table that does not match the digest is refused, whatever is read");
    }
    return report("chunks");
}
