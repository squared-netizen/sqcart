// SPDX-License-Identifier: MIT
//
// sq.cart:fold — when two paths name one file on a case-insensitive file
// system (§4.1): equal after Unicode full case folding. Android's shared
// storage, exFAT, FAT, macOS and Windows all fold case, and fold more than
// ASCII.

export module sq.cart:fold;

import std;
import :json;

namespace sq::cart {

struct Fold {
    char32_t from;
    std::array<char32_t, 3> to;
};

constexpr Fold folds[] =
#include "casefold.inc"
    ;

[[nodiscard]] std::u32string folded(std::string_view path) {
    std::u32string cps, out;
    static_cast<void>(decode_utf8(path, &cps));
    for (const char32_t cp : cps) {
        const auto* it = std::ranges::lower_bound(folds, cp, {}, &Fold::from);
        if (it != std::ranges::end(folds) && it->from == cp) {
            for (const char32_t t : it->to) {
                if (t != 0) out.push_back(t);
            }
        } else {
            out.push_back(cp);
        }
    }
    return out;
}

}  // namespace sq::cart

export namespace sq::cart {

/// Whether two paths collide on a case-insensitive file system: equal after
/// Unicode full case folding (CaseFolding.txt, statuses C and F). Paths are
/// NFC already (§4.1).
[[nodiscard]] bool paths_collide(std::string_view a, std::string_view b) { return folded(a) == folded(b); }

/// The key two colliding paths share, for finding collisions in a set.
[[nodiscard]] std::u32string collision_key(std::string_view path) { return folded(path); }

}  // namespace sq::cart
