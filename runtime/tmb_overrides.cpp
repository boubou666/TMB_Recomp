// Game-specific runtime overrides for Twisted Metal: Black (PAL, SCES-50360).
//
// The descriptor below matches on the ELF file name and entry point, so it only
// fires when the runtime boots this exact executable. Use applyTmbOverrides to
// bind stripped/unsupported functions to runtime handlers, patch behaviour, or
// register replacement functions as bring-up progresses.

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_log.h"

namespace
{
    constexpr uint32_t kTmbPalEntry = 0x00100008u;

    void applyTmbOverrides(PS2Runtime &runtime)
    {
        (void)runtime;
        // Example: ps2_game_overrides::bindAddressHandler(runtime, 0x00123456u, "ret0");
    }
}

PS2_REGISTER_GAME_OVERRIDE("Twisted Metal - Black (Europe)", "SCES_503.60", kTmbPalEntry, 0u, &applyTmbOverrides)
