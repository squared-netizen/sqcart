// SPDX-License-Identifier: MIT
//
// sqcart3: format 3.0 on the command line.
//
//   sqcart3 check FILE                 validate (§12): every entry read and verified
//   sqcart3 ls FILE                    the live entries: flags, size, codec, path, title
//   sqcart3 cat FILE PATH              an entry's content, verified, to stdout
//   sqcart3 info FILE                  the commit, the manifest, the signatures
//   sqcart3 unpack FILE DIR [--merge]  the exploded form (§6.1): files, and SQ-INF/entries.json
//
//   sqcart3 new FILE MANIFEST          a new cartridge, its manifest from a file
//   sqcart3 put FILE PATH SOURCE       an entry from a file (- for stdin), committed
//   sqcart3 rm FILE PATH               an entry removed, committed
//   sqcart3 mv FILE FROM TO            an entry renamed, its content not copied (§3.6), committed
//   sqcart3 truncate FILE PATH SIZE    an entry cut or grown with zeros, committed
//   sqcart3 compact FILE               one commit, its live entries only (§10.4)
//   sqcart3 pack DIR FILE [--drop-orphans]  an exploded cartridge packed, as one commit
//   sqcart3 keygen KEYFILE             an Ed25519 signing key (a 32-byte seed, in hex, mode 0600)
//
// Options for new, put and rm:
//   --title T --author A --media-type M --flags rwxm --store auto|stored|zstd|chunked
//   --sign KEYFILE [--signer NAME]     sign the commit (§9.3)
//   --fast                             zstd level 3, not 19
//
// Built with -DSQCART3_READ_ONLY it has the first four only, and links no
// compressor (the Makefile's sqcart3-read: proof that a reader carries none).

import std;
import sq.cart;
#ifndef SQCART3_READ_ONLY
import sq.cart.write;
#endif

using namespace sq::cart;

