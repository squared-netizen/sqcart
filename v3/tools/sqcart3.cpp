// SPDX-License-Identifier: MIT
//
// sqcart3: the 3.0 reader on the command line.
//
//   sqcart3 check FILE        validate (§12): every entry read and verified
//   sqcart3 ls FILE           the live entries: flags, size, codec, path, title
//   sqcart3 cat FILE PATH     an entry's content, verified, to stdout
//   sqcart3 info FILE         the commit, the manifest, the signatures

import std;
import sq.cart;

using namespace sq::cart;

namespace {

int usage() {
    std::println(std::cerr, "usage: sqcart3 check FILE\n       sqcart3 ls FILE\n       sqcart3 cat FILE PATH\n       sqcart3 info FILE");
    return 2;
}

int fail(const Error& e) {
    std::println(std::cerr, "sqcart3: {}{}{}", e.entry ? *e.entry : "", e.entry ? ": " : "", e.message);
    return 1;
}

std::string letters(std::uint16_t f) {
    std::string s;
    s += (f & flag::r) ? 'r' : '-';
    s += (f & flag::w) ? 'w' : '-';
    s += (f & flag::x) ? 'x' : '-';
    s += (f & flag::mappable) ? 'm' : '-';
    s += (f & flag::dictionary) ? 'd' : '-';
    return s;
}

std::string_view codec(Codec c) {
    switch (c) {
        case Codec::stored: return "stored";
        case Codec::zstd: return "zstd";
        case Codec::zstd_dict: return "zstd+dict";
        case Codec::chunked: return "chunked";
    }
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() < 2) return usage();
    const std::string_view cmd = args[0];
    const std::filesystem::path file(args[1]);

    if (cmd == "check" && args.size() == 2) {
        const CheckReport r = check(file);
        for (const Finding& f : r.findings) {
            std::println("{} {}{}{}", f.severity == Finding::Severity::error ? "error  " : "warning", f.entry ? *f.entry : "",
                         f.entry ? ": " : "", f.message);
        }
        if (r.commit) {
            std::println("{}: commit {}, {} entries read and verified, {} valid signature(s): {}", file.string(), r.commit->sequence,
                         r.entries_checked, r.signatures_valid, r.ok() ? "conforming" : "NOT conforming");
        }
        return r.ok() ? 0 : 1;
    }
    auto cart = Archive::open(file);
    if (!cart) return fail(cart.error());
    if (cmd == "ls" && args.size() == 2) {
        for (const Entry& e : cart->entries()) {
            std::println("{} {:>12} {:<9} {}{}", letters(e.flags), e.size, codec(e.codec), e.path, e.title ? "  \"" + *e.title + "\"" : "");
        }
        return 0;
    }
    if (cmd == "cat" && args.size() == 3) {
        const auto e = cart->find(args[2]);
        if (!e) return fail(Error{Code::not_found, "no such entry", std::string(args[2])});
        auto bytes = cart->read(*e);
        if (!bytes) return fail(bytes.error());
        std::cout.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
        return 0;
    }
    if (cmd == "info" && args.size() == 2) {
        const Commit& c = cart->commit();
        std::println("id          {}\nversion     {}\nkind        {}", cart->manifest().id, cart->manifest().version, cart->manifest().kind);
        std::println("commit      {} ({} index, delta chain {}), {} live entries", c.sequence,
                     c.index_kind == IndexKind::full ? "full" : "delta", c.chain_length, c.live_count);
        std::println("digest      {}", to_hex(c.content_digest));
        std::println("file        {} bytes, {} trailing", cart->file_size(), cart->trailing_bytes());
        auto sigs = cart->signatures();
        if (!sigs) return fail(sigs.error());
        if (sigs->empty()) std::println("signatures  none (unsigned)");
        for (const SignatureCheck& s : *sigs) {
            std::println("signature   key {} {}{}: {}", to_hex(s.signature.key_id), s.signature.signer.empty() ? "" : "\"" + s.signature.signer + "\" ",
                         s.signature.algorithm == 1 ? "Ed25519" : "unknown",
                         s.state == SignatureState::valid ? "valid" : s.why);
        }
        return 0;
    }
    return usage();
}
