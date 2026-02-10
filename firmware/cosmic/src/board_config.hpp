#pragma once

#include <cstdint>

struct BoardInfo {
    uint16_t width;
    uint16_t height;
    const char* name;
};

inline constexpr BoardInfo kCosmicUnicorn{32, 32, "Cosmic Unicorn"};
inline constexpr BoardInfo kGalacticUnicorn{53, 11, "Galactic Unicorn"};
inline constexpr BoardInfo kUnicornHD{16, 16, "Unicorn HD"};
inline constexpr BoardInfo kUnicornPack{8, 4, "Unicorn Pack"};

const BoardInfo& detect_board();