namespace {

int usage() {
    std::println(std::cerr, "usage: sqcart3 check|ls|info FILE\n"
                            "       sqcart3 cat FILE PATH\n"
                            "       sqcart3 unpack FILE DIR [--merge]\n"
#ifndef SQCART3_READ_ONLY
                            "       sqcart3 new FILE MANIFEST [options]\n"
                            "       sqcart3 put FILE PATH SOURCE [options]\n"
                            "       sqcart3 rm FILE PATH [options]\n"
                            "       sqcart3 mv FILE FROM TO [options]\n"
                            "       sqcart3 truncate FILE PATH SIZE [options]\n"
                            "       sqcart3 compact FILE\n"
                            "       sqcart3 pack DIR FILE [--drop-orphans] [--sign KEYFILE --signer NAME]\n"
                            "       sqcart3 keygen KEYFILE\n"
                            "options: --title T --author A --media-type M --flags rwxm\n"
                            "         --store auto|stored|zstd|chunked --sign KEYFILE --signer NAME --fast"
#endif
    );
    return 2;
}

int fail(const Error& e) {
    std::println(std::cerr, "sqcart3: {}{}{}", e.entry ? *e.entry : "", e.entry ? ": " : "", e.message);
    return 1;
}

[[maybe_unused]] int fail(std::string_view message) {
    std::println(std::cerr, "sqcart3: {}", message);
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

int read_commands(std::string_view cmd, const std::vector<std::string_view>& args) {
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
    if (cmd == "unpack" && (args.size() == 3 || (args.size() == 4 && args[3] == "--merge"))) {
        auto n = unpack(*cart, std::filesystem::path(args[2]), {.merge = args.size() == 4});
        if (!n) return fail(n.error());
        std::println("{}: {} entries unpacked into {}", file.string(), *n, args[2]);
        return 0;
    }
    if (cmd == "ls" && args.size() == 2) {
        auto live = cart->entries();
        if (!live) return fail(live.error());
        for (const Entry& e : *live) {
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

#ifndef SQCART3_READ_ONLY

std::optional<std::vector<std::byte>> slurp(std::string_view source) {
    std::vector<char> text;
    if (source == "-") {
        text.assign(std::istreambuf_iterator<char>(std::cin), {});
    } else {
        std::ifstream in{std::filesystem::path(source), std::ios::binary};
        if (!in) return std::nullopt;
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    const auto b = std::as_bytes(std::span(text));
    return std::vector<std::byte>(b.begin(), b.end());
}

struct Options {
    PutOptions put;
    std::optional<std::string> key_file;
    std::string signer;
    bool fast = false;
};

std::optional<Options> parse_options(std::span<const std::string_view> rest) {
    Options o;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        const std::string_view a = rest[i];
        const bool has_value = i + 1 < rest.size();
        const auto value = [&] { return std::string(rest[++i]); };
        if (a == "--fast") {
            o.fast = true;
        } else if (!has_value) {
            return std::nullopt;
        } else if (a == "--title") {
            o.put.title = value();
        } else if (a == "--author") {
            o.put.author = value();
        } else if (a == "--media-type") {
            o.put.media_type = value();
        } else if (a == "--sign") {
            o.key_file = value();
        } else if (a == "--signer") {
            o.signer = value();
        } else if (a == "--flags") {
            o.put.flags = 0;
            for (const char c : rest[++i]) {
                if (c == 'r') o.put.flags |= flag::r;
                else if (c == 'w') o.put.flags |= flag::w;
                else if (c == 'x') o.put.flags |= flag::x;
                else if (c == 'm') o.put.flags |= flag::mappable;
                else return std::nullopt;
            }
        } else if (a == "--store") {
            const std::string_view s = rest[++i];
            if (s == "auto") o.put.store = Store::automatic;
            else if (s == "stored") o.put.store = Store::stored;
            else if (s == "zstd") o.put.store = Store::zstd;
            else if (s == "chunked") o.put.store = Store::chunked;
            else return std::nullopt;
        } else {
            return std::nullopt;
        }
    }
    return o;
}

std::optional<SigningKey> load_key(const std::string& file, const std::string& signer) {
    std::ifstream in{std::filesystem::path(file)};
    std::string hex;
    in >> hex;
    if (hex.size() != 64) return std::nullopt;
    std::array<std::byte, 32> seed{};
    for (std::size_t i = 0; i < 32; ++i) {
        unsigned v = 0;
        if (std::from_chars(hex.data() + 2 * i, hex.data() + 2 * i + 2, v, 16).ec != std::errc{}) return std::nullopt;
        seed[i] = static_cast<std::byte>(v);
    }
    return SigningKey::from_seed(seed, signer);
}

int write_commands(std::string_view cmd, const std::vector<std::string_view>& args) {
    const std::filesystem::path file(args[1]);
    if (cmd == "keygen" && args.size() == 2) {
        if (std::filesystem::exists(file)) return fail(file.string() + " is there already");
        std::random_device random;
        std::string hex;
        for (int i = 0; i < 8; ++i) hex += std::format("{:08x}", random());
        {
            std::ofstream out(file);
            out << hex << '\n';
            if (!out) return fail("cannot write " + file.string());
        }
        std::filesystem::permissions(file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        const auto key = load_key(file.string(), {});
        const Digest id = sha256(key->public_key);
        std::println("{}: an Ed25519 key, id {} (keep the file secret)", file.string(), to_hex(std::span(id).first(8)));
        return 0;
    }
    if (cmd == "compact" && args.size() == 2) {
        if (auto r = compact(file); !r) return fail(r.error());
        std::println("{}: compacted to {} bytes", file.string(), std::filesystem::file_size(file));
        return 0;
    }
    if (cmd == "pack" && args.size() >= 3) {
        bool drop = false;
        std::vector<std::string_view> rest;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--drop-orphans") drop = true;
            else rest.push_back(args[i]);
        }
        const auto o = parse_options(rest);
        if (!o) return usage();
        std::optional<SigningKey> key;
        if (o->key_file) {
            key = load_key(*o->key_file, o->signer);
            if (!key) return fail("not a key file: " + *o->key_file);
        }
        WriteOptions wo;
        if (o->fast) wo.level = 3;
        auto c = pack(file, std::filesystem::path(args[2]), wo, {.drop_orphans = drop}, std::move(key));
        if (!c) return fail(c.error());
        std::println("{}: packed into {}, {} entries{}", file.string(), args[2], c->live_count, c->signatures_length ? ", signed" : "");
        return 0;
    }
    const bool four = cmd == "put" || cmd == "mv" || cmd == "truncate";
    const std::size_t positional = four ? 4 : 3;
    if ((cmd != "new" && cmd != "put" && cmd != "rm" && !four) || args.size() < positional) return usage();
    const auto o = parse_options(std::span(args).subspan(positional));
    if (!o) return usage();
    WriteOptions wo;
    if (o->fast) wo.level = 3;
    auto w = cmd == "new" ? Writer::create(file, wo) : Writer::open(file, wo);
    if (!w) return fail(w.error());
    if (o->key_file) {
        auto key = load_key(*o->key_file, o->signer);
        if (!key) return fail("not a key file: " + *o->key_file);
        w->sign_with(std::move(*key));
    }
    if (cmd == "new") {
        auto manifest = slurp(args[2]);
        if (!manifest) return fail("cannot read " + std::string(args[2]));
        if (auto r = w->put("SQ-INF/manifest.json", *manifest, {.media_type = "application/json"}); !r) return fail(r.error());
    } else if (cmd == "put" && args[3] != "-") {
        // From a file: read at commit, 1 MiB at a time past 64 MiB.
        if (auto r = w->put_file(std::string(args[2]), std::filesystem::path(args[3]), o->put); !r) return fail(r.error());
    } else if (cmd == "put") {
        auto content = slurp(args[3]);
        if (!content) return fail("cannot read " + std::string(args[3]));
        if (auto r = w->put(std::string(args[2]), *content, o->put); !r) return fail(r.error());
    } else if (cmd == "mv") {
        if (auto r = w->rename(args[2], std::string(args[3])); !r) return fail(r.error());
    } else if (cmd == "truncate") {
        std::uint64_t size = 0;
        const auto t = args[3];
        if (std::from_chars(t.data(), t.data() + t.size(), size).ec != std::errc{}) return usage();
        if (auto r = w->resize(args[2], size); !r) return fail(r.error());
    } else {
        if (auto r = w->remove(args[2]); !r) return fail(r.error());
    }
    auto c = w->commit();
    if (!c) return fail(c.error());
    std::println("{}: commit {} ({}), {} live entries{}", file.string(), c->sequence, c->index_kind == IndexKind::full ? "full" : "delta",
                 c->live_count, c->signatures_length ? ", signed" : "");
    return 0;
}

#endif

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() < 2) return usage();
    const std::string_view cmd = args[0];
    if (cmd == "check" || cmd == "ls" || cmd == "cat" || cmd == "info" || cmd == "unpack") return read_commands(cmd, args);
#ifndef SQCART3_READ_ONLY
    return write_commands(cmd, args);
#else
    return usage();
#endif
}
