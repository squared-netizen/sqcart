// SPDX-License-Identifier: MIT
//
// sq.cart.testing — what the tests share: a minimal writer of 3.0
// cartridges (stored, chunked, precompressed zstd runs, deltas, tombstones,
// signatures) with hooks to break what it writes, real zstd frames
// (zstd_fixtures.inc), and a check() that counts failures.
//
// Not the 3.0 writer (phase 2): it writes what a test tells it, including
// what a writer must never write.

export module sq.cart.testing;

import std;
import sq.cart;

export namespace sq::cart::testing {

#include "zstd_fixtures.inc"

inline int failures = 0;

inline void check(bool ok, std::string_view what, std::source_location at = std::source_location::current()) {
    if (!ok) {
        ++failures;
        std::println(std::cerr, "FAIL {}:{}  {}", at.file_name(), at.line(), what);
    }
}

inline int report(std::string_view suite) {
    std::println(std::cerr, "{} {}", failures == 0 ? "ok  " : "FAIL", suite);
    return failures == 0 ? 0 : 1;
}

inline std::vector<std::byte> bytes(std::string_view text) {
    const auto b = std::as_bytes(std::span(text));
    return {b.begin(), b.end()};
}

template <std::size_t N>
std::vector<std::byte> bytes(const std::array<std::uint8_t, N>& a) {
    std::vector<std::byte> out;
    for (const std::uint8_t c : a) out.push_back(static_cast<std::byte>(c));
    return out;
}

inline std::string text(const std::vector<std::byte>& b) { return {reinterpret_cast<const char*>(b.data()), b.size()}; }

/// One entry a commit writes (or deletes, as a tombstone).
struct Put {
    std::string path;
    std::vector<std::byte> content;
    std::uint16_t flags = flag::r;
    Codec codec = Codec::stored;
    std::vector<std::byte> frame;  ///< the run, for codecs 1 and 2
    std::uint32_t dictionary = 0;
    std::optional<std::string> title, author, media_type, attributes;
    std::int64_t created = 0, updated = 0;
    bool tombstone = false;
    std::optional<std::uint64_t> reuse_run;  ///< name this run, written earlier, instead of writing one
};

struct CommitOptions {
    bool full = true;
    std::optional<std::uint64_t> features;     ///< else what the state uses
    std::vector<std::array<std::byte, 32>> signing_seeds;
    std::string signer;
    std::optional<std::uint64_t> live_count;   ///< else the true count
    std::optional<Digest> content_digest;      ///< else the true digest
    /// Change the record's bytes (offsets 0..191) before it is sealed.
    std::function<void(std::span<std::byte>)> edit_record;
    /// Change an entry record's bytes before the index is written.
    std::function<void(const std::string& path, std::span<std::byte, 128>)> edit_entry;
};

inline std::vector<std::byte> manifest(std::string_view id = "data.test-world", std::string_view version = "1.0.0") {
    return bytes(std::format(R"({{"format":"squared-cartridge","format_version":3,"kind":"data","id":"{}","version":"{}","tree":"."}})", id, version));
}

class Builder {
public:
    Builder() : file_(8192) {
        const std::uint8_t m[] = {0x89, 'S', 'Q', 'C', '\r', '\n', 0x1a, '\n'};
        for (std::size_t i = 0; i < 8; ++i) file_[i] = static_cast<std::byte>(m[i]);
        put16(8, 3);
        put16(10, 0);
        put32(12, 8192);
    }

    std::vector<std::byte>& file() noexcept { return file_; }
    [[nodiscard]] std::uint64_t record() const noexcept { return record_; }
    /// Where each path's run (or chunk table) went in the last commit.
    std::map<std::string, std::uint64_t> runs;
    std::map<std::string, std::vector<std::uint64_t>> chunk_runs;
    std::string id = "data.test-world", version = "1.0.0";

