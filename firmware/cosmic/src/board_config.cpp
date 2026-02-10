#include "board_config.hpp"

// Detect the connected board at runtime or via compile-time flags.
// BOARD_COSMIC_UNICORN, BOARD_GALACTIC_UNICORN, BOARD_UNICORN_HD or BOARD_UNICORN_PACK
// may be defined at build time.
const BoardInfo& detect_board() {
#ifdef BOARD_COSMIC_UNICORN
    return kCosmicUnicorn;
#elif defined(BOARD_GALACTIC_UNICORN)
    return kGalacticUnicorn;
#elif defined(BOARD_UNICORN_HD)
    return kUnicornHD;
#elif defined(BOARD_UNICORN_PACK)
    return kUnicornPack;
#else
    // Default to Cosmic Unicorn for Pico W builds
    return kCosmicUnicorn;
#endif
}

