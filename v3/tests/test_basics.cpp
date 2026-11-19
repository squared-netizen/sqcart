// SPDX-License-Identifier: MIT
//
// A cartridge with stored, zstd, dictionary and empty entries: opened from
// memory and from a file, looked up, read, its digest recomputed, checked.

import std;
import sq.cart;
import sq.cart.testing;

using namespace sq::cart;
using namespace sq::cart::testing;

int main() {
    auto b = simple({
        {.path = "README.md", .content = bytes("# hello\n"), .title = "Read me", .media_type = "text/markdown"},
        {.path = "text/fox.txt", .content = {}},
        {.path = "text/quick.txt",
         .content = [] {
             std::string t;
             for (int i = 0; i < 40; ++i) t += "The quick brown fox jumps over the lazy dog. ";
             return bytes(t);
         }(),
         .codec = Codec::zstd,
         .frame = bytes(text_zst)},
        {.path = "SQ-INF/dictionaries/40000.zdict", .content = bytes(dict_40000), .flags = flag::dictionary},
        {.path = "world/settlements/9001.json",
         .content = bytes(record_json),
         .flags = flag::r | flag::w,
         .codec = Codec::zstd_dict,
         .frame = bytes(record_zst),
         .dictionary = 40000,
         .author = "Jesse",
         .attributes = R"({"name":"Northford","population":41250})",
         .created = 1794960000000,
         .updated = 1794960000001},
    });

    auto cart = Archive::open_memory(b.file());
    check(cart.has_value(), cart ? "opens" : cart.error().message);
    if (!cart) return report("basics");

    check(cart->entries()->size() == 6, "six live entries");
    check(cart->manifest().id == "data.test-world" && cart->manifest().kind == "data", "the manifest's id and kind");
    check(cart->trailing_bytes() == 0, "no trailing bytes");
    check(cart->commit().sequence == 1 && cart->chain().size() == 1, "one commit");

    const auto readme = cart->find("README.md");
    check(readme && readme->title == "Read me", "a title");
    if (readme) check(text(*cart->read(*readme)) == "# hello\n", "a stored entry reads back");

    const auto quick = cart->find("text/quick.txt");
    check(quick && quick->codec == Codec::zstd, "a zstd entry");
    if (quick) {
        auto content = cart->read(*quick);
        check(content && content->size() == 1800 && text(*content).starts_with("The quick brown fox"), "a zstd entry decodes");
        auto range = cart->read_range(*quick, 4, 5);
        check(range && text(*range) == "quick", "a range of a zstd entry");
    }

    const auto record = cart->find("world/settlements/9001.json");
    check(record && record->codec == Codec::zstd_dict && record->dictionary == 40000, "a dictionary entry");
    if (record) {
        auto content = cart->read(*record);
        check(content && text(*content) == record_json, content ? "a dictionary entry decodes" : content.error().message);
    }

    const auto empty = cart->find("text/fox.txt");
    check(empty && empty->size == 0, "an empty entry");
    if (empty) check(cart->read(*empty)->empty(), "reads as nothing");

    check(!cart->find("README.MD"), "lookups are case-sensitive");
    check(!cart->find("nothing"), "a missing entry is nothing");

    const auto digest = cart->compute_content_digest();
    check(digest && *digest == cart->commit().content_digest, "the content digest matches the commit's");

    auto report_ = check_memory(b.file());
    check(report_.ok() && report_.findings.empty(), "check finds nothing");
    for (const auto& f : report_.findings) std::println(std::cerr, "  finding: {} {}", f.message, f.entry.value_or(""));
    check(report_.entries_checked == 6, "check reads every entry");

    // The same bytes, from a file.
    const auto path = std::filesystem::temp_directory_path() / std::format("sqcart-basics-{}.sq", std::chrono::steady_clock::now().time_since_epoch().count());
    {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(b.file().data()), static_cast<std::streamsize>(b.file().size()));
    }
    auto from_file = Archive::open(path);
    check(from_file && from_file->entries()->size() == 6, "opens from a file");
    if (from_file) check(text(*from_file->read(*from_file->find("README.md"))) == "# hello\n", "reads from a file");
    std::filesystem::remove(path);

    // Not a 3.0 cartridge.
    std::vector<std::byte> zip(9000);
    zip[0] = std::byte{'P'}, zip[1] = std::byte{'K'};
    auto refused = Archive::open_memory(zip);
    check(!refused && refused.error().code == Code::malformed, "a ZIP is not a 3.0 cartridge");
    return report("basics");
}