    /// Append one commit and point the spare slot at it.
    std::uint64_t commit(std::vector<Put> puts, CommitOptions o = {}) {
        runs.clear();
        chunk_runs.clear();
        std::ranges::sort(puts, {}, &Put::path);
        // 1. Data.
        align(64);
        const std::uint64_t data_offset = file_.size();
        std::vector<Entry> entries;
        std::vector<std::vector<std::byte>> chunk_tables;  // per entry, empty unless chunked
        for (const Put& p : puts) {
            Entry e;
            e.path = p.path;
            e.title = p.title, e.author = p.author, e.media_type = p.media_type, e.attributes = p.attributes;
            e.created = p.created, e.updated = p.updated;
            e.flags = p.tombstone ? flag::deleted : p.flags;
            std::vector<std::byte> table;
            if (!p.tombstone) {
                e.codec = p.codec;
                e.size = p.content.size();
                e.dictionary = p.dictionary;
                e.digest = tree_hash(p.content);
                if (p.codec == Codec::chunked) {
                    for (std::size_t at = 0; at < p.content.size(); at += chunk_size) {
                        const auto piece = std::span(p.content).subspan(at, std::min<std::size_t>(chunk_size, p.content.size() - at));
                        align(64);
                        const std::uint64_t off = file_.size();
                        file_.insert(file_.end(), piece.begin(), piece.end());
                        chunk_runs[p.path].push_back(off);
                        std::array<std::byte, 64> rec{};
                        le(rec, 0, off, 8);
                        le(rec, 8, piece.size(), 4);
                        le(rec, 12, piece.size(), 4);
                        const Digest h = sha256(piece);
                        std::ranges::copy(h, rec.begin() + 32);
                        table.insert(table.end(), rec.begin(), rec.end());
                    }
                    e.stored_size = table.size();
                } else if (p.reuse_run) {
                    e.offset = *p.reuse_run;
                    e.stored_size = p.codec == Codec::stored ? p.content.size() : p.frame.size();
                } else if (!p.content.empty()) {
                    align((p.flags & flag::mappable) ? 16384 : 64);
                    e.offset = file_.size();
                    const auto& run = p.codec == Codec::stored ? p.content : p.frame;
                    file_.insert(file_.end(), run.begin(), run.end());
                    e.stored_size = run.size();
                }
                runs[p.path] = e.offset;
            }
            entries.push_back(std::move(e));
            chunk_tables.push_back(std::move(table));
        }
        const std::uint64_t data_length = file_.size() - data_offset;
        // 2. Strings, in order of first use.
        std::vector<std::byte> strings;
        std::map<std::string, std::uint32_t> at;
        const auto str = [&](const std::optional<std::string>& s) -> std::uint32_t {
            if (!s) return 0xffffffffu;
            if (auto it = at.find(*s); it != at.end()) return it->second;
            const auto off = static_cast<std::uint32_t>(strings.size());
            for (int i = 0; i < 4; ++i) strings.push_back(static_cast<std::byte>(s->size() >> (8 * i)));
            const auto b = std::as_bytes(std::span(*s));
            strings.insert(strings.end(), b.begin(), b.end());
            at.emplace(*s, off);
            return off;
        };
        std::vector<std::array<std::uint32_t, 5>> refs;
        for (const Entry& e : entries) refs.push_back({str(e.path), str(e.title), str(e.author), str(e.media_type), str(e.attributes)});
        // 3. Where the index, strings and chunk tables go.
        align(64);
        const std::uint64_t index_offset = file_.size();
        const std::uint64_t strings_offset = index_offset + entries.size() * 128;
        const std::uint64_t chunks_offset = strings_offset + strings.size();
        std::uint64_t chunk_at = chunks_offset;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].codec == Codec::chunked && !entries[i].deleted()) {
                entries[i].offset = chunk_at;
                runs[entries[i].path] = chunk_at;
                chunk_at += chunk_tables[i].size();
            }
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const Entry& e = entries[i];
            std::array<std::byte, 128> r{};
            for (std::size_t k = 0; k < 5; ++k) le(r, 4 * k, refs[i][k], 4);
            le(r, 20, e.flags, 2);
            le(r, 22, static_cast<std::uint16_t>(e.codec), 2);
            le(r, 24, e.dictionary, 4);
            le(r, 32, e.offset, 8);
            le(r, 40, e.stored_size, 8);
            le(r, 48, e.size, 8);
            le(r, 56, static_cast<std::uint64_t>(e.created), 8);
            le(r, 64, static_cast<std::uint64_t>(e.updated), 8);
            std::ranges::copy(e.digest, r.begin() + 72);
            if (o.edit_entry) o.edit_entry(e.path, r);
            file_.insert(file_.end(), r.begin(), r.end());
        }
        file_.insert(file_.end(), strings.begin(), strings.end());
        for (const auto& t : chunk_tables) file_.insert(file_.end(), t.begin(), t.end());
        const std::uint64_t chunks_length = file_.size() - chunks_offset;
        // 4. The state, its digest and features.
        for (const Entry& e : entries) {
            if (e.deleted()) {
                live_.erase(e.path);
            } else {
                live_.insert_or_assign(e.path, e);
            }
        }
        const Digest digest = o.content_digest.value_or(state_digest());
        std::uint64_t features = 0;
        for (const auto& [path, e] : live_) {
            if (e.codec == Codec::zstd_dict) features |= feature::zstd_dictionaries;
            if (e.codec == Codec::chunked) features |= feature::chunked;
        }
        // 5. Signatures.
        const std::uint64_t signatures_offset = o.signing_seeds.empty() ? 0 : file_.size();
        if (!o.signing_seeds.empty()) {
            std::vector<std::byte> block;
            const auto put = [&](std::uint64_t v, int n) {
                for (int i = 0; i < n; ++i) block.push_back(static_cast<std::byte>(v >> (8 * i)));
            };
            put(o.signing_seeds.size(), 4);
            for (auto seed : o.signing_seeds) {
                std::array<std::byte, 64> secret{};
                std::array<std::byte, 32> pub{};
                ed25519_key_pair(seed, secret, pub);
                const auto sig = ed25519_sign(secret, signed_message(1, id, version, o.signer, digest));
                put(1, 2);
                put(32, 2);
                block.insert(block.end(), pub.begin(), pub.end());
                const Digest kh = sha256(pub);
                block.insert(block.end(), kh.begin(), kh.begin() + 8);
                put(o.signer.size(), 4);
                const auto s = std::as_bytes(std::span(o.signer));
                block.insert(block.end(), s.begin(), s.end());
                put(64, 2);
                block.insert(block.end(), sig.begin(), sig.end());
            }
            file_.insert(file_.end(), block.begin(), block.end());
        }
        const std::uint64_t signatures_length = o.signing_seeds.empty() ? 0 : file_.size() - signatures_offset;
        // 6. The record.
        const std::uint64_t record = file_.size();
        const std::uint32_t chain = o.full ? 0 : chain_ + 1;
        std::array<std::byte, 224> c{};
        const char m[] = "SQCOMMIT";
        for (std::size_t i = 0; i < 8; ++i) c[i] = static_cast<std::byte>(m[i]);
        le(c, 8, ++sequence_, 8);
        le(c, 16, record_, 8);
        le(c, 24, data_offset, 8);
        le(c, 32, data_length, 8);
        le(c, 40, index_offset, 8);
        le(c, 48, entries.size(), 8);
        le(c, 56, strings_offset, 8);
        le(c, 64, strings.size(), 8);
        le(c, 72, chunks_length == 0 ? 0 : chunks_offset, 8);
        le(c, 80, chunks_length, 8);
        le(c, 88, o.full ? 0 : 1, 4);
        le(c, 92, chain, 4);
        le(c, 96, o.live_count.value_or(live_.size()), 8);
        le(c, 104, 0, 8);
        le(c, 112, o.features.value_or(features), 8);
        std::ranges::copy(digest, c.begin() + 120);
        le(c, 152, signatures_offset, 8);
        le(c, 160, signatures_length, 8);
        if (o.edit_record) o.edit_record(std::span(c).first(192));
        const Digest seal = sha256(std::span(c).first(192));
        std::ranges::copy(seal, c.begin() + 192);
        file_.insert(file_.end(), c.begin(), c.end());
        // 7. The spare slot.
        write_slot(next_slot_, sequence_, record);
        next_slot_ = next_slot_ == 64 ? 4096 : 64;
        record_ = record;
        chain_ = chain;
        return record;
    }

    /// Point a slot at a record (or at anything, for tests that break slots).
    void write_slot(std::uint64_t at, std::uint64_t sequence, std::uint64_t record) {
        std::array<std::byte, 64> s{};
        le(s, 0, sequence, 8);
        le(s, 8, record, 8);
        le(s, 16, 224, 8);
        le(s, 24, record + 224, 8);
        const Digest h = sha256(std::span(s).first(56));
        std::copy(h.begin(), h.begin() + 8, s.begin() + 56);
        std::ranges::copy(s, file_.begin() + static_cast<std::ptrdiff_t>(at));
    }

    [[nodiscard]] Digest state_digest() const {
        Sha256 h;
        h.update(std::string_view("squared-cartridge/3 state\0", 26));
        std::array<std::byte, 8> n{};
        le(n, 0, live_.size(), 8);
        h.update(n);
        const auto field = [&](std::span<const std::byte> b) {
            std::array<std::byte, 4> len{};
            le(len, 0, b.size(), 4);
            h.update(len);
            h.update(b);
        };
        for (const auto& [path, e] : live_) {
            const auto meta = metadata_json(e);
            field(std::as_bytes(std::span(path)));
            field(e.digest);
            field(std::as_bytes(std::span(meta ? *meta : std::string("?"))));
        }
        return h.finish();
    }

