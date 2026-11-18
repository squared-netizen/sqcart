// SPDX-License-Identifier: MIT
//
// The building blocks, against values computed elsewhere: SHA-256 (FIPS
// 180-4's vectors), the tree hash of §9.1 (Python's hashlib), RFC 8785's
// canonical JSON, I-JSON's refusals, case folding (Python's str.casefold),
// and Ed25519 (RFC 8032 test 1, and the encodings §9.3 refuses).

import std;
import sq.cart;
import sq.cart.testing;

using namespace sq::cart;
using namespace sq::cart::testing;

namespace {

std::vector<std::byte> from_hex(std::string_view h) {
    std::vector<std::byte> out;
    for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
        unsigned v = 0;
        std::from_chars(h.data() + i, h.data() + i + 2, v, 16);
        out.push_back(static_cast<std::byte>(v));
    }
    return out;
}

}  // namespace

int main() {
    check(to_hex(sha256(bytes(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 of nothing");
    check(to_hex(sha256(bytes("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 of abc");
    check(to_hex(sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
          "sha256 of 448 bits");

    // §9.1: up to 1 MiB the digest is plain SHA-256; past it, the tree.
    std::vector<std::byte> mib(chunk_size, std::byte{'a'});
    check(to_hex(tree_hash(mib)) == "9bc1b2a288b26af7257a36277ae3816a7d4f16e89c1e7e77d0a5c48bad62b360", "1 MiB: plain SHA-256");
    mib.push_back(std::byte{'a'});
    check(to_hex(tree_hash(mib)) == "28b0aa9cd21d048fb70521158ee79ab59486b114db11d063427999d84a43fabd", "1 MiB + 1: the tree");

    // RFC 8785.
    check(canonical_json(R"({ "b": [1, 2.50, 1e21, 1E-7], "a": {"z": 1, "y": "é"} })").value_or("?") ==
              "{\"a\":{\"y\":\"é\",\"z\":1},\"b\":[1,2.5,1e+21,1e-7]}",
          "canonical JSON");
    check(!canonical_json(R"({"a":1,"a":2})"), "I-JSON: no member twice");
    check(!canonical_json("9007199254740992"), "I-JSON: no integer past 2^53 - 1");
    check(canonical_json("9007199254740991").value_or("?") == "9007199254740991", "I-JSON: 2^53 - 1 is fine");
    check(!canonical_json("\xef\xbb\xbf{}"), "I-JSON: no byte order mark");
    check(!canonical_json(R"("\ud800")"), "I-JSON: no lone surrogate");
    check(!canonical_json("[[[[1]]]]", 3), "I-JSON: nesting limit");

    // §4.1: Unicode full case folding.
    check(paths_collide("README.md", "readme.MD"), "ASCII case collides");
    check(paths_collide("Straße/x", "STRASSE/X"), "ß folds to ss");
    check(paths_collide("Été", "éTÉ"), "accented letters fold");
    check(paths_collide("K", "k"), "the Kelvin sign folds to k");
    check(!paths_collide("a/b", "a/c"), "different paths do not");

    // §4.1: Normalization Form C (checked here against Python's unicodedata for
    // every code point and 20,000 mixed strings, when it was written).
    check(nfc("e\u0301") == "\u00e9", "e + acute composes");
    check(nfc("\u212b") == "\u00c5", "the Angstrom sign becomes \u00c5");
    check(nfc("\u1100\u1161\u11a8") == "\uac01", "Hangul jamo compose");
    check(nfc("a\u0323\u0302") == nfc("a\u0302\u0323") && nfc("a\u0302\u0323") == "\u1ead", "marks in canonical order, then composed");
    check(nfc("\u0958") == "\u0915\u093c", "a composition exclusion stays decomposed");
    check(is_nfc("plain/ascii.txt") && is_nfc("caf\u00e9") && !is_nfc("cafe\u0301"), "is_nfc");

    // RFC 8032 test 1: the empty message.
    const auto pk = from_hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
    const auto sig = from_hex(
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
    const auto key = std::span<const std::byte, 32>(pk.data(), 32);
    check(ed25519_verify(std::span<const std::byte, 64>(sig.data(), 64), key, {}), "RFC 8032 test 1 verifies");
    auto tampered = sig;
    tampered[10] ^= std::byte{1};
    check(!ed25519_verify(std::span<const std::byte, 64>(tampered.data(), 64), key, {}), "a changed signature does not");
    // S + L: the same signature with a non-canonical S (Monocypher refuses).
    auto s_plus_l = sig;
    const auto l = from_hex("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010");
    unsigned carry = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        const unsigned v = std::to_integer<unsigned>(s_plus_l[32 + i]) + std::to_integer<unsigned>(l[i]) + carry;
        s_plus_l[32 + i] = static_cast<std::byte>(v & 0xff);
        carry = v >> 8;
    }
    check(!ed25519_verify(std::span<const std::byte, 64>(s_plus_l.data(), 64), key, {}), "a non-canonical S is refused");
    // A public key whose y is not reduced (y = p): refused before Monocypher.
    auto bad_key = from_hex("edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f");
    check(!ed25519_verify(std::span<const std::byte, 64>(sig.data(), 64), std::span<const std::byte, 32>(bad_key.data(), 32), {}),
          "a non-canonical key is refused");

    // Sign and verify with a seed.
    std::array<std::byte, 32> seed{};
    seed[0] = std::byte{7};
    std::array<std::byte, 64> secret{};
    std::array<std::byte, 32> pub{};
    ed25519_key_pair(seed, secret, pub);
    const auto message = bytes("squared");
    const auto s = ed25519_sign(secret, message);
    check(ed25519_verify(s, pub, message), "sign, then verify");
    return report("hash");
}
