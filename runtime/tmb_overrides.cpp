// Game-specific runtime setup for Twisted Metal: Black (PAL, SCES-50360).

#include "game_overrides.h"
#include "ps2_runtime.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace
{
    constexpr uint32_t kTmbPalEntry = 0x00100008u;

    // The game reads raw sectors with sceCdRead (LBN 0x10, the primary volume descriptor,
    // right after boot), which the file-backed cdrom0: cannot serve. Give the CD device the
    // disc image itself: TMB_ISO, else the first .iso found walking up from the ELF folder
    // (game/ -> repository root).
    std::filesystem::path findDiscImage(const std::filesystem::path &elfDirectory)
    {
        if (const char *configured = std::getenv("TMB_ISO"); configured && configured[0] != '\0')
        {
            return configured;
        }
        std::filesystem::path dir = elfDirectory;
        for (int depth = 0; depth < 4 && !dir.empty(); ++depth, dir = dir.parent_path())
        {
            std::error_code ec;
            for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
            {
                if (!entry.is_regular_file(ec))
                {
                    continue;
                }
                std::string ext = entry.path().extension().string();
                for (char &c : ext)
                {
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (ext == ".iso")
                {
                    return entry.path();
                }
            }
            if (dir == dir.root_path())
            {
                break;
            }
        }
        return {};
    }

    void applyTmb(PS2Runtime &runtime)
    {
        (void)runtime;

        PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
        paths.cdImage = findDiscImage(paths.elfDirectory);
        if (paths.cdImage.empty())
        {
            std::cerr << "[tmb] no disc image found: raw sceCdRead calls will fail (set TMB_ISO)" << std::endl;
            return;
        }
        std::cout << "[tmb] disc image: " << paths.cdImage.string() << std::endl;
        PS2Runtime::setIoPaths(paths);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Twisted Metal - Black (Europe)", "SCES_503.60", kTmbPalEntry, 0u, &applyTmb)
