// Game-specific runtime setup for Twisted Metal: Black (PAL, SCES-50360).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "Stubs/MPEG.h"
#include "runtime/ee_scheduler.h"
#include <string>

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace ps2_stubs
{
    std::string mpegDebugSummary(); // Stubs/MPEG.cpp
}

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

    // Movie skip. The player (Sony's ezmpeg sample) aborts a movie with videoDecAbort, then
    // videoDecFlush appends a sequence_end_code to the IPU ring and waits until the decode
    // thread reaches it. With libmpeg HLE'd that code never reaches the host decoder, so
    // sceMpegIsEnd stays false and updateMpegMovieDecoding waits forever. Tell the HLE the
    // playback is over at the abort; the VideoDec starts with its sceMpeg, so a0 is both.
    constexpr uint32_t kVideoDecAbort = 0x001FF920u;

    void videoDecAbortOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t videoDec = getRegU32(ctx, 4);
        videoDecAbort__FP8VideoDec_0x1ff920(rdram, ctx, runtime);
        ps2_stubs::abortMpegPlayback(runtime, videoDec);
    }

    // TMB_TRACE_MOVIE=1: log the movie player's thread teardown (temporary bring-up aid).
    constexpr uint32_t kTermAll = 0x001E0948u;

    void termAllTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const auto readGuest = [&](uint32_t addr) { return *reinterpret_cast<const int32_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const int ids[2] = {readGuest(gp - 0x321Cu), readGuest(gp - 0x3218u)};
        termAll__Fv_0x1e0948(rdram, ctx, runtime);
        for (int id : ids)
        {
            const GuestThread *t = runtime->eeScheduler().thread(id);
            std::cerr << "[tmb:termAll] thread " << id << " after terminate: "
                      << (t ? std::to_string(static_cast<int>(t->status)) : std::string("gone"))
                      << (t ? " entry=0x" : "") << std::hex << (t ? t->entry : 0u) << std::dec << std::endl;
        }
    }

    constexpr uint32_t kPcmCallback = 0x001ED5F8u;
    constexpr uint32_t kAudioDecSendToIop = 0x00202090u;
    uint32_t g_pcmCalls = 0;
    uint32_t g_sendCalls = 0;

    void pcmCallbackTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cbData = getRegU32(ctx, 5);
        pcmCallback__FP7sceMpegP16sceMpegCbDataStrPv_0x1ed5f8(rdram, ctx, runtime);
        if (g_pcmCalls++ < 8u)
        {
            std::cerr << "[tmb:pcm] call " << g_pcmCalls << " cbData=0x" << std::hex << cbData
                      << " ret=" << std::dec << static_cast<int32_t>(getRegU32(ctx, 2)) << std::endl;
        }
    }

    void audioDecSendToIopTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ad = getRegU32(ctx, 4);
        const auto rd = [&](uint32_t off) { return *reinterpret_cast<const int32_t *>(rdram + ((ad + off) & 0x1FFFFFFu)); };
        const int32_t stateBefore = rd(0x0);
        audioDecSendToIOP__FP8AudioDec_0x202090(rdram, ctx, runtime);
        if ((g_sendCalls++ % 50u) == 0u)
        {
            std::cerr << "[tmb:audio] send#" << g_sendCalls << " state " << stateBefore << "->" << rd(0x0)
                      << " +0x30=" << rd(0x30) << " +0x34=" << rd(0x34) << " +0x38=" << rd(0x38)
                      << " +0x3c=" << rd(0x3c) << " +0x44=0x" << std::hex << rd(0x44) << std::dec
                      << " +0x48=" << rd(0x48) << " +0x54=" << rd(0x54) << " pcmCalls=" << g_pcmCalls << std::endl;
        }
    }

    constexpr uint32_t kVblankHandler = 0x001C2898u;
    uint32_t g_vblankCalls = 0;

    void vblankHandlerTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const auto rd = [&](uint32_t addr) { return *reinterpret_cast<const int32_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const int32_t displaying = rd(gp - 0x340Cu);
        vblankHandler__Fi_0x1c2898(rdram, ctx, runtime);
        if (displaying && (g_vblankCalls++ % 100u) == 0u)
        {
            std::cerr << "[tmb:vblank] call#" << g_vblankCalls << " displaying=" << displaying
                      << " pathBusy=" << rd(gp - 0x3400u) << " ret=" << static_cast<int32_t>(getRegU32(ctx, 2))
                      << " voBuf(+8,+0x10,+0xc)=" << rd(0x4C7A18u + 8u) << "," << rd(0x4C7A18u + 0x10u) << ","
                      << rd(0x4C7A18u + 0xCu) << " | " << ps2_stubs::mpegDebugSummary() << std::endl;
        }
    }

    void applyTmb(PS2Runtime &runtime)
    {
        runtime.registerFunction(kVideoDecAbort, &videoDecAbortOverride);
        if (const char *trace = std::getenv("TMB_TRACE_MOVIE"); trace && *trace == '1')
        {
            runtime.registerFunction(kTermAll, &termAllTrace);
            runtime.registerFunction(kPcmCallback, &pcmCallbackTrace);
            runtime.registerFunction(kAudioDecSendToIop, &audioDecSendToIopTrace);
            runtime.registerFunction(kVblankHandler, &vblankHandlerTrace);
        }

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
