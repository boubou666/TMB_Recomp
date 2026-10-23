// Game-specific runtime setup for Twisted Metal: Black (PAL, SCES-50360).

#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_recompiled_functions.h"
#include "Stubs/MPEG.h"
#include "runtime/ee_scheduler.h"
#include "runtime/gs/gs_cpu_backend.h"
#include "runtime/ps2_memory.h"
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>

#include <cmath>
#include <cstdio>
#include <vector>
#include <cstring>
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
        static uint32_t s_dispCalls = 0;
        if (std::getenv("TMB_TRACE_DISPLAY") && (s_dispCalls++ % 150u) == 0u)
        {
            PS2Memory &m = runtime->memory();
            std::cerr << std::hex << "[tmb:disp] pmode=0x" << m.read64(0x12000000u) << " smode2=0x" << m.read64(0x12000020u)
                      << " dispfb1=0x" << m.read64(0x12000070u) << " display1=0x" << m.read64(0x12000080u)
                      << " dispfb2=0x" << m.read64(0x12000090u) << " display2=0x" << m.read64(0x120000A0u)
                      << " bg=0x" << m.read64(0x120000E0u) << std::dec << std::endl;
        }
        if (displaying && (g_vblankCalls++ % 100u) == 0u)
        {
            std::cerr << "[tmb:vblank] call#" << g_vblankCalls << " displaying=" << displaying
                      << " pathBusy=" << rd(gp - 0x3400u) << " ret=" << static_cast<int32_t>(getRegU32(ctx, 2))
                      << " voBuf(+8,+0x10,+0xc)=" << rd(0x4C7A18u + 8u) << "," << rd(0x4C7A18u + 0x10u) << ","
                      << rd(0x4C7A18u + 0xCu) << " | " << ps2_stubs::mpegDebugSummary() << std::endl;
        }
    }

    // TMB_TRACE_FONT=1: dump the font packet state on each fontDmaFontData (bring-up aid).
    constexpr uint32_t kFontDmaFontData = 0x001C4FE0u;
    uint32_t g_fontCalls = 0;

    void fontDmaFontDataTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        const auto rd32 = [&](uint32_t addr) { return *reinterpret_cast<const int32_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const auto rd64 = [&](uint32_t addr) { return *reinterpret_cast<const uint64_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const int32_t state = rd32(gp - 0x2D28u), count = rd32(gp - 0x2D2Cu), base = rd32(gp - 0x2D20u);
        const int32_t tagIdx = rd32(gp - 0x2D24u), prims = static_cast<int32_t>(rd32(gp - 0x2D30u) & 0xFFFF);
        fontDmaFontData__Fv_0x1c4fe0(rdram, ctx, runtime);
        const uint32_t n = g_fontCalls++;
        if (n < 6u || (n % 200u) == 0u || (state == 2 && n < 400u && (n % 20u) == 0u))
        {
            constexpr uint32_t kBuf = 0x0050CB60u;
            std::cerr << std::hex << "[tmb:font] call#" << std::dec << n << " state=" << state << " count=" << count
                      << " base=" << base << " tagIdx=" << tagIdx << " prims=" << prims << std::hex
                      << " | tag@idx=0x" << rd64(kBuf + 16u * static_cast<uint32_t>(tagIdx)) << "_" << rd64(kBuf + 16u * static_cast<uint32_t>(tagIdx) + 8u)
                      << " q0=0x" << rd64(kBuf) << "_" << rd64(kBuf + 8u) << " after: count=" << std::dec << rd32(gp - 0x2D2Cu)
                      << " tagIdx=" << rd32(gp - 0x2D24u) << " state=" << rd32(gp - 0x2D28u) << std::endl;
        }
    }

    // TMB_TRACE_THREADS=<seconds>: print every guest thread and semaphore that often (hang hunting).
    void startThreadTrace(PS2Runtime &runtime, double periodSeconds)
    {
        std::thread([&runtime, periodSeconds]()
                    {
                        static const char *const kStatus[] = {"run", "ready", "wait", "wait+susp", "susp", "dormant"};
                        static const char *const kWait[] = {"-", "sleep", "sema", "evflag", "vsync", "external", "mpeg"};
                        for (uint32_t pass = 0;; ++pass)
                        {
                            std::this_thread::sleep_for(std::chrono::duration<double>(periodSeconds));
                            const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
                            std::cerr << "[tmb:threads] pass " << pass << " running=" << snap.runningThreadId
                                      << " | mpeg: " << ps2_stubs::mpegDebugSummary() << std::endl;
                            for (const EeThreadSnapshot &t : snap.threads)
                            {
                                const auto st = static_cast<size_t>(t.status);
                                const auto wr = static_cast<size_t>(t.waitReason);
                                std::cerr << std::hex << "  th" << std::dec << t.id << " " << (st < 6 ? kStatus[st] : "?")
                                          << " wait=" << (wr < 7 ? kWait[wr] : "?") << ":" << t.waitId
                                          << " prio=" << t.currentPriority << std::hex << " pc=0x" << t.pc
                                          << " ra=0x" << t.ra << " sp=0x" << t.sp << " entry=0x" << t.entry << std::dec
                                          << " wakeups=" << t.wakeupCount << " depth=" << t.invocationDepth << std::endl;
                            }
                            {
                                const auto &g = runtime.memory().gs();
                                std::cerr << std::hex << "  [gs] pmode=0x" << g.pmode << " smode2=0x" << g.smode2
                                          << " dispfb1=0x" << g.dispfb1 << " display1=0x" << g.display1
                                          << " dispfb2=0x" << g.dispfb2 << " display2=0x" << g.display2
                                          << std::dec << std::endl;
                            }
                            if (std::getenv("TMB_TRACE_IOP"))
                            {
                                const auto iop = runtime.iopDebugSnapshot();
                                for (const std::string &line : iop.diagnostics)
                                {
                                    std::cerr << "  [iop] " << line << std::endl;
                                }
                            }
                            for (const EeSemaphoreSnapshot &sem : snap.semaphores)
                            {
                                if (sem.waiters != 0u)
                                {
                                    std::cerr << "  sema" << sem.id << " count=" << sem.count << "/" << sem.maxCount
                                              << " waiters=" << sem.waiters << std::endl;
                                }
                            }
                        } })
            .detach();
    }

    // End of a movie. Every PSS ends with 4 bytes past its last sector (the program end code); the
    // player treats the stream as complete with < 5 bytes left and never reads them. It then calls
    // videoDecFlush, which appends a sequence_end_code to the IPU ring so the decoder finishes. With
    // libmpeg HLE'd, tell the HLE the input is complete once that flush succeeds.
    constexpr uint32_t kVideoDecFlush = 0x001FF9E0u;

    void videoDecFlushOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t videoDec = getRegU32(ctx, 4);
        videoDecFlush__FP8VideoDec_0x1ff9e0(rdram, ctx, runtime);
        if (getRegU32(ctx, 2) != 0u)
        {
            ps2_stubs::endMpegPlaybackInput(runtime, videoDec);
        }
    }

    // TMB_TRACE_SND=1: log every 989snd command sent with snd_SendIOPCommandAndWait (bring-up aid).
    constexpr uint32_t kSndSendIopCommandAndWait = 0x001BCE28u;

    void sndSendIopCommandAndWaitTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t s_calls = 0;
        const uint32_t command = getRegU32(ctx, 4);
        const int32_t count = static_cast<int32_t>(getRegU32(ctx, 5));
        const uint32_t args = getRegU32(ctx, 6);
        const uint32_t n = s_calls++;
        std::cerr << std::hex << "[tmb:snd] #" << std::dec << n << " cmd=0x" << std::hex << command << " argc=" << std::dec << count;
        for (int32_t i = 0; i < count && i < 6; ++i)
        {
            std::cerr << std::hex << " 0x" << *reinterpret_cast<const uint32_t *>(rdram + ((args + 4u * static_cast<uint32_t>(i)) & 0x1FFFFFFu));
        }
        std::cerr << std::dec << std::endl;
        snd_SendIOPCommandAndWait_0x1bce28(rdram, ctx, runtime);
        std::cerr << "[tmb:snd] #" << n << " -> 0x" << std::hex << getRegU32(ctx, 2) << std::dec << std::endl;
    }

    // TMB_TRACE_HIER=1: count VIF1 DMA completion handler calls of the hier renderer (bring-up aid).
    constexpr uint32_t kHierDmaHandler = 0x001D63A0u;
    std::atomic<uint32_t> g_hierDmaCalls{0};

    void hierDmaHandlerTrace(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t channel = getRegU32(ctx, 4);
        const auto rd = [&](uint32_t addr) { return *reinterpret_cast<const uint32_t *>(rdram + (addr & 0x1FFFFFFu)); };
        const uint32_t before0 = rd(0x00503CF8u), before1 = rd(0x00503CFCu);
        hierDmaHandler__Fi_0x1d63a0(rdram, ctx, runtime);
        const uint32_t n = g_hierDmaCalls.fetch_add(1);
        if (n < 20u || (n % 500u) == 0u)
            std::cerr << "[tmb:hier] dma handler #" << n << " ch=" << channel << " flags " << before0 << "," << before1
                      << " -> " << rd(0x00503CF8u) << "," << rd(0x00503CFCu) << std::endl;
    }

    // The game measures each frame with T0 (BUSCLK/256, 576 ticks per ms) into a global that timerGetFrameTime turns
    // into milliseconds, and integrates the car physics over it without a limit. Built for 50 fps (20 ms), it blows
    // up at the 80-100 ms frames of a slow host: cars fly off or fall through the arena. Capped at 40 ms
    // (TMB_MAX_FRAME_MS), the game runs in slow motion instead when the host cannot keep up.
    constexpr uint32_t kTimerEndOfRealTimeProcessing = 0x001F7A00u;
    constexpr int32_t kFrameTicksGpOffset = -0x2BDC; // timerGetFrameTime's tick count (gp-relative)
    int32_t g_maxFrameTicks = 40 * 576;

    void timerEndOfRealTimeProcessingOverride(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t gp = getRegU32(ctx, 28);
        timerEndOfRealTimeProcessing__Fv_0x1f7a00(rdram, ctx, runtime);
        int32_t *frameTicks = reinterpret_cast<int32_t *>(rdram + ((gp + kFrameTicksGpOffset) & 0x1FFFFFFu));
        if (*frameTicks > g_maxFrameTicks)
            *frameTicks = g_maxFrameTicks;
    }

    // TMB_TRACE_BLACKBOX=<file>: the player vehicle (0x259c70, 0x2200 bytes) at every collisCheckHdResults; when its
    // position goes past 50000 the last 64 frames are written to <file> (bring-up aid for the physics blow-ups).
    constexpr uint32_t kPlayerVehicle = 0x00259C70u;
    constexpr uint32_t kVehicleBytes = 0x2200u;
    constexpr uint32_t kBlackboxFrames = 64u;
    std::vector<uint8_t> g_blackbox;
    uint32_t g_blackboxNext = 0;
    bool g_blackboxDumped = false;
    const char *g_blackboxPath = nullptr;

    void collisCheckHdResultsBlackbox(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (getRegU32(ctx, 4) == kPlayerVehicle && !g_blackboxDumped)
        {
            if (g_blackbox.empty())
                g_blackbox.resize(kBlackboxFrames * kVehicleBytes);
            std::memcpy(g_blackbox.data() + (g_blackboxNext % kBlackboxFrames) * kVehicleBytes, rdram + kPlayerVehicle,
                        kVehicleBytes);
            ++g_blackboxNext;
            float pos[3];
            std::memcpy(pos, rdram + kPlayerVehicle + 0x260u, sizeof(pos));
            if (!(std::fabs(pos[0]) < 50000.0f && std::fabs(pos[1]) < 50000.0f && std::fabs(pos[2]) < 50000.0f))
            {
                g_blackboxDumped = true;
                if (FILE *out = std::fopen(g_blackboxPath, "wb"))
                {
                    for (uint32_t i = 0; i < kBlackboxFrames; ++i)
                        std::fwrite(g_blackbox.data() + ((g_blackboxNext + i) % kBlackboxFrames) * kVehicleBytes, 1,
                                    kVehicleBytes, out);
                    std::fclose(out);
                }
                std::fprintf(stderr, "[tmb:blackbox] position %g %g %g at frame %u: dumped\n", pos[0], pos[1], pos[2],
                             g_blackboxNext);
            }
        }
        collisCheckHdResults__7Vehicle_0x117ad8(rdram, ctx, runtime);
    }

    void applyTmb(PS2Runtime &runtime)
    {
        // The match is bound by the software rasterizer: draw on worker threads (TMB_GS_SERIAL=1 keeps the GS
        // thread alone). Four workers beat eight here, every worker sets up each of the many small triangles.
        if (const char *serial = std::getenv("TMB_GS_SERIAL"); !(serial && *serial == '1'))
        {
            setenv("PS2X_GS_RASTER_THREADS", "4", 0);
            GSCpuBackend::SetAsyncRasterization(true);
        }
        if (const char *trace = std::getenv("TMB_TRACE_HIER"); trace && *trace == '1')
        {
            runtime.registerFunction(kHierDmaHandler, &hierDmaHandlerTrace);
        }
        if (const char *trace = std::getenv("TMB_TRACE_SND"); trace && *trace == '1')
        {
            runtime.registerFunction(kSndSendIopCommandAndWait, &sndSendIopCommandAndWaitTrace);
        }
        runtime.registerFunction(kVideoDecFlush, &videoDecFlushOverride);
        if (const char *maxMs = std::getenv("TMB_MAX_FRAME_MS"); maxMs && *maxMs)
            g_maxFrameTicks = std::max(1, std::atoi(maxMs)) * 576;
        runtime.registerFunction(kTimerEndOfRealTimeProcessing, &timerEndOfRealTimeProcessingOverride);
        if (const char *path = std::getenv("TMB_TRACE_BLACKBOX"); path && *path)
        {
            g_blackboxPath = path;
            runtime.registerFunction(0x00117AD8u, &collisCheckHdResultsBlackbox);
        }
        if (const char *period = std::getenv("TMB_TRACE_THREADS"); period && *period)
        {
            startThreadTrace(runtime, std::max(0.5, std::atof(period)));
        }
        runtime.registerFunction(kVideoDecAbort, &videoDecAbortOverride);
        if (const char *trace = std::getenv("TMB_TRACE_FONT"); trace && *trace == '1')
        {
            runtime.registerFunction(kFontDmaFontData, &fontDmaFontDataTrace);
        }
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
