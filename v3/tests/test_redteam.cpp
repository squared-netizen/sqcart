// SPDX-License-Identifier: MIT
//
// The red-team review's findings (docs/developer/specs/sq-cartridge-3.0-red-team.md),
// one hostile or broken cartridge each, and what the reader must do with it.

import std;
import sq.cart;
import sq.cart.testing;

using namespace sq::cart;
using namespace sq::cart::testing;

namespace {

bool has(const CheckReport& r, Finding::Severity s, std::string_view words) {
    return std::ranges::any_of(r.findings, [&](const Finding& f) { return f.severity == s && f.message.find(words) != std::string::npos; });
}

/// Refused by the reader: at open, or when its whole state is asked for
/// (entries(), §12), with `code` (or any code, when none is given).
bool refused(const std::vector<std::byte>& file, std::optional<Code> code = std::nullopt) {
    auto a = Archive::open_memory(file);
    if (!a) return !code || a.error().code == *code;
    auto live = a->entries();
    return !live && (!code || live.error().code == *code);
}

std::vector<std::byte> quick() {
    std::string t;
    for (int i = 0; i < 40; ++i) t += "The quick brown fox jumps over the lazy dog. ";
    return bytes(t);
}

}  // namespace

int main() {
    using enum Finding::Severity;

    // R1: slot B torn by a crash while commit 2 was named. Slot A, commit 1, is in charge.
    {
        auto b = simple({{.path = "a", .content = bytes("one")}});
        b.commit({{.path = "a", .content = bytes("two")}}, {.full = false});
        b.file()[4096 + 3] ^= std::byte{0xff};
        auto cart = Archive::open_memory(b.file());
        check(cart && cart->commit().sequence == 1, "R1: a torn slot falls back to the other");
        if (cart) check(text(*cart->read(*cart->find("a"))) == "one", "R1: and reads commit 1's state");
        check(has(check_memory(b.file()), warning, "checksum fails"), "R1: check reports the torn slot");
        check(has(check_memory(b.file()), warning, "after the current commit"), "R1: and commit 2's bytes as trailing");
        b.file()[64 + 3] ^= std::byte{0xff};
        check(!Archive::open_memory(b.file()), "R1: with both slots torn, it is refused");
    }
    // R2: a required feature bit this reader does not know; an optional one is fine.
    {
        auto b = simple({}, {.features = std::uint64_t{1} << 5});
        auto r = Archive::open_memory(b.file());
        check(!r && r.error().code == Code::unsupported, "R2: an unknown required feature is refused");
        auto o = simple({}, {.features = std::uint64_t{1} << 40});
        check(Archive::open_memory(o.file()).has_value(), "R2: an unknown optional feature is not");
    }
    // R5: frames that lie or carry extra.
    {
        auto nosize = simple({{.path = "t", .content = quick(), .codec = Codec::zstd, .frame = bytes(text_nosize_zst)}});
        auto c = Archive::open_memory(nosize.file());
        check(c && !c->read(*c->find("t")), "R5: a frame with no content size is refused");
        auto extra = bytes(text_zst);
        extra.push_back(std::byte{0});
        auto trailing = simple({{.path = "t", .content = quick(), .codec = Codec::zstd, .frame = extra}});
        c = Archive::open_memory(trailing.file());
        check(c && !c->read(*c->find("t")), "R5: bytes after the frame are refused");
        auto skippable = bytes(text_zst);
        const std::uint8_t skip[] = {0x50, 0x2a, 0x4d, 0x18, 1, 0, 0, 0, 0x41};
        for (auto x : skip) skippable.push_back(static_cast<std::byte>(x));
        auto sk = simple({{.path = "t", .content = quick(), .codec = Codec::zstd, .frame = skippable}});
        c = Archive::open_memory(sk.file());
        check(c && !c->read(*c->find("t")), "R5: a skippable frame after it is refused");
        auto wrong_size = simple({{.path = "t", .content = bytes("short"), .codec = Codec::zstd, .frame = bytes(text_zst)}});
        c = Archive::open_memory(wrong_size.file());
        check(c && !c->read(*c->find("t")), "R5: a frame whose size is not the entry's is refused");
        ReadOptions tiny;
        tiny.limits.max_window_log = 10;
        auto good = simple({{.path = "t", .content = quick(), .codec = Codec::zstd, .frame = bytes(text_zst)}});
        c = Archive::open_memory(good.file(), tiny);
        auto r = c ? c->read(*c->find("t")) : std::unexpected(Error{});
        check(c && !r && r.error().code == Code::unsupported, "R5: a window past the reader's limit is refused");
    }
    // R7: a frame for one dictionary, a record naming another; a dictionary missing.
    {
        auto b = simple({{.path = "SQ-INF/dictionaries/40000.zdict", .content = bytes(dict_40000), .flags = flag::dictionary},
                         {.path = "r", .content = bytes(record_json), .codec = Codec::zstd_dict, .frame = bytes(record_zst), .dictionary = 40001}});
        auto c = Archive::open_memory(b.file());
        check(c && !c->read(*c->find("r")), "R7: a dictionary ID that is not the frame's is refused");
        auto m = simple({{.path = "r", .content = bytes(record_json), .codec = Codec::zstd_dict, .frame = bytes(record_zst), .dictionary = 40000}});
        c = Archive::open_memory(m.file());
        check(c && !c->read(*c->find("r")), "R7: a missing dictionary is refused");
        check(has(check_memory(m.file()), error, "no dictionary 40000"), "R7: check says which");
    }
    // R9: a delta whose previous commit is after it.
    {
        auto b = simple({{.path = "a", .content = bytes("x")}});
        b.commit({{.path = "b", .content = bytes("y")}}, {.full = false, .edit_record = [](std::span<std::byte> r) {
                                                               for (int i = 0; i < 8; ++i) r[16 + i] = std::byte{0xff};
                                                           }});
        check(!Archive::open_memory(b.file()), "R9: a previous commit after this one is refused");
    }
    // R10: two entries partly over the same bytes.
    {
        auto b = simple({{.path = "a", .content = bytes("0123456789abcdefgh")}});
        const std::uint64_t run = b.runs["a"];
        b.commit({{.path = "b", .content = bytes("23456789xy"), .reuse_run = run + 2}}, {.full = false});
        const auto r = check_memory(b.file());
        check(has(r, error, "partly overlap"), "R10: overlapping runs are an error");
    }
    // R11: bytes appended after the last commit (a ZIP, say).
    {
        auto b = simple({});
        const auto zip = bytes("PK\x05\x06 an end of central directory");
        b.file().insert(b.file().end(), zip.begin(), zip.end());
        check(Archive::open_memory(b.file()).has_value(), "R11: trailing bytes do not stop a reader");
        check(has(check_memory(b.file()), warning, "after the current commit"), "R11: check reports them");
    }
    // R12 / §4.1: paths.
    for (const char* bad : {"../escape", "/abs", "a//b", "a/./b", "a\\b", "c:x", "tab\there", "cafe\u0301.txt"}) {
        auto b = simple({{.path = bad, .content = bytes("x")}});
        check(refused(b.file(), Code::path), std::format("R12: the path \"{}\" is refused", bad));
        auto a = Archive::open_memory(b.file());
        auto e = a ? a->find(bad) : std::unexpected(Error{});
        check(!e && e.error().code == Code::path, std::format("R12: and find(\"{}\") refuses its record", bad));
    }
    // R13: paths that collide on a case-insensitive file system.
    {
        auto b = simple({{.path = "Readme.md", .content = bytes("1")}, {.path = "README.md", .content = bytes("2")}});
        check(refused(b.file(), Code::path), "R13: README.md and Readme.md collide");
        auto s = simple({{.path = "straße", .content = bytes("1")}, {.path = "STRASSE", .content = bytes("2")}});
        check(refused(s.file(), Code::path), "R13: straße and STRASSE collide");
    }
    // R19: a manifest that is not I-JSON.
    {
        Builder b;
        b.commit({{.path = "SQ-INF/manifest.json",
                   .content = bytes(R"({"format":"squared-cartridge","format_version":3,"kind":"data","id":"a","id":"b","version":"1"})")}});
        auto r = Archive::open_memory(b.file());
        check(!r && r.error().code == Code::json, "R19: a manifest with a member twice is refused");
    }
    // R21: signatures.
    {
        std::array<std::byte, 32> seed{};
        seed[1] = std::byte{42};
        auto b = simple({{.path = "a", .content = bytes("signed")}}, {.signing_seeds = {seed}, .signer = "Jesse"});
        auto c = Archive::open_memory(b.file());
        auto sigs = c ? c->signatures() : std::unexpected(Error{});
        check(sigs && sigs->size() == 1 && (*sigs)[0].state == SignatureState::valid && (*sigs)[0].signature.signer == "Jesse",
              "R21: a valid signature verifies");
        check(check_memory(b.file()).signatures_valid == 1, "R21: check counts it");
        // The signer's name is signed: change it and the signature fails.
        auto renamed = b.file();
        const std::uint64_t block = c->commit().signatures_offset;
        renamed[block + 4 + 4 + 32 + 8 + 4] = std::byte{'Z'};
        auto c2 = Archive::open_memory(renamed);
        auto s2 = c2 ? c2->signatures() : std::unexpected(Error{});
        check(s2 && (*s2)[0].state == SignatureState::invalid, "R21: a renamed signer breaks the signature");
        check(has(check_memory(renamed), error, "invalid"), "R21: check reports an invalid signature");
        auto unknown = b.file();
        unknown[block + 4] = std::byte{9};
        auto c3 = Archive::open_memory(unknown);
        auto s3 = c3 ? c3->signatures() : std::unexpected(Error{});
        check(s3 && (*s3)[0].state == SignatureState::unknown_algorithm, "R21: an unknown algorithm is reported, not trusted");
    }
    // §9.2: a commit whose content digest is not its state's.
    {
        auto b = simple({{.path = "a", .content = bytes("x")}}, {.content_digest = Digest{}});
        check(Archive::open_memory(b.file()).has_value(), "a wrong content digest does not stop a reader");
        check(has(check_memory(b.file()), error, "content digest"), "but check reports it");
    }
    // §9.1: content that does not match its digest.
    {
        auto b = simple({{.path = "a", .content = bytes("hello")}});
        b.file()[b.runs["a"]] = std::byte{'j'};
        auto c = Archive::open_memory(b.file());
        auto r = c ? c->read(*c->find("a")) : std::unexpected(Error{});
        check(!r && r.error().code == Code::integrity, "changed content is refused, not returned");
    }
    // §4.2: attributes not in canonical form are a warning; mappable must align.
    {
        auto b = simple({{.path = "a", .content = bytes("x"), .attributes = R"({ "b": 1, "a": 2 })"}});
        check(has(check_memory(b.file()), warning, "canonical"), "attributes not in canonical form are reported");
        auto m = simple({{.path = "font.ttf", .content = bytes("glyphs"), .flags = flag::r | flag::mappable}});
        check(Archive::open_memory(m.file()).has_value(), "a mappable entry at 16384 opens");
        auto mis = simple({{.path = "font.ttf", .content = bytes("glyphs"), .flags = flag::r | flag::mappable}},
                          {.edit_entry = [](const std::string& path, std::span<std::byte, 128> r) {
                               if (path == "font.ttf") r[32] = std::byte{64};  // offset 16384 + 64
                           }});
        check(refused(mis.file()), "a mappable entry off a 16384 boundary is refused");
    }
    // §7: no manifest, a live count that lies.
    {
        Builder b;
        b.commit({{.path = "a", .content = bytes("x")}});
        check(!Archive::open_memory(b.file()), "no manifest: refused");
        auto lie = simple({}, {.live_count = 7});
        check(refused(lie.file()), "a live count that lies: refused");
    }
    return report("redteam");
}