private:
    template <std::size_t N>
    static void le(std::array<std::byte, N>& a, std::size_t at, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) a[at + static_cast<std::size_t>(i)] = static_cast<std::byte>(v >> (8 * i));
    }
    static void le(std::span<std::byte, 128> a, std::size_t at, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) a[at + static_cast<std::size_t>(i)] = static_cast<std::byte>(v >> (8 * i));
    }
    void put16(std::size_t at, std::uint16_t v) {
        file_[at] = static_cast<std::byte>(v & 0xff);
        file_[at + 1] = static_cast<std::byte>(v >> 8);
    }
    void put32(std::size_t at, std::uint32_t v) {
        for (std::size_t i = 0; i < 4; ++i) file_[at + i] = static_cast<std::byte>(v >> (8 * i));
    }
    void align(std::uint64_t to) { file_.resize((file_.size() + to - 1) / to * to); }

    std::vector<std::byte> file_;
    std::map<std::string, Entry> live_;
    std::uint64_t sequence_ = 0;
    std::uint64_t record_ = 0;
    std::uint32_t chain_ = 0;
    std::uint64_t next_slot_ = 64;
};

/// A cartridge with a manifest and `puts`, in one full commit.
inline Builder simple(std::vector<Put> puts, CommitOptions o = {}) {
    Builder b;
    puts.push_back({.path = "SQ-INF/manifest.json", .content = manifest(), .media_type = "application/json"});
    b.commit(std::move(puts), std::move(o));
    return b;
}

}  // namespace sq::cart::testing
