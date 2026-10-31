// Native PC rendering, TMB's adapter (docs/RECOMP_NOTES.md "Native PC port"): TMB's 3D objects read from the game's
// own data and drawn by the fork's native renderer (ps2x::native::Renderer), without running VU1 code.
//
// TMB's hier renderer sends its objects to VU1 as VIF1 DMA chains, in batches from several places in its code
// (hierObjectNode, hierTraverseAsm, hierFlushObjQ). The fork hands every VIF1 stream to this adapter as the game sends
// it (ps2x::native::setVif1Observer), and the adapter keeps VU1's data memory the way the stream fills it: the
// frame's camera and lights (VU 9-26), then per object its matrix (VU 0-3), lights (6-8, and 165-175 for the
// point-light mode), material (150-152: the A+D block with TEX0), texture offset and colour scale (4-5), a setup
// microprogram start (its entry picks the shading mode), its vertex pool (count at 180, positions from 181), the
// vertex pass, and one MSCNT per strip (a GIF tag and three quadwords per vertex: normal, colour, texture
// coordinates). Each strip becomes triangles with the formulas the microprograms compute (tools/tmb_mesh_decode.py
// checks them against VU1); the frame is published at the end of TMB's real-time processing.
//
// TMB_NATIVE=1 turns it on (the GS still draws its own frame under it; PS2X_NATIVE_VIEW=only shows the native one).

#include "ps2_runtime.h"
#include "runtime/native/native_renderer.h"
#include "runtime/native/native_vif1.h"
#include "runtime/native/native_vu1_sandbox.h"
#include "runtime/ps2_memory.h"
#include "tmb_region.h" // TMB_REGION_US / TMB_REGION_PAL

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tmb::native
{
    namespace
    {

        enum class Mode
        {
            None,
            Unlit,
            Scaled,
            Directional,
            Point,
            Sprite, // camera-facing quads (glows, smoke, light halos): 0x3000, one per MSCNT
            Rain,   // the effects image's rain lines (0x1d40)
            Ribbon, // beams and trails (0x3270): two screen-space vertices per point, a triangle strip
            Reflective, // chrome, glass and water strips (0x1db8): environment-mapped, alpha by the view angle
            Particles, // the effects image's sprites (0x390, its MSCNTs at 0x588 / 0x748): one screen-space quad each
            LensFlares, // the effects image's lens flares (0x2338): screen-space sprites from the lights' visibility
            Streaks, // the effects image's streaks (0x118): fading lines; an MSCNT after it runs 0x390's code
            Trails, // the effects image's trails (0x1830): sprites along a curve through control points
            TexParticles, // the effects image's textured particles (0x798, MSCNTs at 0x9b8): a TEX0 per sprite
            HeatHaze, // the effects image's heat haze (0xa08): fans textured with the frame itself
            Lightning, // the effects image's lightning (0x1198): subdivided bolts as strips; its MSCNT runs 0x1830
            Beams, // the effects image's beams (0xcf8): a strip along a list of points
            SkinDirectional, // skinned strips (0x640, after setup 0x50 / 0x70): directional light on the pool's normals
            SkinUnlit, // skinned strips without light (0x878 after setup 0x90, 0x9e8 after 0xb0): colour * VU 5 + VU 20
        };

        Mode modeForSetup(uint32_t pc)
        {
            switch (pc)
            {
            case 0x0cc8: return Mode::Unlit;
            case 0x0d08: return Mode::Scaled;
            case 0x0ce8: return Mode::Directional;
            case 0x1700: return Mode::Point;
            case 0x3000: return Mode::Sprite;
            case 0x3270: return Mode::Ribbon;
            case 0x1db8: return Mode::Reflective;
            case 0x0050:
            case 0x0070: return Mode::SkinDirectional;
            case 0x0090:
            case 0x00b0: return Mode::SkinUnlit;
            default: return Mode::None; // other programs (0x20d0, 0x1db8): not native yet
            }
        }

        // The microprograms this adapter draws, recognised by the lower instruction at each entry (so only while their
        // code image is loaded). Setups (and programs that stand alone): the hier image's strip setups, skinned setups
        // (0x50, 0x70: strips at 0x640; 0x90, 0xb0: unlit), sprites, beams and trails (0x3270), chrome and water (0x1db8),
        // and the effects image's programs (rain, particles, flares, streaks, trails, heat haze, lightning, beams).
        // Vertex passes (0x00 rigid, 0x10 morph, 0x20 / 0x30 / 0x40 skinned with three, two, one bones) run after a setup
        // and are drawn natively exactly when that setup is. VU1 skips what is drawn here (and the MSCNTs that continue
        // it); a program not listed is not run at all (reported once).
        struct Program
        {
            uint32_t pc, lower;
        };
        constexpr Program kSetups[] = {{0x0cc8u, 0x420f01e7u}, {0x0ce8u, 0x420f01e3u}, {0x0d08u, 0x420f01dfu},
                                       {0x1700u, 0x420f00a0u}, {0x0050u, 0x420f0376u}, {0x0070u, 0x420f0372u},
                                       {0x3000u, 0x420f0580u}, {0x1d40u, 0x420f07deu}, {0x3270u, 0x420f0532u},
                                       {0x1db8u, 0x420f07c9u},
                                       // The effects image: its constants (0x0, LOI) and its particle sprites (0x390).
                                       {0x0000u, 0x4b000000u}, {0x0390u, 0x800206bcu}, {0x2338u, 0x0824002cu},
                                       {0x0118u, 0x10020218u},
                                       {0x0090u, 0x420f036eu}, {0x00b0u, 0x420f036au}, {0x1830u, 0x800206bcu},
                                       {0x0798u, 0x800206bcu}, {0x0a08u, 0x800206bcu},
                                       {0x1198u, 0x01f4012cu}, {0x0cf8u, 0x800206bcu}};
        constexpr Program kVertexPasses[] = {{0x0000u, 0x400001d4u}, {0x0010u, 0x400001aeu}, {0x0020u, 0x40000015u},
                                             {0x0030u, 0x40000058u}, {0x0040u, 0x40000091u}};

        template <size_t N>
        bool matches(const Program (&programs)[N], const uint8_t *vu1Code, uint32_t startPc)
        {
            for (const Program &program : programs)
                if (program.pc == startPc)
                {
                    uint32_t lower;
                    std::memcpy(&lower, vu1Code + startPc, 4);
                    return lower == program.lower;
                }
            return false;
        }

        // The same decision on the VIF1 thread (the gate) and in the adapter (the stream), each with its own state,
        // over the same MSCALs in the same order.

        bool decide(bool &lastSetupNative, const uint8_t *vu1Code, uint32_t startPc)
        {
            // TMB_NATIVE_HIDE=<pc>[,<pc>...] (hex): VU1 skips those programs and nothing draws them (to see what they are).
            static const std::vector<uint32_t> s_hidden = []
            {
                std::vector<uint32_t> pcs;
                if (const char *value = std::getenv("TMB_NATIVE_HIDE"))
                    for (char *end = nullptr; *value; value = *end ? end + 1 : end)
                    {
                        pcs.push_back(static_cast<uint32_t>(std::strtoul(value, &end, 16)));
                        if (end == value)
                            break;
                    }
                return pcs;
            }();
            for (uint32_t pc : s_hidden)
                if (pc == startPc)
                {
                    lastSetupNative = false;
                    return true;
                }
            if (matches(kVertexPasses, vu1Code, startPc))
                return lastSetupNative;
            lastSetupNative = matches(kSetups, vu1Code, startPc);
            return lastSetupNative;
        }

        // A match is under way while its frames end (the end of real-time processing, onFrameEnd) less than 100 ms
        // apart on the host's clock; the menus (the shell overlay's loop) end none.
        double hostSeconds()
        {
            static const auto s_start = std::chrono::steady_clock::now();
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - s_start).count();
        }
        double g_lastMatchFrame = -1.0, g_lastMenuFrame = -1.0;
        bool inMatch() { return g_lastMatchFrame >= 0.0 && hostSeconds() - g_lastMatchFrame < 0.1; }

        // The menus natively too (TMB_NATIVE_MENUS=0: left to VU1 and the GS): they only run the translated strip
        // setups 0xd08 and 0xcc8. Their frames end at sceGsSyncV (onMenuFrameEnd, with native frames).
        bool menusNative()
        {
            static const bool s_on = []
            {
                const char *value = std::getenv("TMB_NATIVE_MENUS");
                return !(value && *value == '0');
            }();
            return s_on && !inMatch() && g_lastMenuFrame >= 0.0 && hostSeconds() - g_lastMenuFrame < 0.1;
        }

        // What the adapter decided for each MSCAL as it read the stream (at the transfer's start, on the EE thread),
        // for the VU1 gate when VIF1 gets to it: the two agree program for program (the GS stream's groups count
        // the skipped ones on both sides), whatever changed in between (a match starting, a menu).
        struct Decision
        {
            uint32_t pc = 0;      // an MSCAL's start, or a DIRECT's size in quadwords
            bool skipped = false;
            bool direct = false;
        };
        std::mutex g_decisionsMutex;
        std::deque<Decision> g_decisions;

        void noteDecision(uint32_t pc, bool skipped, bool direct = false)
        {
            std::lock_guard<std::mutex> lock(g_decisionsMutex);
            g_decisions.push_back({pc, skipped, direct});
            if (g_decisions.size() > 65536u) // nobody takes them (no gate)
                g_decisions.pop_front();
        }

        bool drawnNatively(const uint8_t *vu1Code, uint32_t startPc)
        {
            (void)vu1Code;
            std::lock_guard<std::mutex> lock(g_decisionsMutex);
            while (!g_decisions.empty())
            {
                const Decision decision = g_decisions.front();
                g_decisions.pop_front();
                if (!decision.direct && decision.pc == startPc)
                    return decision.skipped;
            }
            return false; // a transfer the adapter did not see: VU1 draws it
        }

        // The DIRECT packets the adapter drew (the particles): taken in the same queue, in stream order. A DIRECT the
        // VIF1 interpreter does not ask about (cut, or finishing an image) leaves its entry for drawnNatively to drop.
        bool directDrawnNatively(uint32_t qwords)
        {
            std::lock_guard<std::mutex> lock(g_decisionsMutex);
            while (!g_decisions.empty() && g_decisions.front().direct)
            {
                const Decision decision = g_decisions.front();
                g_decisions.pop_front();
                if (decision.pc == qwords)
                    return decision.skipped;
            }
            return false;
        }

        // A DIRECT packet that only draws (vertex kicks, no image transfer): the particles TMB sends that way.
        bool drawsOnly(const uint32_t *words, size_t qwords)
        {
            bool draws = false;
            size_t q = 0;
            while (q < qwords)
            {
                const uint64_t lo = words[q * 4] | (static_cast<uint64_t>(words[q * 4 + 1]) << 32);
                const uint64_t regs = words[q * 4 + 2] | (static_cast<uint64_t>(words[q * 4 + 3]) << 32);
                ++q;
                const uint32_t nloop = lo & 0x7fffu, flg = (lo >> 58) & 3u;
                const uint32_t nreg = ((lo >> 60) & 0xfu) ? static_cast<uint32_t>((lo >> 60) & 0xfu) : 16u;
                if (flg >= 2u)
                    return false; // an image transfer: the GS (and the texture cache) must have it
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    const uint32_t reg = (regs >> (4u * r)) & 0xfu;
                    draws = draws || reg == 0x4u || reg == 0x5u;
                }
                if (flg == 0u)
                {
                    for (uint32_t i = 0; i < nloop * nreg && q < qwords; ++i, ++q)
                        if (((regs >> (4u * (i % nreg))) & 0xfu) == 0xeu)
                        {
                            const uint32_t address = words[q * 4 + 2] & 0xffu;
                            if (address == 0x04u || address == 0x05u)
                                draws = true;
                            else if (address >= 0x50u && address <= 0x54u)
                                return false; // a transfer set up (BITBLTBUF, TRXPOS, TRXREG, TRXDIR, HWREG)
                        }
                }
                else
                    q += (static_cast<size_t>(nloop) * nreg + 1u) / 2u;
            }
            return draws;
        }

        // VU1 data memory as the chain fills it: 1024 quadwords, read as words or floats.
        struct VuImage
        {
            uint32_t words[1024 * 4] = {};
            float f(uint32_t qw, int lane) const
            {
                float v;
                std::memcpy(&v, &words[(qw & 0x3ffu) * 4 + lane], 4);
                return v;
            }
            uint32_t w(uint32_t qw, int lane) const { return words[(qw & 0x3ffu) * 4 + lane]; }
        };

        const uint8_t *g_rdram = nullptr; // guest memory, from the first hier (setGuestMemory)

        // The fog colour the GS fogs with: fogUpdate's A+D packet (FOGCOL, then register 0x3d), which the game sends
        // outside the VIF1 stream. Until the game's memory is known, the stream's own DIRECT FOGCOL.
        uint32_t gameFogColour(uint32_t streamColour)
        {
            if (!g_rdram)
                return streamColour;
            constexpr uint32_t kFogPacket = TMB_ADDR(0x004A25D0u);
            if (g_rdram[(kFogPacket + 8u) & 0x1ffffffu] != 0x3du)
                return streamColour;
            uint32_t value;
            std::memcpy(&value, g_rdram + (kFogPacket & 0x1ffffffu), sizeof(value));
            return value & 0xffffffu;
        }

        struct State
        {
            VuImage vu;
            uint32_t cl = 1, wl = 1;   // STCYCL
            uint32_t base = 0, offset = 0, tops = 0, top = 0, itops = 0;
            VIFRegisters vif{}; // STMOD, STMASK, STROW / STCOL (and STCYCL, TOPS) for vifUnpack
            // Which of them the observed streams set: the others come from VIF1's own registers (TMB sets its mask and
            // row once, in a transfer before the ones the adapter sees; without them the ribbons' texture coordinates
            // were wrong in the copy, and their segments with them).
            bool sawMask = false, sawRow = false, sawCol = false, sawMode = false;
            // The game's DIRECT packets that only draw (its 2D and particles in a match), drawn natively from their GIF data.
            std::unique_ptr<ps2x::native::GifToNative> direct;
            // TMB_NATIVE_CHECK=<n>: VU1's own program run beside the translation (checkTranslation), captured, not drawn.
            std::unique_ptr<ps2x::native::Vu1Sandbox> check;
            std::unique_ptr<ps2x::native::Vu1Sandbox> checkEffects; // the effects image's programs: their own VU1 and memory
            std::vector<ps2x::native::Vertex> checkVertices;
            std::vector<ps2x::native::Material> checkMaterials;
            uint32_t checkSetup = 0; // the translated setup the strips belong to
            bool checkResumed = false; // the check is of an MSCNT (else of an MSCAL)
            bool flushed = false;    // flush() ran (the check's way to tell a strip's own triangles)
            bool setupOnVu1 = false;                           // TMB_NATIVE_VU1_PCS
            Mode mode = Mode::None;
            float matrix[16] = {};     // object to GS screen: camera (VU 9-12) x object (VU 0-3), columns
            std::vector<float> pool;   // x, y, z per pool vertex (the object's space)
            uint64_t tex0 = 0;
            bool haveTexture = false;
            bool blended = false; // the strip's PRIM.ABE
            uint8_t code[16 * 1024] = {}; // VU1 micro memory as the stream's MPGs fill it
            uint64_t directTex0 = 0;       // the last TEX0_1 the stream's DIRECT packets set (the rain's texture)
            uint64_t group = 0;            // the natively drawn programs so far (Renderer::beginGroup)
            bool lastSetupNative = false;  // decide()'s state for the stream
            // A skinned pass's pool: 3 quadwords per vertex, the strips index it by quadword offset (181 + offset);
            // per offset / 3, the blended position and normal.
            std::vector<float> skinPositions, skinNormals;
            uint32_t setupPc = 0;
            // Objects keyed this frame (Renderer::setKey): their own data's hash -> how many so far, so identical
            // objects (two of the same barrel) keep their own key, by their order.
            std::unordered_map<uint64_t, uint32_t> keysThisFrame;
            uint64_t alpha = 0x44;         // ALPHA_1 as the stream's DIRECT packets set it (how blended draws blend)
            uint64_t batchAlpha = 0x44;    // the ALPHA_1 of the batch being built
            bool fogged = false;           // the strip's PRIM.FGE
            uint32_t fogColour = 0;        // FOGCOL as the stream's DIRECT packets set it (before the game's own is known)
            uint32_t prim = 0;             // PRIM as the stream's DIRECT and header A+D set it (a tag without PRE draws with it)
            uint64_t lastTex0 = 0;         // the last TEX0_1 the GS got (DIRECT or A+D): a packet without one draws with it
            float reflect[3] = {};         // 0x1db8's alpha terms (its setup's TOP .yzw: base, slope, floor)
            bool water = false;            // 0x1db8 with all three 0: the water variant (0x2208)
            bool reflectParams = false;    // 0x1db8's next MSCNT is its parameter step (0xf50 -> 0x1de0), not a strip
            float fxConst[4][4] = {};      // the effects image's VU 4-7 as its 0x0 loads them (vf29, vf27, vf28, vf31)
            float fxMatrix[16] = {};       // 0x390's screen matrix (L_0038): VU 0-3, times the batch's own when TOP.w < 0
            uint32_t fxNext = 0;           // the next particle's position (read downwards, across the MSCNTs)
            bool fxFade = false;           // 0x390's second variant (TOP.z & 0x8000): fog as alpha, no size clamp
            float fxBias = 0.0f;           // 0x798's depth term ((TOP + 4).x)
            bool sampleTarget = false;     // the batch samples the frame drawn into (the heat haze)
            size_t particleFrom = 0;       // TMB_NATIVE_CHECK: where an 0x390 MSCAL's own triangles start
            bool collecting = false;       // TMB_NATIVE_CHECK: an effect's MSCAL is drawing; flush() keeps its triangles
            std::vector<ps2x::native::Vertex> checkMine;
            uint64_t preTags = 0, primTags = 0; // TMB_NATIVE_LOG_PRE: tags with PRE, without
            uint32_t batchFogColour = 0;
            bool depthMasked = false;      // ZBUF_1.ZMSK as the stream's DIRECT packets (or a strip's material) set it
            bool batchDepthMasked = false;
            uint64_t test = 0;             // TEST_1 as the stream's DIRECT packets set it (TMB toggles its alpha test)
            uint64_t batchTest = 0;
        };

        struct Stats
        {
            uint64_t chains = 0, objects = 0, strips = 0, triangles = 0, textured = 0, skipped = 0;
        } g_stats;

        // The fog factor a microprogram writes in XYZF2's F, over 255: clamp(VU 4.w + VU 21.x + VU 21.w / w, 0, VU 21.y)
        // (the same in the strip and sprite programs; checked on 3000 strip vertices of the match trace).
        float fogFactor(const VuImage &vu, float q)
        {
            return std::min(std::max(vu.f(4, 3) + vu.f(21, 0) + vu.f(21, 3) * q, 0.0f), vu.f(21, 1)) / 255.0f;
        }

        uint32_t clampByte(float v)
        {
            if (!(v > 0.0f))
                return 0u;
            return v >= 255.0f ? 255u : static_cast<uint32_t>(v);
        }

        // The colour a strip's microprogram writes for one vertex (tools/tmb_mesh_decode.py, Strip.shade).
        uint32_t shade(const State &s, const uint32_t normal[3], const uint32_t rgba[4], const float *pos)
        {
            const VuImage &vu = s.vu;
            float c[4];
            // The unlit strips (0x10a8) scale their colour by VU 5 too, as the scaled ones (0xf60): colour * vf31 +
            // vf11. VU 5 is the object's tint (the health bar's green, orange, red).
            if (s.mode == Mode::Unlit || s.mode == Mode::Scaled)
                for (int k = 0; k < 4; ++k)
                    c[k] = static_cast<float>(rgba[k]) * vu.f(5, k) + vu.f(20, k);
            else if (s.mode == Mode::Directional)
            {
                float n[3], d[3];
                for (int k = 0; k < 3; ++k)
                    n[k] = 1.0f - static_cast<float>(normal[k] & 0xffu) / 128.0f;
                for (int i = 0; i < 3; ++i)
                    d[i] = std::max(0.0f, vu.f(6, i) * n[0] + vu.f(7, i) * n[1] + vu.f(8, i) * n[2]);
                for (int k = 0; k < 3; ++k)
                {
                    const float light = (vu.f(17, k) * d[0] + vu.f(18, k) * d[1] + vu.f(19, k) * d[2]) * vu.f(5, k);
                    c[k] = static_cast<float>(rgba[k]) * light + vu.f(20, k);
                }
                c[3] = static_cast<float>(rgba[3]) * vu.f(5, 3) + vu.f(20, 3);
            }
            else // point lights
            {
                float n[3];
                for (int k = 0; k < 3; ++k)
                    n[k] = static_cast<float>(normal[k] & 0xffu) / 127.0f - 128.0f / 127.0f;
                for (int k = 0; k < 4; ++k)
                    c[k] = static_cast<float>(rgba[k]) * vu.f(5, k) + vu.f(20, k);
                for (int i = 0; i < 3; ++i)
                {
                    const float d[3] = {-(vu.f(169, i) + pos[0]), -(vu.f(170, i) + pos[1]), -(vu.f(171, i) + pos[2])};
                    const float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
                    if (d2 <= 0.0f)
                        continue;
                    const float inv = 1.0f / std::sqrt(d2);
                    const float facing = std::max(0.0f, (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) * inv);
                    const float att = std::min(0.5f, facing * inv * inv * vu.f(175, i));
                    float cone = std::max(0.0f, (d[0] * vu.f(165, i) + d[1] * vu.f(166, i) + d[2] * vu.f(167, i)) * inv + vu.f(168, i));
                    cone *= cone;
                    cone *= cone;
                    for (int k = 0; k < 3; ++k)
                        c[k] += vu.f(172 + i, k) * att * cone;
                }
            }
            return clampByte(c[0]) | (clampByte(c[1]) << 8) | (clampByte(c[2]) << 16) | (clampByte(c[3]) << 24);
        }

        void setup(State &s, uint32_t pc)
        {
            s.mode = modeForSetup(pc);
            s.setupPc = pc;
            ps2x::native::Renderer::instance().setKey(0); // sprites: screen space, not interpolated
            ++g_stats.objects;
            g_stats.skipped += s.mode == Mode::None ? 1u : 0u;
            // M = camera (VU 9-12) x object (VU 0-3): column j of M is camera applied to column j of the object's.
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    s.matrix[j * 4 + k] = s.vu.f(9, k) * s.vu.f(j, 0) + s.vu.f(10, k) * s.vu.f(j, 1) +
                                          s.vu.f(11, k) * s.vu.f(j, 2) + s.vu.f(12, k) * s.vu.f(j, 3);
            // The rigid strip programs scale z / w by VU 4.z, the object's depth factor (MULz.z vf24, vf19z at 0x1018,
            // 0x1188, 0x1460, 0x16c8), as the sprites do (sprite() applies it itself): without it the scenery sat at
            // another depth than the glows and smoke drawn over it, and cut them. The skinned strips do not scale.
            // The unlit skinned loops (0x878, 0x9e8) scale it too (MULz.z vf24, vf24, vf6z at 0x948, 0xa90); the lit one does not.
            if (s.mode == Mode::Unlit || s.mode == Mode::Scaled || s.mode == Mode::Directional || s.mode == Mode::Point ||
                s.mode == Mode::Reflective || s.mode == Mode::SkinUnlit)
                for (int j = 0; j < 4; ++j)
                    s.matrix[j * 4 + 2] *= s.vu.f(4, 2);
            s.haveTexture = false;
            s.reflectParams = s.mode == Mode::Reflective;
        }

        // An object's key for interpolation (Renderer::setKey): its setup, its vertex count and its pool's words as
        // the stream brought them (object space, so the same from one frame to the next, a skinned one's bind pose
        // too), with how many objects had that hash before it this frame.
        void keyObject(State &s, uint32_t count, uint32_t qwords)
        {
            uint64_t hash = 1469598103934665603ull ^ (static_cast<uint64_t>(s.setupPc) << 32) ^ count;
            const uint32_t words = std::min(qwords, 64u) * 4u;
            for (uint32_t i = 0; i < words; ++i)
            {
                hash ^= s.vu.words[181u * 4u + i];
                hash *= 1099511628211ull;
            }
            const uint32_t occurrence = s.keysThisFrame[hash]++;
            ps2x::native::Renderer::instance().setKey((hash ^ (static_cast<uint64_t>(occurrence) * 0x9e3779b97f4a7c15ull)) | 1u);
        }

        // The skinned vertex passes (0x20 / 0x30 / 0x40 -> 0xd0 / 0x2f8 / 0x4d0): pool vertex i is 3 quadwords at
        // 181 + 3i: the position (weight 0 in w), the normal as bytes (n = 1 - v / 128; bone 0's offset in w), then
        // weight 1, bone 1, weight 2, bone 2 (x, y, z, w; two bones: x, y). A bone is 4 quadwords at TOP + its offset;
        // position and normal are the weighted sums of the bones' transforms (weights / 65536, one bone: unweighted).
        // Checked on 3171 vertices of a recorded two-bone pass (VU1's blended position and normal, within 1e-5).
        void skinnedPass(State &s, int bones)
        {
            const VuImage &vu = s.vu;
            const uint32_t count = vu.w(180, 3) & 0xffffu;
            keyObject(s, count, count * 3u);
            s.skinPositions.assign(static_cast<size_t>(count) * 3u, 0.0f);
            s.skinNormals.assign(static_cast<size_t>(count) * 3u, 0.0f);
            float weightScale;
            const uint32_t weightBits = 0x377ffff6u; // the program's 1/65536
            std::memcpy(&weightScale, &weightBits, 4);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t b = 181u + 3u * i;
                const float p[3] = {vu.f(b, 0), vu.f(b, 1), vu.f(b, 2)};
                const float n[3] = {1.0f - static_cast<float>(vu.w(b + 1, 0)) / 128.0f, 1.0f - static_cast<float>(vu.w(b + 1, 1)) / 128.0f,
                                    1.0f - static_cast<float>(vu.w(b + 1, 2)) / 128.0f};
                const uint32_t offset[3] = {vu.w(b + 1, 3), vu.w(b + 2, 1), vu.w(b + 2, 3)};
                const float weight[3] = {bones == 1 ? 1.0f : static_cast<float>(static_cast<int32_t>(vu.w(b, 3))) * weightScale,
                                         static_cast<float>(static_cast<int32_t>(vu.w(b + 2, 0))) * weightScale,
                                         static_cast<float>(static_cast<int32_t>(vu.w(b + 2, 2))) * weightScale};
                float *outP = &s.skinPositions[i * 3u], *outN = &s.skinNormals[i * 3u];
                for (int bone = 0; bone < bones; ++bone)
                {
                    const uint32_t m = s.top + (offset[bone] & 0x3ffu);
                    for (int k = 0; k < 3; ++k)
                    {
                        outP[k] += (vu.f(m, k) * p[0] + vu.f(m + 1, k) * p[1] + vu.f(m + 2, k) * p[2] + vu.f(m + 3, k)) * weight[bone];
                        outN[k] += (vu.f(m, k) * n[0] + vu.f(m + 1, k) * n[1] + vu.f(m + 2, k) * n[2]) * weight[bone];
                    }
                }
            }
        }

        // The morph pass (0x10 -> 0xd88): pool vertex i at 181 + i moved by its delta at 373 + i times VU 31.
        void morphPass(State &s)
        {
            const VuImage &vu = s.vu;
            const uint32_t count = vu.w(180, 3) & 0xffffu;
            keyObject(s, count, count);
            s.pool.resize(static_cast<size_t>(count) * 3u);
            for (uint32_t i = 0; i < count; ++i)
                for (int k = 0; k < 3; ++k)
                    s.pool[i * 3u + k] = vu.f(181 + i, k) + vu.f(373 + i, k) * vu.f(31, k);
        }

        void vertexPass(State &s)
        {
            const uint32_t count = s.vu.w(180, 3) & 0xffffu;
            keyObject(s, count, count);
            s.pool.resize(static_cast<size_t>(count) * 3u);
            for (uint32_t i = 0; i < count; ++i)
                for (int k = 0; k < 3; ++k)
                    s.pool[i * 3u + k] = s.vu.f(181 + i, k);
        }

        void flush(State &s, std::vector<ps2x::native::Vertex> &triangles);

        // A strip's or sprite's material: its texture (an A+D block at 149 + header, TEX0 among it, when it brings one)
        // and whether it blends (PRIM.ABE in its GIF tag). A new material ends the batch so far. False when the GIF tag
        // is not an STQ / RGBAQ / XYZF2 one.
        // TMB_NATIVE_LOG_AD=1: each GS register the stream sets (DIRECT packets, strips' materials), at each new value.
        void logRegister(const char *where, uint32_t address, uint64_t value)
        {
            static const bool s_log = std::getenv("TMB_NATIVE_LOG_AD") != nullptr;
            if (!s_log)
                return;
            static std::unordered_map<uint64_t, uint64_t> s_seen; // where's first letter and address -> value
            const uint64_t key = (static_cast<uint64_t>(where[0]) << 8) | address;
            const auto it = s_seen.find(key);
            if (it != s_seen.end() && it->second == value)
                return;
            static std::unordered_map<char, uint32_t> s_lines; // 400 per source
            if (++s_lines[where[0]] > 400u)
                return;
            s_seen[key] = value;
            std::fprintf(stderr, "[tmb:native] %s A+D %#x = %#llx\n", where, address, static_cast<unsigned long long>(value));
        }

        // regs: the tag's registers (the strips' ST, RGBAQ, XYZF2 by default); adAt / adCount: an A+D block of the
        // packet's own (the particles' TEX0) instead of a header at 149.
        bool useMaterial(State &s, uint32_t tagQw, uint32_t header, std::vector<ps2x::native::Vertex> &triangles,
                         uint32_t regs = 0x412u, uint32_t adAt = 0u, uint32_t adCount = 0u)
        {
            const uint32_t tag1 = s.vu.w(tagQw, 1), tag2 = s.vu.w(tagQw, 2);
            if (tag2 != regs)
                return false;
            uint64_t tex0 = s.tex0;
            bool textured = s.haveTexture;
            const uint32_t first = adCount != 0u ? adAt : 149u + header, count = adCount != 0u ? adCount : header != 0u ? 3u : 0u;
            for (uint32_t q = 0; q < count; ++q)
                {
                    const uint32_t qw = first + q;
                    logRegister("material", s.vu.w(qw, 2) & 0xffu, static_cast<uint64_t>(s.vu.w(qw, 0)) | (static_cast<uint64_t>(s.vu.w(qw, 1)) << 32));
                    if ((s.vu.w(qw, 2) & 0xffu) == 0x00u) // PRIM
                        s.prim = s.vu.w(qw, 0) & 0x7ffu;
                    if ((s.vu.w(qw, 2) & 0xffu) == 0x42u) // ALPHA_1 (the flares' packet sets its own)
                        s.alpha = static_cast<uint64_t>(s.vu.w(qw, 0)) | (static_cast<uint64_t>(s.vu.w(qw, 1)) << 32);
                    if ((s.vu.w(qw, 2) & 0xffu) == 0x47u) // TEST_1
                        s.test = static_cast<uint64_t>(s.vu.w(qw, 0)) | (static_cast<uint64_t>(s.vu.w(qw, 1)) << 32);
                    if ((s.vu.w(qw, 2) & 0xffu) == 0x4eu) // ZBUF_1
                        s.depthMasked = (s.vu.w(qw, 1) & 1u) != 0u;
                    if ((s.vu.w(qw, 2) & 0xffu) == 0x06u) // TEX0_1
                    {
                        tex0 = s.lastTex0 = static_cast<uint64_t>(s.vu.w(qw, 0)) | (static_cast<uint64_t>(s.vu.w(qw, 1)) << 32);
                        textured = true;
                    }
                }
            // The tag's PRIM only with PRE set; without it the GS draws with its PRIM register (PRMODECONT is 1).
            const bool pre = ((tag1 >> 14) & 1u) != 0u;
            const uint32_t attributes = pre ? (tag1 >> 15) & 0x7ffu : s.prim;
            const bool blended = (attributes & 0x40u) != 0u, fogged = (attributes & 0x20u) != 0u;
            if (regs != 0x412u) // an effects packet: textured by PRIM.TME, with its own TEX0 or the GS's last
            {
                if (count == 0u)
                    tex0 = s.lastTex0;
                textured = (attributes & 0x10u) != 0u;
            }
            ++(pre ? s.preTags : s.primTags);
            static const bool s_logPre = std::getenv("TMB_NATIVE_LOG_PRE") != nullptr;
            if (s_logPre && (s.preTags + s.primTags) % 20000u == 0u)
                std::fprintf(stderr, "[tmb:native] strip tags: %llu with PRE, %llu without (PRIM %#x)\n",
                             static_cast<unsigned long long>(s.preTags), static_cast<unsigned long long>(s.primTags), s.prim);
            if (fogged)
                s.fogColour = gameFogColour(s.fogColour);
            if (tex0 != s.tex0 || textured != s.haveTexture || blended != s.blended || (blended && s.alpha != s.batchAlpha) ||
                fogged != s.fogged || (fogged && s.fogColour != s.batchFogColour) || s.depthMasked != s.batchDepthMasked ||
                s.test != s.batchTest)
            {
                flush(s, triangles);
                s.tex0 = tex0;
                s.haveTexture = textured;
                s.blended = blended;
                s.batchAlpha = s.alpha;
                s.fogged = fogged;
                s.batchFogColour = s.fogColour;
                s.batchDepthMasked = s.depthMasked;
                s.batchTest = s.test;
            }
            return true;
        }

        // One sprite of the 0x3000 program (its loop at 0x3030, re-entered at 0x3260): a centre (VU top+1, the size in
        // w), a colour (top+2, scaled like the scaled mode) and four corners' texture coordinates (top+3..6, 4.12). The
        // centre goes to the screen as (M * p) / w; the quad around it is 608 x 304 pixels per unit of size and of 1/w
        // (square on TMB's 640 x 256 field), corners (+x -y), (-x -y), (+x +y), (-x +y). Its vertices are in screen
        // space, drawn with an identity matrix.
        void sprite(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            const uint32_t top = s.top;
            if (!useMaterial(s, top, s.vu.w(top, 3), triangles))
                return;
            const VuImage &vu = s.vu;
            const float p[3] = {vu.f(top + 1, 0), vu.f(top + 1, 1), vu.f(top + 1, 2)}, size = vu.f(top + 1, 3);
            float c[4];
            for (int k = 0; k < 4; ++k)
                c[k] = s.matrix[k] * p[0] + s.matrix[4 + k] * p[1] + s.matrix[8 + k] * p[2] + s.matrix[12 + k];
            if (c[3] <= 0.0f)
                return; // behind the camera (VU1 culls it by its clip test)
            const float q = 1.0f / c[3];
            const float x = c[0] * q, y = c[1] * q, z = c[2] * q * vu.f(4, 2);
            const float hx = 608.0f * size * q, hy = 304.0f * size * q;
            float rgba[4];
            for (int k = 0; k < 4; ++k)
                rgba[k] = static_cast<float>(vu.w(top + 2, k)) * vu.f(5, k) + vu.f(20, k);
            const uint32_t colour = clampByte(rgba[0]) | (clampByte(rgba[1]) << 8) | (clampByte(rgba[2]) << 16) | (clampByte(rgba[3]) << 24);
            const float corner[4][2] = {{x + hx, y - hy}, {x - hx, y - hy}, {x + hx, y + hy}, {x - hx, y + hy}};
            ps2x::native::Vertex quad[4];
            for (int i = 0; i < 4; ++i)
            {
                quad[i].x = corner[i][0];
                quad[i].y = corner[i][1];
                quad[i].z = z;
                quad[i].u = static_cast<float>(static_cast<int32_t>(vu.w(top + 3 + i, 0))) / 4096.0f;
                quad[i].v = static_cast<float>(static_cast<int32_t>(vu.w(top + 3 + i, 1))) / 4096.0f;
                quad[i].rgba = colour;
                quad[i].fog = fogFactor(vu, q);
            }
            const int order[6] = {0, 1, 2, 1, 2, 3};
            for (int i : order)
                triangles.push_back(quad[i]);
        }

        // A skinned strip (0x640): with a texture header, VU memory holds an A+D GIF tag at TOP (the header in its w) and
        // the strip's tag at TOP + 4, else the strip's tag at TOP; vertex k at TOP + 17 + 3k: the pool offset (w), the
        // colour, the texture coordinates (4.12, plus VU 4). Directional light on the pool's blended normal:
        // colour * (VU 17-19 . max(0, VU 6-8 . n)) * VU 5 + VU 20. All 4005 checked vertices of the recorded match
        // (colour, screen position, texture coordinates) match VU1. Unlit (0x878, 0x9e8: setups 0x90, 0xb0): the same
        // layout, the colour * VU 5 + VU 20 (at most 255).
        void skinnedStrip(State &s, std::vector<ps2x::native::Vertex> &triangles, bool lit = true)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            const uint32_t top = s.top;
            const uint32_t header = vu.w(top, 3);
            const uint32_t tagQw = header != 0u ? top + 4u : top;
            if (!useMaterial(s, tagQw, header, triangles))
                return;
            const uint32_t nloop = vu.w(tagQw, 0) & 0x7fffu;
            std::vector<ps2x::native::Vertex> strip;
            strip.reserve(nloop);
            for (uint32_t k = 0; k < nloop; ++k)
            {
                const uint32_t q = top + 17u + 3u * k;
                const uint32_t offset = vu.w(q, 3) & 0x7fffu;
                ps2x::native::Vertex out;
                if (offset % 3u != 0u || static_cast<size_t>(offset) + 2u >= s.skinPositions.size())
                {
                    out.rgba = 0xdeadu; // a break, as in strip()
                    strip.push_back(out);
                    continue;
                }
                const float *pos = &s.skinPositions[offset], *n = &s.skinNormals[offset];
                out.x = pos[0];
                out.y = pos[1];
                out.z = pos[2];
                float d[3], c[4];
                for (int i = 0; i < 3; ++i)
                    d[i] = std::max(0.0f, vu.f(6, i) * n[0] + vu.f(7, i) * n[1] + vu.f(8, i) * n[2]);
                for (int k2 = 0; k2 < 3; ++k2)
                    c[k2] = static_cast<float>(vu.w(q + 1, k2)) * (lit ? vu.f(17, k2) * d[0] + vu.f(18, k2) * d[1] + vu.f(19, k2) * d[2] : 1.0f) *
                                vu.f(5, k2) +
                            vu.f(20, k2);
                c[3] = static_cast<float>(vu.w(q + 1, 3)) * vu.f(5, 3) + vu.f(20, 3);
                out.rgba = clampByte(c[0]) | (clampByte(c[1]) << 8) | (clampByte(c[2]) << 16) | (clampByte(c[3]) << 24);
                out.u = static_cast<float>(static_cast<int32_t>(vu.w(q + 2, 0))) / 4096.0f + vu.f(4, 0);
                out.v = static_cast<float>(static_cast<int32_t>(vu.w(q + 2, 1))) / 4096.0f + vu.f(4, 1);
                const float w = s.matrix[3] * pos[0] + s.matrix[7] * pos[1] + s.matrix[11] * pos[2] + s.matrix[15];
                out.fog = w > 0.0f ? fogFactor(vu, 1.0f / w) : 0.0f;
                strip.push_back(out);
            }
            for (size_t i = 2; i < strip.size(); ++i)
            {
                if (strip[i].rgba == 0xdeadu || strip[i - 1].rgba == 0xdeadu || strip[i - 2].rgba == 0xdeadu)
                    continue;
                triangles.push_back(strip[i - 2]);
                triangles.push_back(strip[i - 1]);
                triangles.push_back(strip[i]);
            }
        }

        // A beam or trail (0x3270, its MSCNTs from 0x3290): N points from TOP (N in the tag at TOP, the header in its
        // w). Point k is projected through the object's matrix (screen = M p, q = 1 / w) and widened on screen to two
        // vertices, x + r and x - r with r = 608 p.w q, x and y held to 0..4095; their texture coordinates (4.12, plus
        // VU 4.xy, over Q) and colours (times VU 5, plus VU 20, at most 255) come in pairs from TOP. Fog as the strips
        // (VU 4.w + VU 21.x + VU 21.w q, 0 to VU 21.y). Both vertices of a point take ADC (no triangle) when the point
        // is outside the clip volume (VU 13-16 x VU 0-3, the setup's last vf13-16, on p with y - 2: CLIP) or every other point (the count left,
        // N - 1 - k, odd): the strip is drawn as separate segments. The loop is software-pipelined: the reads below
        // follow it (point k + 1 read while k is written, 3 or 6 quadwords on by the parity).
        void ribbon(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            const uint32_t top = s.top;
            if (!useMaterial(s, top, vu.w(top, 3), triangles))
                return;
            const uint32_t count = vu.w(top, 0) & 0x7fffu;
            float clip[16];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    clip[j * 4 + k] = vu.f(13, k) * vu.f(j, 0) + vu.f(14, k) * vu.f(j, 1) + vu.f(15, k) * vu.f(j, 2) + vu.f(16, k) * vu.f(j, 3);
            const auto colourAt = [&](uint32_t qw)
            {
                float c[4];
                for (int k = 0; k < 4; ++k)
                    c[k] = std::min(static_cast<float>(static_cast<int32_t>(vu.w(qw, k))) * vu.f(5, k) + vu.f(20, k), 255.0f);
                return clampByte(c[0]) | (clampByte(c[1]) << 8) | (clampByte(c[2]) << 16) | (clampByte(c[3]) << 24);
            };
            const auto uvAt = [&](uint32_t qw, ps2x::native::Vertex &out)
            {
                const float q = static_cast<float>(static_cast<int32_t>(vu.w(qw, 2))) / 4096.0f;
                const float sOff = static_cast<float>(static_cast<int32_t>(vu.w(qw, 0))) / 4096.0f + vu.f(4, 0);
                const float tOff = static_cast<float>(static_cast<int32_t>(vu.w(qw, 1))) / 4096.0f + vu.f(4, 1);
                out.u = q != 0.0f ? sOff / q : sOff;
                out.v = q != 0.0f ? tOff / q : tOff;
            };
            std::vector<ps2x::native::Vertex> strip;
            std::vector<bool> adc;
            strip.reserve(count * 2u);
            adc.reserve(count * 2u);
            uint32_t position = top + 1u, next = top + 4u;
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t parity = (count - 1u - k) & 1u;
                const float p[4] = {vu.f(position, 0), vu.f(position, 1), vu.f(position, 2), vu.f(position, 3)};
                position = next;
                next += parity ? 3u : 6u;
                float c[4], cc[4];
                for (int i = 0; i < 4; ++i)
                {
                    c[i] = s.matrix[i] * p[0] + s.matrix[4 + i] * p[1] + s.matrix[8 + i] * p[2] + s.matrix[12 + i];
                    cc[i] = clip[i] * p[0] + clip[4 + i] * (p[1] - 2.0f) + clip[8 + i] * p[2] + clip[12 + i];
                }
                const float q = 1.0f / c[3];
                const float aw = std::fabs(cc[3]);
                const bool clipped = std::fabs(cc[0]) > aw || std::fabs(cc[1]) > aw || std::fabs(cc[2]) > aw;
                const float x = c[0] * q, y = std::min(std::max(c[1] * q, 0.0f), 4095.0f), z = c[2] * q * vu.f(4, 2);
                const float r = p[3] * q * 608.0f;
                const float fog = std::min(std::max(vu.f(4, 3) + vu.f(21, 0) + vu.f(21, 3) * q, 0.0f), vu.f(21, 1)) / 255.0f;
                ps2x::native::Vertex a, b;
                a.x = std::min(std::max(x + r, 0.0f), 4095.0f);
                b.x = std::min(std::max(x - r, 0.0f), 4095.0f);
                a.y = b.y = y;
                a.z = b.z = z;
                a.fog = b.fog = fog;
                uvAt(top + 3u + 6u * k, a);
                uvAt(top + 6u + 6u * k, b);
                a.rgba = colourAt(k == 0u ? top + 2u : top + 6u * k - 1u);
                b.rgba = colourAt(top + 6u * k + (k == 0u ? 5u : 2u));
                strip.push_back(a);
                strip.push_back(b);
                adc.push_back(clipped || parity != 0u);
                adc.push_back(clipped || parity != 0u);
            }
            for (size_t i = 2; i < strip.size(); ++i)
            {
                if (adc[i])
                    continue;
                triangles.push_back(strip[i - 2]);
                triangles.push_back(strip[i - 1]);
                triangles.push_back(strip[i]);
            }
        }

        // A reflective strip (0x1db8, its MSCNTs from 0x1ec8, the water's from 0x2248): the strips' layout (vertex k at
        // TOP + 1 + 3k: normal, pool index in w; colour; texture coordinates, rewritten here), positions from the pool.
        // The first MSCNT after the vertex pass only reads the alpha terms (parameter step). Then for each vertex:
        // the eye vector e = (p + R^T t) / w (VU 0-2 the object's rotation, VU 3 its translation, w the screen matrix's,
        // which the vertex pass leaves in the pool's w as 1 / w) is reflected about the
        // normal n = 1.00787 - b / 127 (the water's first bent by a wave: n.xy += 0.125 (s - s^3) - 0.03125, s = 2 frac(VU
        // 21.z + p.xy (0.019, 0.021)) - 1): r = e - 2 (e.n) n. The texture coordinates are 0.5 + 0.414 r taken to the
        // world (VU 0-2) along x and z (the water's x and y), Q 1. Alpha times max(base + slope (e.n), floor) (the water's
        // 1, 2, 0.1875), then the colour times VU 5 plus VU 20. VU1 also cuts triangles that cross the clip volume
        // (0x26e0); the GPU clips them here.
        void reflectiveStrip(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            const VuImage &vu = s.vu;
            const uint32_t top = s.top;
            if (s.reflectParams)
            {
                // The MSCNT after the vertex pass: the alpha terms from its TOP (.yzw), all three 0 for the water.
                s.reflectParams = false;
                for (int k = 0; k < 3; ++k)
                    s.reflect[k] = vu.f(top, 1 + k);
                s.water = s.reflect[0] + s.reflect[1] + s.reflect[2] == 0.0f; // FSAND 0x001 after their sum (0x1e58)
                return;
            }
            ++g_stats.strips;
            if (!useMaterial(s, top, vu.w(top, 3), triangles))
                return;
            const uint32_t nloop = vu.w(top, 0) & 0x7fffu;
            float eyeOffset[3];
            for (int j = 0; j < 3; ++j)
                eyeOffset[j] = vu.f(j, 0) * vu.f(3, 0) + vu.f(j, 1) * vu.f(3, 1) + vu.f(j, 2) * vu.f(3, 2);
            const int vAxis = s.water ? 1 : 2;
            const float base = s.water ? 1.0f : s.reflect[0], slope = s.water ? 2.0f : s.reflect[1], floor = s.water ? 0.1875f : s.reflect[2];
            std::vector<ps2x::native::Vertex> strip;
            strip.reserve(nloop);
            for (uint32_t v = 0; v < nloop; ++v)
            {
                const uint32_t q = top + 1u + 3u * v;
                const uint32_t index = vu.w(q, 3) & 0x7fffu;
                ps2x::native::Vertex out;
                if (static_cast<size_t>(index) * 3u + 2u >= s.pool.size())
                {
                    strip.push_back(out);
                    strip.back().rgba = 0xdeadu;
                    continue;
                }
                const float *pos = &s.pool[index * 3u];
                out.x = pos[0];
                out.y = pos[1];
                out.z = pos[2];
                // The vertex pass left 1 / w (the screen matrix's) in the pool's w (0xf18): e is scaled by it.
                const float w = s.matrix[3] * pos[0] + s.matrix[7] * pos[1] + s.matrix[11] * pos[2] + s.matrix[15];
                const float depthQ = w != 0.0f ? 1.0f / w : 0.0f;
                float n[3], e[3];
                for (int k = 0; k < 3; ++k)
                {
                    n[k] = 1.00787f - static_cast<float>(static_cast<int32_t>(vu.w(q, k))) * 0.00787402f;
                    e[k] = (pos[k] + eyeOffset[k]) * depthQ;
                }
                if (s.water)
                {
                    const float scale[2] = {0.019f, 0.021f};
                    for (int k = 0; k < 2; ++k)
                    {
                        const float x = vu.f(21, 2) + pos[k] * scale[k];
                        const float wave = 2.0f * (x - 0.5f) - 2.0f * std::trunc(x);
                        n[k] += 0.125f * (wave - wave * wave * wave) - 0.03125f;
                    }
                }
                const float d = e[0] * n[0] + e[1] * n[1] + e[2] * n[2];
                float r[3];
                for (int k = 0; k < 3; ++k)
                    r[k] = e[k] - 2.0f * d * n[k];
                out.u = 0.5f + 0.414062f * (vu.f(0, 0) * r[0] + vu.f(1, 0) * r[1] + vu.f(2, 0) * r[2]);
                out.v = 0.5f + 0.414062f * (vu.f(0, vAxis) * r[0] + vu.f(1, vAxis) * r[1] + vu.f(2, vAxis) * r[2]);
                const float fade = std::max(base + slope * d, floor);
                float c[4];
                for (int k = 0; k < 4; ++k)
                {
                    float value = static_cast<float>(static_cast<int32_t>(vu.w(q + 1, k)));
                    if (k == 3)
                        value *= fade;
                    c[k] = std::min(value * vu.f(5, k) + vu.f(20, k), 255.0f);
                }
                out.rgba = clampByte(c[0]) | (clampByte(c[1]) << 8) | (clampByte(c[2]) << 16) | (clampByte(c[3]) << 24);
                // The setup put VU 21.z where the vertex pass's fog takes VU 21.x (ADDz.x vf18 at 0x1dc8).
                out.fog = w > 0.0f ? std::min(std::max(vu.f(4, 3) + vu.f(21, 2) + vu.f(21, 3) / w, 0.0f), vu.f(21, 1)) / 255.0f : 0.0f;
                strip.push_back(out);
            }
            for (size_t i = 2; i < strip.size(); ++i)
            {
                if (strip[i].rgba == 0xdeadu || strip[i - 1].rgba == 0xdeadu || strip[i - 2].rgba == 0xdeadu)
                    continue;
                triangles.push_back(strip[i - 2]);
                triangles.push_back(strip[i - 1]);
                triangles.push_back(strip[i]);
            }
        }

        // The effects image's particles (0x390; checked against 51,643 particles of PS2X_VU1_RECORD runs: counts,
        // culling, colours, UVs and fog exact, corners within VU rounding). Its 0x0 left VU 4-7 in vf29, vf27, vf28,
        // vf31. The MSCAL: an A+D block (TEX0) at TOP, the sprite tag at TOP + 2 (its x the count), the positions read
        // downwards from (TOP.w & 0x7fff) + 7 through VU 0-3 (times the batch's matrix at TOP + 5, + 10, + 15, + 20 when
        // TOP.w is negative); particle k's colour at TOP + 3 + 5k and its UV row's index at TOP + 4 + 5k (VU 296 + i:
        // U1 V1 U2 V2, in texels x 16). Its MSCNTs (0x588, 0x748): the tag at TOP, colours from TOP + 1, the positions
        // where the last batch stopped. Each particle: c = M p, q = 1 / c.w, half size VU 5.xyz p.w q (x, y at most
        // VU 5.xy / 2), corners c q -+ half; dropped when a corner leaves VU 6.w's guard band (offset by VU 6.xy) or
        // VU 6.z + c.w < -VU 6.w (behind the camera). Colour + VU 4.rgb, at most 255; fog VU 4.w + VU 5.w q (at most VU 7.w); the second
        // variant (TOP.z & 0x8000) takes that over VU 7.w (0..1) as the alpha's factor instead, without the size limit.
        // The lens flares (the effects image's 0x2338; checked against 2,866 flare elements of PS2X_VU1_RECORD runs:
        // counts, colours, UVs and corners exact). VU 44: the screen centre (xy), the groups (z) and the elements (w);
        // element j at 45 + 3j (its light on screen), 46 + 3j (flags in x, then three terms) and 47 + 3j (colour, and
        // the light's visibility in w); group g at 8 + g (TEX0 in xy, the step along the line in z, the size in w), 26 +
        // g (colour, and a size factor in w), its UV extent at 272 + (8 + g).w & 3. First each element's scale and
        // visibility (flags 1: drawn; 16: |z|; 8: scaled by the third term, hidden when that leaves the visibility <= 0;
        // the second divide takes Q before VU1's new one lands, so twice the first), then each group: one sprite per
        // drawn element, on the line from the centre to the light, the first group sized by the element's scale (alpha
        // 192), the others fixed (alpha 128, colours times the visibility, at most 32); an element whose colour comes
        // out under 1 on all three is left out of the groups after. The packet's own A+D sets ALPHA (0x48: added) and
        // TEX0, from the two output buffers (0x110, 0x288) the groups alternate between; Z is the farthest, on top.
        void lensFlares(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            VuImage &vu = s.vu;
            const float centre[2] = {vu.f(44, 0), vu.f(44, 1)};
            const uint32_t groups = vu.w(44, 2) & 0xffffu, items = vu.w(44, 3) & 0xffffu;
            std::vector<uint8_t> drawn(items);
            std::vector<float> scale(items), seen(items);
            for (uint32_t j = 0; j < items; ++j)
            {
                const uint32_t flags = vu.w(46 + 3 * j, 0);
                const float a = vu.f(46 + 3 * j, 1), b = vu.f(46 + 3 * j, 2), c = vu.f(46 + 3 * j, 3);
                float visibility = vu.f(47 + 3 * j, 3), size = 0.0f;
                bool show = (flags & 1u) != 0u;
                if (show)
                {
                    const float root = std::sqrt(std::fabs(visibility));
                    const float z = ((flags & 16u) ? std::fabs(b) : std::max(b, 0.0f)) + (c - 1.0f);
                    visibility = visibility / a / a;
                    size = root / a;
                    if (flags & 8u)
                    {
                        size = size * z / c;
                        const float scaled = visibility * z;
                        visibility = scaled / c;
                        show = scaled > 0.0f;
                    }
                }
                drawn[j] = show ? 1u : 0u;
                scale[j] = size;
                seen[j] = visibility;
            }
            for (uint32_t g = 0; g < groups; ++g)
            {
                const uint32_t buffer = (g & 1u) ? 0x288u : 0x110u;
                // VU1 writes the group's TEX0 into its buffer's A+D block (SQ.xy at 0x2508): the copy here does too.
                vu.words[(buffer + 2u) * 4u + 0u] = vu.w(8 + g, 0);
                vu.words[(buffer + 2u) * 4u + 1u] = vu.w(8 + g, 1);
                if (!useMaterial(s, buffer + 3u, 0u, triangles, 0x53531u, buffer + 1u, vu.w(buffer, 0) & 0x7fffu))
                {
                    static uint32_t s_logged = 0;
                    if (s_logged++ < 20u)
                        std::fprintf(stderr, "[tmb:native] flares: buffer %#x tag %08x %08x %08x %08x, A+D tag %08x %08x %08x %08x\n", buffer,
                                     vu.w(buffer + 3u, 0), vu.w(buffer + 3u, 1), vu.w(buffer + 3u, 2), vu.w(buffer + 3u, 3), vu.w(buffer, 0),
                                     vu.w(buffer, 1), vu.w(buffer, 2), vu.w(buffer, 3));
                    continue;
                }
                const uint32_t width = 1u << ((s.tex0 >> 26) & 0xfu), height = 1u << ((s.tex0 >> 30) & 0xfu);
                const float step = vu.f(8 + g, 2), sizeX = vu.f(8 + g, 3), sizeY = vu.f(8 + g, 3) * 0.5f;
                const float colour[4] = {vu.f(26 + g, 0), vu.f(26 + g, 1), vu.f(26 + g, 2), vu.f(26 + g, 3)};
                const float extent = static_cast<float>(vu.w(272u + (vu.w(8 + g, 3) & 3u), 3)) / 16.0f;
                for (uint32_t j = 0; j < items; ++j)
                {
                    if (!drawn[j])
                        continue;
                    const float w = scale[j] * colour[3], weight = colour[3] * seen[j];
                    float rgb[3];
                    for (int i = 0; i < 3; ++i)
                        rgb[i] = colour[i] * vu.f(47 + 3 * j, i) * (g == 0u ? 1.0f : weight);
                    const float x = centre[0] + (vu.f(45 + 3 * j, 0) - centre[0]) * step;
                    const float y = centre[1] + (vu.f(45 + 3 * j, 1) - centre[1]) * step;
                    const float dx = g == 0u ? sizeX * w : sizeX, dy = g == 0u ? sizeY * w : sizeY;
                    const float limit = g == 0u ? 255.0f : 32.0f;
                    const uint32_t alpha = g == 0u ? 192u : 128u;
                    uint32_t rgba = alpha << 24;
                    for (int i = 0; i < 3; ++i)
                        rgba |= clampByte(std::min(rgb[i], limit)) << (8 * i);
                    if (rgb[0] < 1.0f && rgb[1] < 1.0f && rgb[2] < 1.0f)
                        drawn[j] = 0u; // SUBw.xyz vf0, vf22, vf0w: left out of the next groups
                    const float xs[4] = {x + dx, x - dx, x + dx, x - dx}, ys[4] = {y + dy, y + dy, y - dy, y - dy};
                    const float us[4] = {0.5f, extent, 0.5f, extent}, vs[4] = {0.5f, 0.5f, extent, extent};
                    ps2x::native::Vertex corner[4];
                    for (int i = 0; i < 4; ++i)
                    {
                        corner[i].x = xs[i];
                        corner[i].y = ys[i];
                        corner[i].z = 16777215.0f; // XYZ2: FTOI4 of 1048575.94, Z 0xffffff (the farthest)
                        corner[i].u = us[i] / width;
                        corner[i].v = vs[i] / height;
                        corner[i].rgba = rgba;
                        corner[i].fog = 0.0f;
                    }
                    const int order[6] = {0, 1, 2, 1, 2, 3}; // as GifToNative splits a sprite (the check compares in order)
                    for (int i : order)
                        triangles.push_back(corner[i]);
                }
            }
        }

        // The effects image's streaks (0x118; checked against the lines of PS2X_VU1_RECORD runs: counts, positions and
        // alpha exact, colours within one step of VU1's rounding). Its input at 0x218 (not TOP): x the palette's scale,
        // the tag's PRIM and registers (a line, Gouraud, RGBAQ XYZ2 twice) in y and z, the count in w (negative: times
        // the matrix at 0x21a-0x21d). Line k runs from VU 8 + k to it minus twice VU 264 + k, through VU 0-3 with its
        // z row over 16; its colour between palette entries VU 520 + i and 521 + i (i and the step: VU (264 + k).w times
        // 0x218.x), its alpha times (VU 4.w + VU 5.w q) (8 + k).w / 255 (q the start's 1 / w, at most 255), the far
        // end's alpha 0. Dropped when either end leaves VU 6.w's guard band (offset by VU 6.xy), lies behind the camera
        // (VU 6.z + w < -VU 6.w) or the alpha is under 1. Drawn as GifToNative draws a line: a quad one pixel wide.
        void streaks(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            if (!useMaterial(s, 0x218u, 0u, triangles, 0x5151u))
                return;
            int32_t count = static_cast<int32_t>(vu.w(0x218u, 3));
            float object[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
            if (count < 0)
            {
                for (int j = 0; j < 4; ++j)
                    for (int k = 0; k < 4; ++k)
                        object[j][k] = vu.f(0x21au + j, k);
                count &= 0x7fff;
            }
            float m[16];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    m[j * 4 + k] = (vu.f(0, k) * object[j][0] + vu.f(1, k) * object[j][1] + vu.f(2, k) * object[j][2] + vu.f(3, k) * object[j][3]) *
                                   (k == 2 ? 0.0625f : 1.0f);
            const float(&c4)[4] = s.fxConst[0];
            const float(&c5)[4] = s.fxConst[1];
            const float(&c6)[4] = s.fxConst[2];
            const float guard = std::fabs(c6[3]), scale = vu.f(0x218u, 0);
            for (int32_t k = 0; k < count; ++k)
            {
                const uint32_t at = 8u + static_cast<uint32_t>(k);
                const float start[4] = {vu.f(at, 0), vu.f(at, 1), vu.f(at, 2), vu.f(at, 3)};
                const float end[3] = {start[0] - 2.0f * vu.f(264u + k, 0), start[1] - 2.0f * vu.f(264u + k, 1), start[2] - 2.0f * vu.f(264u + k, 2)};
                float a[4], b[4];
                for (int i = 0; i < 4; ++i)
                {
                    a[i] = m[i] * start[0] + m[4 + i] * start[1] + m[8 + i] * start[2] + m[12 + i];
                    b[i] = m[i] * end[0] + m[4 + i] * end[1] + m[8 + i] * end[2] + m[12 + i];
                }
                const float qa = 1.0f / a[3], qb = 1.0f / b[3];
                const float t = vu.f(264u + k, 3) * scale;
                const int32_t index = static_cast<int32_t>(t);
                const float step = t - static_cast<float>(index);
                const uint32_t low = (520u + static_cast<uint32_t>(index)) & 0x3ffu, high = (521u + static_cast<uint32_t>(index)) & 0x3ffu;
                float colour[4];
                for (int i = 0; i < 4; ++i)
                    colour[i] = vu.f(low, i) - vu.f(low, i) * step + vu.f(high, i) * step;
                colour[3] = std::min(colour[3] * (c4[3] / 255.0f * start[3] + c5[3] / 255.0f * start[3] * qa), 255.0f);
                const float ax = a[0] * qa, ay = a[1] * qa, bx = b[0] * qb, by = b[1] * qb;
                if (std::fabs(ax + c6[0]) > guard || std::fabs(ay + c6[1]) > guard || c6[2] + a[3] < -guard ||
                    std::fabs(bx + c6[0]) > guard || std::fabs(by + c6[1]) > guard || c6[2] + b[3] < -guard || colour[3] < 1.0f)
                    continue;
                uint32_t nearColour = 0;
                for (int i = 0; i < 4; ++i)
                    nearColour |= (static_cast<uint32_t>(static_cast<int32_t>(colour[i])) & 0xffu) << (8 * i);
                const uint32_t farColour = nearColour & 0x00ffffffu;
                const float dx = bx - ax, dy = by - ay, length = std::sqrt(dx * dx + dy * dy);
                const float nx = length > 0.0f ? -dy / length * 0.5f : 0.5f, ny = length > 0.0f ? dx / length * 0.5f : 0.0f;
                ps2x::native::Vertex corner[4];
                const float xs[4] = {ax + nx, ax - nx, bx + nx, bx - nx}, ys[4] = {ay + ny, ay - ny, by + ny, by - ny};
                for (int i = 0; i < 4; ++i)
                {
                    corner[i].x = xs[i];
                    corner[i].y = ys[i];
                    corner[i].z = (i < 2 ? a[2] * qa : b[2] * qb) * 16.0f; // XYZ2: FTOI4's value whole is the GS Z
                    corner[i].rgba = i < 2 ? nearColour : farColour;
                }
                const int order[6] = {0, 1, 2, 1, 2, 3}; // as GifToNative splits a line (the check compares in order)
                for (int i : order)
                    triangles.push_back(corner[i]);
            }
        }

        // VU1's random numbers (RINIT, RNEXT): a 23-bit shift register under the exponent of 1.0.
        uint32_t vuRandInit(float seed)
        {
            uint32_t value;
            std::memcpy(&value, &seed, 4);
            return 0x3f800000u | (value & 0x7fffffu);
        }

        uint32_t vuRandNext(uint32_t r)
        {
            const uint32_t x = (r >> 4) & 1u, y = (r >> 22) & 1u;
            return (((r << 1) ^ x ^ y) & 0x7fffffu) | 0x3f800000u;
        }

        float vuRandFloat(uint32_t r)
        {
            float value;
            std::memcpy(&value, &r, 4);
            return value;
        }

        // The effects image's trails (0x1830; checked against 3,469 sprites of PS2X_VU1_RECORD runs: counts, corners,
        // colours, UVs and the random choices exact). A curve through control points read downwards from VU 8 +
        // (TOP + 6).z (negative: the batch's matrix at TOP + 7 too), its first point mirrored about TOP + 5, its last
        // extended; each segment (TOP + 2.y sprites) evaluated by forward differences (TOP + 3: the velocity's weights,
        // TOP + 4 the acceleration's, reversed after TOP + 2.z sprites, blended by 0.5, or 0.125 when TOP + 2.y is odd).
        // Each sprite: half size VU 5.xyz (TOP + 2.x + TOP + 1.y t) times a random factor (TOP + 1.w), t counting from
        // TOP + 6.y; colour between palette rows 264 + i and 265 + i (i: TOP + 1.x t (1 + 2a - a r), at most 31, a = TOP.x,
        // multiplied once more at a segment's start after the first of a batch), plus VU 4; UVs from row 296 + (r & 3);
        // fog and culling as the particles (the second corner's test on VU 6.z + fog). Batches of TOP + 2.w sprites (whole
        // segments), TOP + 6.x in all. The random numbers are seeded from each segment's first point (its w).
        void trails(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            const uint32_t top = s.top;
            if (!useMaterial(s, top, 0u, triangles, 0x43431u))
                return;
            const auto row = [&](uint32_t at, float out[4])
            {
                for (int i = 0; i < 4; ++i)
                    out[i] = vu.f(at & 0x3ffu, i);
            };
            int32_t list = static_cast<int32_t>(vu.w(top + 6u, 2));
            float object[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
            if (list < 0)
            {
                for (int j = 0; j < 4; ++j)
                    row(top + 7u + j, object[j]);
                list &= 0x7fff;
            }
            float m[16];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    m[j * 4 + k] = vu.f(0, k) * object[j][0] + vu.f(1, k) * object[j][1] + vu.f(2, k) * object[j][2] + vu.f(3, k) * object[j][3];
            const float(&c4)[4] = s.fxConst[0];
            const float(&c5)[4] = s.fxConst[1];
            const float(&c6)[4] = s.fxConst[2];
            const float(&c7)[4] = s.fxConst[3];
            float t1[4], t3[4], t4[4], t5[4];
            row(top + 1u, t1);
            row(top + 3u, t3);
            row(top + 4u, t4);
            row(top + 5u, t5);
            const float a = vu.f(top, 0), size0 = vu.f(top + 2u, 0);
            const uint32_t perSegment = vu.w(top + 2u, 1) & 0xffffu, switchAt = vu.w(top + 2u, 2) & 0xffffu;
            const uint32_t perBatch = vu.w(top + 2u, 3) & 0xffffu;
            int32_t total = static_cast<int32_t>(vu.w(top + 6u, 0) & 0xffffu);
            const float h = (vu.w(top + 2u, 1) & 1u) ? 0.125f : 0.5f, y13 = 1.0f + 2.0f * a;
            float t = vu.f(top + 6u, 1);
            const uint32_t width = 1u << ((s.tex0 >> 26) & 0xfu), height = 1u << ((s.tex0 >> 30) & 0xfu);
            uint32_t at = static_cast<uint32_t>(list);
            float p5[4], p6[4], p7[4], p8[4];
            row(8u + at, p6);
            row(7u + at, p7);
            row(6u + at, p8);
            --at;
            const float d = 2.0f * (t5[0] * (p7[0] - p6[0]) + t5[1] * (p7[1] - p6[1]) + t5[2] * (p7[2] - p6[2]));
            for (int i = 0; i < 3; ++i)
                p5[i] = p7[i] - t5[i] * d;
            p5[3] = 0.0f;
            uint32_t random = vuRandNext(vuRandInit(p6[3]));
            float rnd = vuRandFloat(random);
            if (at == 0u)
                for (int i = 0; i < 3; ++i)
                    p8[i] = 2.0f * p7[i] - p6[i];
            const float guard = std::fabs(c6[3]);
            while (total > 0)
            {
                int32_t left = static_cast<int32_t>(std::min<uint32_t>(static_cast<uint32_t>(total), perBatch));
                float pos[3] = {p6[0], p6[1], p6[2]};
                bool firstSegment = true;
                while (true)
                {
                    float vel[3], acc[3], late[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        vel[i] = p5[i] * t3[0] + p6[i] * t3[1] + p7[i] * t3[2] + p8[i] * t3[3];
                        acc[i] = p5[i] * t4[0] + p6[i] * t4[1] + p7[i] * t4[2] + p8[i] * t4[3];
                        late[i] = p5[i] * t4[3] + p6[i] * t4[2] + p7[i] * t4[1] + p8[i] * t4[0];
                    }
                    random = vuRandInit(p6[3]);
                    --at;
                    const bool switches = perSegment != 2u;
                    const uint32_t mid = switches ? switchAt : 2u * perSegment;
                    std::memcpy(p5, p6, sizeof(p5));
                    std::memcpy(p6, p7, sizeof(p6));
                    std::memcpy(p7, p8, sizeof(p7));
                    if (at != 0u)
                        row(7u + at, p8);
                    else
                        for (int i = 0; i < 3; ++i)
                            p8[i] = 2.0f * p7[i] - p6[i];
                    for (uint32_t k = perSegment; k > 0; --k)
                    {
                        const float sizeFactor = 1.0f - t1[3] + rnd * t1[3];
                        float pal = std::min(t1[0] * t * (y13 - a * rnd), 31.0f);
                        if (k == perSegment && !firstSegment)
                            pal = std::min(pal * (y13 - a * rnd), 31.0f); // 0x19c0 multiplies the carried value again
                        const int32_t index = static_cast<int32_t>(pal);
                        const float step = pal - static_cast<float>(index);
                        const float size = (t1[1] * t + size0) * sizeFactor;
                        t += 1.0f;
                        random = vuRandNext(random);
                        rnd = vuRandFloat(random);
                        float c[4];
                        for (int i = 0; i < 4; ++i)
                            c[i] = m[i] * pos[0] + m[4 + i] * pos[1] + m[8 + i] * pos[2] + m[12 + i];
                        const float q = 1.0f / c[3];
                        for (int i = 0; i < 3; ++i)
                        {
                            pos[i] += vel[i];
                            vel[i] += acc[i];
                        }
                        if (switches && k - 1u == mid)
                            for (int i = 0; i < 3; ++i)
                            {
                                vel[i] = vel[i] - acc[i] * h + late[i] * h;
                                acc[i] = late[i];
                            }
                        const float x0 = c[0] * q - c5[0] * size * q, y0 = c[1] * q - c5[1] * size * q;
                        const float x1 = c[0] * q + c5[0] * size * q, y1 = c[1] * q + c5[1] * size * q;
                        const float z1 = c[2] * q + c5[2] * size * q;
                        const float fog = std::min(c4[3] + c5[3] * q, c7[3]);
                        --left;
                        --total;
                        if (std::fabs(x0 + c6[0]) > guard || std::fabs(y0 + c6[1]) > guard || c6[2] + c[3] < -guard ||
                            std::fabs(x1 + c6[0]) > guard || std::fabs(y1 + c6[1]) > guard || c6[2] + fog < -guard)
                            continue;
                        uint32_t rgba = 0;
                        const uint32_t low = (264u + static_cast<uint32_t>(index)) & 0x3ffu, high = (265u + static_cast<uint32_t>(index)) & 0x3ffu;
                        for (int i = 0; i < 4; ++i)
                        {
                            const float lo = static_cast<float>(static_cast<int32_t>(vu.w(low, i)));
                            const float hi = static_cast<float>(static_cast<int32_t>(vu.w(high, i)));
                            const float bias = (i < 3 ? c4[i] : 0.0f) + 8388608.0f; // vf30: VU 4.rgb + 2^23
                            const float value = std::min((lo - lo * step + hi * step) + bias, 8388863.0f);
                            rgba |= (static_cast<uint32_t>(static_cast<int32_t>(value - 8388608.0f)) & 0xffu) << (8 * i);
                        }
                        const uint32_t uvRow = 296u + (random & 3u);
                        const float u0 = static_cast<float>(vu.w(uvRow, 0)) / 16.0f / width, v0 = static_cast<float>(vu.w(uvRow, 1)) / 16.0f / height;
                        const float u1 = static_cast<float>(vu.w(uvRow, 2)) / 16.0f / width, v1 = static_cast<float>(vu.w(uvRow, 3)) / 16.0f / height;
                        ps2x::native::Vertex corner[4];
                        const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1}, us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
                        for (int i = 0; i < 4; ++i)
                        {
                            corner[i].x = xs[i];
                            corner[i].y = ys[i];
                            corner[i].z = z1;
                            corner[i].u = us[i];
                            corner[i].v = vs[i];
                            corner[i].rgba = rgba;
                            corner[i].fog = std::floor(std::max(fog, 0.0f)) / 255.0f;
                        }
                        const int order[6] = {0, 1, 2, 1, 2, 3};
                        for (int i : order)
                            triangles.push_back(corner[i]);
                    }
                    std::memcpy(pos, p6, sizeof(pos));
                    firstSegment = false;
                    if (left == 0 || total <= 0) // VU1 ends a batch at exactly 0 (whole segments); the total bounds it here
                        break;
                }
            }
        }

        // The effects image's textured particles (0x798, its MSCNTs at 0x9b8; checked against 63,644 sprites of
        // PS2X_VU1_RECORD runs: counts, TEX0, colours, UVs, corners and fog exact, Z within float rounding). As 0x390,
        // but each particle brings its own TEX0 (VU 264 + i, i at TOP + 1 + 6k) and 6 quadwords (TEX0, colour at TOP + 2 +
        // 6k, the UV row's index at TOP + 3 + 6k), the batch's matrix at TOP + 4 + 6j, and Z set from the distance alone:
        // -1024 + 16778240 / (w - (TOP + 4).x).
        void texParticles(State &s, std::vector<ps2x::native::Vertex> &triangles, bool first)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            const uint32_t top = s.top;
            if (first)
            {
                const int32_t list = static_cast<int32_t>(vu.w(top, 3));
                float object[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
                if (list < 0)
                    for (int j = 0; j < 4; ++j)
                        for (int k = 0; k < 4; ++k)
                            object[j][k] = vu.f(top + 4u + 6u * j, k);
                for (int j = 0; j < 4; ++j)
                    for (int k = 0; k < 4; ++k)
                        s.fxMatrix[j * 4 + k] = vu.f(0, k) * object[j][0] + vu.f(1, k) * object[j][1] + vu.f(2, k) * object[j][2] +
                                                vu.f(3, k) * object[j][3];
                s.fxNext = (static_cast<uint32_t>(list) & 0x7fffu) + 7u;
                s.fxBias = vu.f(top + 4u, 0);
            }
            if (!useMaterial(s, top, 0u, triangles, 0x434316u))
                return;
            const float(&c4)[4] = s.fxConst[0];
            const float(&c5)[4] = s.fxConst[1];
            const float(&c6)[4] = s.fxConst[2];
            const float(&c7)[4] = s.fxConst[3];
            const uint32_t count = vu.w(top, 0) & 0xffffu;
            const float guard = std::fabs(c6[3]);
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t at = s.fxNext-- & 0x3ffu;
                const float p[4] = {vu.f(at, 0), vu.f(at, 1), vu.f(at, 2), vu.f(at, 3)};
                float c[4];
                for (int i = 0; i < 4; ++i)
                    c[i] = s.fxMatrix[i] * p[0] + s.fxMatrix[4 + i] * p[1] + s.fxMatrix[8 + i] * p[2] + s.fxMatrix[12 + i];
                const float q = 1.0f / c[3];
                const float hx = std::min(c5[0] * p[3] * q, c5[0] * 0.5f), hy = std::min(c5[1] * p[3] * q, c5[1] * 0.5f);
                const float fog = std::min(c4[3] + c5[3] * q, c7[3]);
                const float z = -1024.0f + 16778240.0f / (c[3] - s.fxBias);
                const float x0 = c[0] * q - hx, y0 = c[1] * q - hy, x1 = c[0] * q + hx, y1 = c[1] * q + hy;
                if (std::fabs(x0 + c6[0]) > guard || std::fabs(y0 + c6[1]) > guard || c6[2] + c[3] < -guard ||
                    std::fabs(x1 + c6[0]) > guard || std::fabs(y1 + c6[1]) > guard || c6[2] + fog < -guard)
                    continue;
                const uint32_t base = top + 1u + 6u * k;
                const uint32_t texRow = (264u + (vu.w(base, 0) & 0xffffu)) & 0x3ffu;
                const uint64_t tex0 = static_cast<uint64_t>(vu.w(texRow, 0)) | (static_cast<uint64_t>(vu.w(texRow, 1)) << 32);
                if (tex0 != s.tex0 || !s.haveTexture)
                {
                    flush(s, triangles); // the sprite's own TEX0 (written per sprite in the packet)
                    s.tex0 = s.lastTex0 = tex0;
                    s.haveTexture = true;
                }
                const uint32_t width = 1u << ((tex0 >> 26) & 0xfu), height = 1u << ((tex0 >> 30) & 0xfu);
                uint32_t rgba = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const float value = static_cast<float>(static_cast<int32_t>(vu.w(base + 1u, i))) + (i < 3 ? c4[i] : 0.0f);
                    rgba |= std::min(static_cast<uint32_t>(std::max(value, 0.0f)), 255u) << (8 * i);
                }
                const uint32_t uvRow = (296u + (vu.w(base + 2u, 0) & 0xffffu)) & 0x3ffu;
                const float u0 = static_cast<float>(vu.w(uvRow, 0)) / 16.0f / width, v0 = static_cast<float>(vu.w(uvRow, 1)) / 16.0f / height;
                const float u1 = static_cast<float>(vu.w(uvRow, 2)) / 16.0f / width, v1 = static_cast<float>(vu.w(uvRow, 3)) / 16.0f / height;
                ps2x::native::Vertex corner[4];
                const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1}, us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
                for (int i = 0; i < 4; ++i)
                {
                    corner[i].x = xs[i];
                    corner[i].y = ys[i];
                    corner[i].z = z;
                    corner[i].u = us[i];
                    corner[i].v = vs[i];
                    corner[i].rgba = rgba;
                    corner[i].fog = std::floor(std::max(fog, 0.0f)) / 255.0f;
                }
                const int order[6] = {0, 1, 2, 1, 2, 3};
                for (int i : order)
                    triangles.push_back(corner[i]);
            }
        }

        // The effects image's heat haze (0xa08; checked against the fans of PS2X_VU1_RECORD runs: counts, corners and
        // alpha exact, the UVs as VU1's but clamped at 0). Each point read downwards from VU 7 + VU 268.x (negative: the
        // batch's matrix at TOP - TOP + 3) is drawn as a fan of four triangles around it, textured with the frame itself
        // (TEX0 the frame buffer, UVs the vertices' own screen positions less the origin VU1 takes: 1728, 2^19 - VU
        // 268.y): the centre's UV moved by a random step (VU 5.xy VU 267.y (VU 267.z + r), r from the point's w) times
        // q and a factor VU 268.w that grows by VU 268.z from point to point, which bends what is behind. Half size VU 5.xy
        // VU 267.y q; centre colour VU 266 with alpha min(fog VU 267.w, 128) times the factor, the corners VU 269; Z the
        // centre's, whole. PRIM from VU 265 (a fan, blended). Dropped as the particles are.
        void heatHaze(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            const uint32_t top = s.top;
            s.prim = vu.w(265, 0) & 0x7ffu; // the packet's PRIM, written per fan
            // The tag VU1 kicks is VU 264's (SQ.yzw vf20 to TOP at 0xae8; TOP itself holds an A+D tag till then).
            if (!useMaterial(s, 264u, 0u, triangles, 0x35315310u))
                return;
            if (!s.sampleTarget)
            {
                flush(s, triangles);
                s.sampleTarget = true;
            }
            int32_t count = static_cast<int32_t>(vu.w(268, 0));
            float object[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
            if (count < 0)
            {
                for (int j = 0; j < 4; ++j)
                    for (int k = 0; k < 4; ++k)
                        object[j][k] = vu.f(top + j, k);
                count &= 0x7fff;
            }
            float m[16];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    m[j * 4 + k] = vu.f(0, k) * object[j][0] + vu.f(1, k) * object[j][1] + vu.f(2, k) * object[j][2] + vu.f(3, k) * object[j][3];
            const float(&c4)[4] = s.fxConst[0];
            const float(&c5)[4] = s.fxConst[1];
            const float(&c6)[4] = s.fxConst[2];
            const float sizeX = c5[0] * vu.f(267, 1), sizeY = c5[1] * vu.f(267, 1);
            const float originX = 524288.0f - 522560.0f, originY = 524288.0f - vu.f(268, 1);
            float factor = vu.f(268, 3);
            uint32_t at = static_cast<uint32_t>(count) + 7u;
            const auto jitter = [&](float seed, float out[2])
            {
                uint32_t r = vuRandNext(vuRandInit(seed));
                const float rx = vuRandFloat(r);
                r = vuRandNext(r);
                const float ry = vuRandFloat(r);
                out[0] = sizeX * vu.f(267, 2) + sizeX * rx;
                out[1] = sizeY * vu.f(267, 2) + sizeY * ry;
            };
            float step[2];
            jitter(vu.f(at & 0x3ffu, 3), step);
            uint32_t centreColour = 0, cornerColour = 0;
            for (int i = 0; i < 3; ++i)
            {
                centreColour |= (vu.w(266, i) & 0xffu) << (8 * i);
                cornerColour |= (vu.w(269, i) & 0xffu) << (8 * i);
            }
            cornerColour |= (vu.w(269, 3) & 0xffu) << 24;
            const float guard = std::fabs(c6[3]);
            for (int32_t k = 0; k < count; ++k)
            {
                const uint32_t here = at-- & 0x3ffu;
                const float p[3] = {vu.f(here, 0), vu.f(here, 1), vu.f(here, 2)};
                float c[4];
                for (int i = 0; i < 4; ++i)
                    c[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
                const float q = 1.0f / c[3];
                const float cx = c[0] * q, cy = c[1] * q, z = std::trunc(c[2] * q); // XYZ2's Z: FTOI0, whole
                const float fog = c4[3] + c5[3] * q;
                const float x0 = cx - sizeX * q, y0 = cy - sizeY * q, x1 = cx + sizeX * q, y1 = cy + sizeY * q;
                const float alpha = std::min(fog * vu.f(267, 3), 128.0f) * factor;
                const float du = step[0] * q * factor, dv = step[1] * q * factor;
                factor += vu.f(268, 2);
                jitter(vu.f(at & 0x3ffu, 3), step);
                if (std::fabs(x0 + c6[0]) > guard || std::fabs(y0 + c6[1]) > guard || c6[2] + c[3] < -guard ||
                    std::fabs(x1 + c6[0]) > guard || std::fabs(y1 + c6[1]) > guard || c6[2] + fog < -guard)
                    continue;
                ps2x::native::Vertex v[5];
                const float xs[5] = {cx, x0, x1, x1, x0}, ys[5] = {cy, y0, y0, y1, y1};
                for (int i = 0; i < 5; ++i)
                {
                    v[i].x = xs[i];
                    v[i].y = ys[i];
                    v[i].z = z;
                    v[i].u = std::max(xs[i] - originX, 0.0f);
                    v[i].v = std::max(ys[i] - originY, 0.0f);
                    v[i].rgba = cornerColour;
                }
                v[0].u = std::max(cx - originX + du, 0.0f);
                v[0].v = std::max(cy - originY + dv, 0.0f);
                v[0].rgba = centreColour | (std::min(static_cast<uint32_t>(std::max(alpha, 0.0f)), 255u) << 24);
                const int fan[12] = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 1};
                for (int i : fan)
                    triangles.push_back(v[i]);
            }
            flush(s, triangles);
            s.sampleTarget = false;
        }

        // The effects image's lightning (0x1198; checked against the 4,029 points of 237 PS2X_VU1_RECORD runs: positions,
        // colours and texture coordinates exact). Bolt b (VU 300.w of them) runs between VU 8 + 2b and 9 + 2b, from the
        // farther end (the larger w) to the nearer: subdivided while 0.5 L (at most 65, L the length, VU 301.z) halved
        // stays at 1.5 or over, each midpoint (a + b) / 2 + (r - 1.5) s (s = VU 301.y L, halved per level; r three of
        // VU1's random numbers seeded from the start's w, two fresh at each level). Each point is drawn as two vertices
        // across the bolt on screen: (VU 5.x dy, -VU 5.y dx) VU 301.w / |d| (d the ends on screen, y doubled; an end
        // behind the camera taken at w = 1), times q; texture S from -VU 301.x L / 2 + start.w - 1.5 by VU 301.x L / 2^n,
        // T 1 and 0 (0 and 1 when start.w < 1.5); colours between palette rows 264 + i and 265 + i, i from 29 down (28
        // up when the start is the farther), 1 / 2^n a point; fog VU 4.w + (VU 5.w - L) q, at most VU 7.w. A point
        // whose corners (or the previous point's) leave the guard band takes ADC: a strip broken there.
        void lightning(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            if (!useMaterial(s, 300u, 0u, triangles, 0x42412u))
                return;
            const float(&c4)[4] = s.fxConst[0];
            const float(&c5)[4] = s.fxConst[1];
            const float(&c6)[4] = s.fxConst[2];
            const float(&c7)[4] = s.fxConst[3];
            const float guard = std::fabs(c6[3]);
            float m[16];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    m[j * 4 + k] = vu.f(j, k);
            const auto project = [&](const float p[3], float out[4])
            {
                for (int i = 0; i < 4; ++i)
                    out[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
            };
            const float x301 = vu.f(301, 0), y301 = vu.f(301, 1), z301 = vu.f(301, 2), w301 = vu.f(301, 3);
            const uint32_t bolts = vu.w(300, 3) & 0xffffu;
            for (uint32_t b = 0; b < bolts; ++b)
            {
                float start[4], end[4];
                for (int i = 0; i < 4; ++i)
                {
                    start[i] = vu.f(8u + 2u * b, i);
                    end[i] = vu.f(9u + 2u * b, i);
                }
                const float d3[3] = {end[0] - start[0], end[1] - start[1], end[2] - start[2]};
                const float length = std::sqrt(d3[0] * d3[0] + d3[1] * d3[1] + d3[2] * d3[2]);
                float sc[4], ec[4];
                project(start, sc);
                project(end, ec);
                const bool startBehind = sc[3] < 1.0f, endBehind = ec[3] < 1.0f;
                if (startBehind && endBehind)
                    continue;
                const bool fromEnd = !(ec[3] - sc[3] < 0.0f); // MSUB.w vf0 at 0x1260: from the farther end
                float first[4], second[4];
                std::memcpy(first, fromEnd ? ec : sc, sizeof(first));
                std::memcpy(second, fromEnd ? sc : ec, sizeof(second));
                if (startBehind || endBehind)
                {
                    const bool firstBehind = fromEnd ? endBehind : startBehind;
                    float *p = firstBehind ? first : second;
                    const float *o = firstBehind ? second : first;
                    const float t = (1.0f - o[3]) / (p[3] - o[3]);
                    for (int i : {0, 1, 3})
                        p[i] = o[i] + (p[i] - o[i]) * t;
                }
                const float qa = first[3] != 0.0f ? 1.0f / first[3] : 0.0f, qb = second[3] != 0.0f ? 1.0f / second[3] : 0.0f;
                const float dx = first[0] * qa - second[0] * qb, dy = 2.0f * (first[1] * qa - second[1] * qb);
                const float span = std::sqrt(dx * dx + dy * dy);
                if (!(span > 0.0f))
                    continue;
                const float perp[2] = {c5[0] * dy / span * w301, -c5[1] * dx / span * w301};
                std::vector<std::array<float, 3>> points;
                points.push_back(fromEnd ? std::array<float, 3>{end[0], end[1], end[2]} : std::array<float, 3>{start[0], start[1], start[2]});
                points.push_back(fromEnd ? std::array<float, 3>{start[0], start[1], start[2]} : std::array<float, 3>{end[0], end[1], end[2]});
                uint32_t random = vuRandInit(start[3]);
                float scale = y301 * length, level = std::min(z301 * length, 65.0f);
                uint32_t levels = 0;
                while (level >= 1.5f && points.size() < 513u)
                {
                    random = vuRandNext(random);
                    float rx = vuRandFloat(random);
                    random = vuRandNext(random);
                    float ry = vuRandFloat(random);
                    std::vector<std::array<float, 3>> next;
                    next.reserve(points.size() * 2u);
                    next.push_back(points[0]);
                    for (size_t i = 0; i + 1 < points.size(); ++i)
                    {
                        random = vuRandNext(random);
                        const float rz = vuRandFloat(random);
                        const float r[3] = {rx, ry, rz};
                        std::array<float, 3> mid;
                        for (int k = 0; k < 3; ++k)
                            mid[k] = points[i][k] * 0.5f + (r[k] - 1.5f) * scale + points[i + 1][k] * 0.5f;
                        next.push_back(mid);
                        next.push_back(points[i + 1]);
                        random = vuRandNext(random);
                        rx = vuRandFloat(random);
                        random = vuRandNext(random);
                        ry = vuRandFloat(random);
                    }
                    points.swap(next);
                    scale *= 0.5f;
                    level *= 0.5f;
                    ++levels;
                }
                const float step = x301 * length / static_cast<float>(1u << levels);
                float sCoord = -0.5f * x301 * length + (start[3] - 1.5f);
                float index = fromEnd ? 29.0f : 28.0f;
                const float indexStep = (fromEnd ? -1.0f : 1.0f) / static_cast<float>(1u << levels);
                const bool flip = start[3] - 1.5f >= 0.0f;
                std::vector<ps2x::native::Vertex> strip;
                std::vector<bool> adc;
                bool outsideBefore = false;
                for (const auto &p : points)
                {
                    float c[4];
                    project(p.data(), c);
                    const float q = 1.0f / c[3];
                    const float fogA = c4[3] + c5[3] * q - length * q;
                    const float fog = std::min(fogA, c7[3]);
                    const int32_t i = static_cast<int32_t>(index);
                    const float frac = index - static_cast<float>(i);
                    const uint32_t low = (264u + static_cast<uint32_t>(i)) & 0x3ffu, high = (265u + static_cast<uint32_t>(i)) & 0x3ffu;
                    uint32_t rgba = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        const float lo = static_cast<float>(static_cast<int32_t>(vu.w(low, k)));
                        const float hi = static_cast<float>(static_cast<int32_t>(vu.w(high, k)));
                        const float value = (lo + 8388608.0f) - lo * frac + hi * frac;
                        rgba |= (static_cast<uint32_t>(static_cast<int32_t>(value - 8388608.0f)) & 0xffu) << (8 * k);
                    }
                    ps2x::native::Vertex a, v;
                    a.x = c[0] * q - perp[0] * q;
                    a.y = c[1] * q - perp[1] * q;
                    a.z = c[2] * q - d3[2] * q;
                    v.x = c[0] * q + perp[0] * q;
                    v.y = c[1] * q + perp[1] * q;
                    v.z = c[2] * q + d3[2] * q;
                    a.u = v.u = sCoord;
                    a.v = flip ? 1.0f : 0.0f;
                    v.v = flip ? 0.0f : 1.0f;
                    a.rgba = v.rgba = rgba;
                    a.fog = v.fog = std::floor(std::max(fog, 0.0f)) / 255.0f;
                    const bool outside = std::fabs(a.x + c6[0]) > guard || std::fabs(a.y + c6[1]) > guard || c6[2] + fogA < -guard ||
                                         std::fabs(v.x + c6[0]) > guard || std::fabs(v.y + c6[1]) > guard || c6[2] + c[3] < -guard;
                    const bool kick = !(outside || outsideBefore);
                    outsideBefore = outside;
                    strip.push_back(a);
                    strip.push_back(v);
                    adc.push_back(!kick);
                    adc.push_back(!kick);
                    sCoord += step;
                    index += indexStep;
                }
                for (size_t i = 2; i < strip.size(); ++i)
                {
                    if (adc[i])
                        continue;
                    triangles.push_back(strip[i - 2]);
                    triangles.push_back(strip[i - 1]);
                    triangles.push_back(strip[i]);
                }
            }
        }

        // The effects image's beams (0xcf8; not seen in the recorded matches: derived from its code, its strip being drawn by
        // the lightning's builder at 0xf38, which 0x1198's check covers). n = VU 296.w points, VU 7 + n down to VU 8 (VU 8
        // up when the first is the nearer, the steps then reversed), drawn as the lightning's strip: across the beam by
        // (VU 5.x dy, -VU 5.y dx) / |d| (d from the first point to VU 8 on screen, y doubled; an end behind the camera taken
        // at w = 1) times a width; texture S, palette index and width from VU 298 (x, z, w), stepping by VU 297 a point;
        // T 1 and 0; fog VU 4.w + VU 5.w q (at most VU 7.w).
        void beams(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            if (!useMaterial(s, 296u, 0u, triangles, vu.w(296, 2)) || vu.w(296, 2) != 0x42412u)
                return;
            const float(&c4)[4] = s.fxConst[0];
            const float(&c5)[4] = s.fxConst[1];
            const float(&c6)[4] = s.fxConst[2];
            const float(&c7)[4] = s.fxConst[3];
            const float guard = std::fabs(c6[3]);
            int32_t count = static_cast<int32_t>(vu.w(296, 3));
            float object[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
            if (count < 0)
            {
                for (int j = 0; j < 4; ++j)
                    for (int k = 0; k < 4; ++k)
                        object[j][k] = vu.f(s.top + 3u + j, k);
                count &= 0x7fff;
            }
            if (count < 2)
                return;
            float m[16];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    m[j * 4 + k] = vu.f(0, k) * object[j][0] + vu.f(1, k) * object[j][1] + vu.f(2, k) * object[j][2] + vu.f(3, k) * object[j][3];
            const auto project = [&](uint32_t at, float out[4])
            {
                const float p[3] = {vu.f(at & 0x3ffu, 0), vu.f(at & 0x3ffu, 1), vu.f(at & 0x3ffu, 2)};
                for (int i = 0; i < 4; ++i)
                    out[i] = m[i] * p[0] + m[4 + i] * p[1] + m[8 + i] * p[2] + m[12 + i];
            };
            float first[4], last[4];
            project(7u + static_cast<uint32_t>(count), first);
            project(8u, last);
            const bool firstBehind = first[3] < 1.0f, lastBehind = last[3] < 1.0f;
            if (firstBehind && lastBehind)
                return;
            float a[4], b[4];
            std::memcpy(a, first, sizeof(a));
            std::memcpy(b, last, sizeof(b));
            if (firstBehind || lastBehind)
            {
                float *p = firstBehind ? a : b;
                const float *o = firstBehind ? b : a;
                const float t = (1.0f - o[3]) / (p[3] - o[3]);
                for (int i : {0, 1, 3})
                    p[i] = o[i] + (p[i] - o[i]) * t;
            }
            const float qa = 1.0f / a[3], qb = 1.0f / b[3];
            const float dx = a[0] * qa - b[0] * qb, dy = 2.0f * (a[1] * qa - b[1] * qb);
            const float span = std::sqrt(dx * dx + dy * dy);
            if (!(span > 0.0f))
                return;
            const float perp[2] = {c5[0] * dy / span, -c5[1] * dx / span};
            float sCoord = vu.f(298, 0), index = vu.f(298, 2), width = vu.f(298, 3);
            float sStep = vu.f(297, 0), indexStep = vu.f(297, 2), widthStep = vu.f(297, 3);
            uint32_t at = 7u + static_cast<uint32_t>(count);
            int32_t direction = -1;
            if (first[3] - last[3] < 0.0f) // the first is the nearer: from VU 8 up, the steps from the far end back
            {
                at = 8u;
                direction = 1;
                sCoord += sStep * static_cast<float>(count - 1);
                index += indexStep * static_cast<float>(count - 1);
                width += widthStep * static_cast<float>(count - 1);
                sStep = -sStep;
                indexStep = -indexStep;
                widthStep = -widthStep;
            }
            std::vector<ps2x::native::Vertex> strip;
            std::vector<bool> adc;
            bool outsideBefore = false;
            for (int32_t k = 0; k < count; ++k, at = static_cast<uint32_t>(static_cast<int32_t>(at) + direction))
            {
                float c[4];
                project(at, c);
                const float q = 1.0f / c[3];
                const float fogA = c4[3] + c5[3] * q;
                const float fog = std::min(fogA, c7[3]);
                const int32_t i = static_cast<int32_t>(index);
                const float frac = index - static_cast<float>(i);
                const uint32_t low = (264u + static_cast<uint32_t>(i)) & 0x3ffu, high = (265u + static_cast<uint32_t>(i)) & 0x3ffu;
                uint32_t rgba = 0;
                for (int n = 0; n < 4; ++n)
                {
                    const float lo = static_cast<float>(static_cast<int32_t>(vu.w(low, n)));
                    const float hi = static_cast<float>(static_cast<int32_t>(vu.w(high, n)));
                    const float value = (lo + 8388608.0f) - lo * frac + hi * frac;
                    rgba |= (static_cast<uint32_t>(static_cast<int32_t>(value - 8388608.0f)) & 0xffu) << (8 * n);
                }
                ps2x::native::Vertex va, vb;
                va.x = c[0] * q - perp[0] * width * q;
                va.y = c[1] * q - perp[1] * width * q;
                vb.x = c[0] * q + perp[0] * width * q;
                vb.y = c[1] * q + perp[1] * width * q;
                va.z = vb.z = c[2] * q;
                va.u = vb.u = sCoord;
                va.v = 1.0f;
                vb.v = 0.0f;
                va.rgba = vb.rgba = rgba;
                va.fog = vb.fog = std::floor(std::max(fog, 0.0f)) / 255.0f;
                const bool outside = std::fabs(va.x + c6[0]) > guard || std::fabs(va.y + c6[1]) > guard || c6[2] + fogA < -guard ||
                                     std::fabs(vb.x + c6[0]) > guard || std::fabs(vb.y + c6[1]) > guard || c6[2] + c[3] < -guard;
                const bool kick = !(outside || outsideBefore);
                outsideBefore = outside;
                strip.push_back(va);
                strip.push_back(vb);
                adc.push_back(!kick);
                adc.push_back(!kick);
                sCoord += sStep;
                index += indexStep;
                width += widthStep;
            }
            for (size_t i = 2; i < strip.size(); ++i)
            {
                if (adc[i])
                    continue;
                triangles.push_back(strip[i - 2]);
                triangles.push_back(strip[i - 1]);
                triangles.push_back(strip[i]);
            }
        }

        void particles(State &s, std::vector<ps2x::native::Vertex> &triangles, bool first)
        {
            ++g_stats.strips;
            const VuImage &vu = s.vu;
            const uint32_t top = s.top;
            uint32_t tagQw = top, data = top + 1u;
            if (first)
            {
                tagQw = top + 2u;
                data = top + 3u;
                const int32_t list = static_cast<int32_t>(vu.w(top, 3));
                float object[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
                if (list < 0)
                    for (int j = 0; j < 4; ++j)
                        for (int k = 0; k < 4; ++k)
                            object[j][k] = vu.f(top + 5u + 5u * j, k);
                for (int j = 0; j < 4; ++j)
                    for (int k = 0; k < 4; ++k)
                        s.fxMatrix[j * 4 + k] = vu.f(0, k) * object[j][0] + vu.f(1, k) * object[j][1] + vu.f(2, k) * object[j][2] +
                                                vu.f(3, k) * object[j][3];
                s.fxNext = (static_cast<uint32_t>(list) & 0x7fffu) + 7u;
                s.fxFade = (vu.w(top, 2) & 0x8000u) != 0u;
                if (!useMaterial(s, tagQw, 0u, triangles, 0x43431u, top + 1u, vu.w(top, 0) & 0x7fffu))
                    return;
            }
            else if (!useMaterial(s, tagQw, 0u, triangles, 0x43431u))
                return;
            const float(&c4)[4] = s.fxConst[0]; // VU 4 (vf29)
            const float(&c5)[4] = s.fxConst[1]; // VU 5 (vf27)
            const float(&c6)[4] = s.fxConst[2]; // VU 6 (vf28)
            const float(&c7)[4] = s.fxConst[3]; // VU 7 (vf31)
            const uint32_t width = 1u << ((s.tex0 >> 26) & 0xfu), height = 1u << ((s.tex0 >> 30) & 0xfu);
            const uint32_t count = vu.w(tagQw, 0) & 0x7fffu;
            const float guard = std::fabs(c6[3]);
            for (uint32_t k = 0; k < count; ++k)
            {
                const uint32_t at = s.fxNext-- & 0x3ffu;
                const float p[4] = {vu.f(at, 0), vu.f(at, 1), vu.f(at, 2), vu.f(at, 3)};
                float c[4];
                for (int i = 0; i < 4; ++i)
                    c[i] = s.fxMatrix[i] * p[0] + s.fxMatrix[4 + i] * p[1] + s.fxMatrix[8 + i] * p[2] + s.fxMatrix[12 + i];
                const float q = 1.0f / c[3];
                float half[3];
                for (int i = 0; i < 3; ++i)
                    half[i] = c5[i] * p[3] * q;
                float fog, fade = 1.0f;
                if (!s.fxFade)
                {
                    half[0] = std::min(half[0], c5[0] * 0.5f);
                    half[1] = std::min(half[1], c5[1] * 0.5f);
                    fog = std::max(std::min(c4[3] + c5[3] * q, c7[3]), 0.0f);
                }
                else
                    fog = fade = std::max(std::min((c4[3] + c5[3] * q) / c7[3], 1.0f), 0.0f);
                const float x0 = c[0] * q - half[0], y0 = c[1] * q - half[1];
                const float x1 = c[0] * q + half[0], y1 = c[1] * q + half[1], z = c[2] * q + half[2];
                if (std::fabs(x0 + c6[0]) > guard || std::fabs(y0 + c6[1]) > guard || std::fabs(x1 + c6[0]) > guard ||
                    std::fabs(y1 + c6[1]) > guard || c6[2] + c[3] < -guard) // the raw w (ADDw.z vf18 before the divide)
                    continue;
                const uint32_t qw = data + 5u * k;
                uint32_t rgba = 0;
                for (int i = 0; i < 3; ++i)
                {
                    const float value = static_cast<float>(static_cast<int32_t>(vu.w(qw, i))) + c4[i];
                    const uint32_t byte = s.fxFade ? static_cast<uint32_t>(static_cast<int32_t>(value)) & 0xffu : clampByte(std::min(value, 255.0f));
                    rgba |= byte << (8 * i);
                }
                const float alpha = static_cast<float>(static_cast<int32_t>(vu.w(qw, 3))) * fade;
                const uint32_t alphaByte = s.fxFade ? static_cast<uint32_t>(static_cast<int32_t>(alpha)) & 0xffu : clampByte(std::min(alpha, 255.0f));
                rgba |= alphaByte << 24;
                const uint32_t row = 296u + vu.w(qw + 1u, 0);
                const float u0 = static_cast<float>(vu.w(row, 0)) / 16.0f / width, v0 = static_cast<float>(vu.w(row, 1)) / 16.0f / height;
                const float u1 = static_cast<float>(vu.w(row, 2)) / 16.0f / width, v1 = static_cast<float>(vu.w(row, 3)) / 16.0f / height;
                ps2x::native::Vertex corner[4];
                const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1}, us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
                for (int i = 0; i < 4; ++i)
                {
                    corner[i].x = xs[i];
                    corner[i].y = ys[i];
                    corner[i].z = z; // a sprite takes its second vertex's Z and fog
                    corner[i].u = us[i];
                    corner[i].v = vs[i];
                    corner[i].rgba = rgba;
                    corner[i].fog = std::floor(fog) / 255.0f;
                }
                const int order[6] = {0, 1, 2, 1, 2, 3}; // as GifToNative splits a sprite (the check compares in order)
                for (int i : order)
                    triangles.push_back(corner[i]);
            }
        }

        void strip(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            if (s.mode == Mode::None)
                return;
            if (s.mode == Mode::Particles)
            {
                particles(s, triangles, false);
                return;
            }
            if (s.mode == Mode::TexParticles)
            {
                texParticles(s, triangles, false);
                return;
            }
            if (s.mode == Mode::Lightning) // 0x1198 ends at 0x1830: its MSCNT is a trail's MSCAL
            {
                s.mode = Mode::Trails;
                trails(s, triangles);
                return;
            }
            if (s.mode == Mode::Trails)
            {
                static uint32_t s_logged = 0;
                if (s_logged++ < 5u)
                    std::fprintf(stderr, "[tmb:native] an MSCNT after a trail (0x1c38): not translated, not drawn\n");
                return;
            }
            if (s.mode == Mode::Streaks) // 0x118 ends at 0x390: its MSCNT is that program's start
            {
                s.mode = Mode::Particles;
                particles(s, triangles, true);
                return;
            }
            if (s.mode == Mode::Reflective)
            {
                reflectiveStrip(s, triangles);
                return;
            }
            if (s.mode == Mode::Ribbon)
            {
                ribbon(s, triangles);
                return;
            }
            if (s.mode == Mode::SkinDirectional || s.mode == Mode::SkinUnlit)
            {
                skinnedStrip(s, triangles, s.mode == Mode::SkinDirectional);
                return;
            }
            if (s.mode == Mode::Sprite)
            {
                sprite(s, triangles);
                return;
            }
            ++g_stats.strips;
            const uint32_t top = s.top;
            const uint32_t tag0 = s.vu.w(top, 0);
            if (!useMaterial(s, top, s.vu.w(top, 3), triangles))
                return;
            const uint32_t nloop = tag0 & 0x7fffu;
            const float uOffset = s.vu.f(4, 0), vOffset = s.vu.f(4, 1);
            std::vector<ps2x::native::Vertex> strip;
            strip.reserve(nloop);
            for (uint32_t v = 0; v < nloop; ++v)
            {
                const uint32_t q = top + 1u + 3u * v;
                const uint32_t index = s.vu.w(q, 3) & 0x7fffu;
                ps2x::native::Vertex out;
                if (static_cast<size_t>(index) * 3u + 2u >= s.pool.size())
                {
                    strip.push_back(out); // marks a break: no triangle uses it (below)
                    strip.back().rgba = 0xdeadu;
                    continue;
                }
                const float *pos = &s.pool[index * 3u];
                out.x = pos[0];
                out.y = pos[1];
                out.z = pos[2];
                const uint32_t normal[3] = {s.vu.w(q, 0), s.vu.w(q, 1), s.vu.w(q, 2)};
                const uint32_t rgba[4] = {s.vu.w(q + 1, 0), s.vu.w(q + 1, 1), s.vu.w(q + 1, 2), s.vu.w(q + 1, 3)};
                out.rgba = shade(s, normal, rgba, pos);
                out.u = static_cast<float>(static_cast<int32_t>(s.vu.w(q + 2, 0))) / 4096.0f + uOffset;
                out.v = static_cast<float>(static_cast<int32_t>(s.vu.w(q + 2, 1))) / 4096.0f + vOffset;
                const float w = s.matrix[3] * pos[0] + s.matrix[7] * pos[1] + s.matrix[11] * pos[2] + s.matrix[15];
                out.fog = w > 0.0f ? fogFactor(s.vu, 1.0f / w) : 0.0f;
                strip.push_back(out);
            }
            for (size_t i = 2; i < strip.size(); ++i)
            {
                if (strip[i].rgba == 0xdeadu || strip[i - 1].rgba == 0xdeadu || strip[i - 2].rgba == 0xdeadu)
                    continue;
                triangles.push_back(strip[i - 2]);
                triangles.push_back(strip[i - 1]);
                triangles.push_back(strip[i]);
            }
        }

        // The GS blend (A - B) * C + D (ALPHA_1: A, B, D pick Cs / Cd / 0, C picks As / Ad / FIX) as a PC blend: Cs
        // over Cd by As (TMB's 0x44), or Cs * As added to Cd (0x48, and 0x49's Cd * As + Cd, which brightens what is
        // behind: drawn as additive too).
        ps2x::native::Material::Blend blendFor(uint64_t alpha)
        {
            const uint32_t a = alpha & 3u, b = (alpha >> 2) & 3u, d = (alpha >> 6) & 3u;
            if (b == 2u && d == 1u)
                return ps2x::native::Material::Blend::Additive; // (A - 0) * C + Cd
            if (a == 0u && b == 1u && d == 1u)
                return ps2x::native::Material::Blend::Alpha; // (Cs - Cd) * C + Cd
            return ps2x::native::Material::Blend::Alpha;
        }

        void flush(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            if (s.collecting)
            {
                s.checkMine.insert(s.checkMine.end(), triangles.begin() + static_cast<std::ptrdiff_t>(std::min(s.particleFrom, triangles.size())),
                                   triangles.end());
                s.particleFrom = 0;
            }
            s.flushed = true;
            if (triangles.empty())
                return;
            g_stats.triangles += triangles.size() / 3u;
            g_stats.textured += s.haveTexture ? 1u : 0u;
            ps2x::native::Material material;
            material.textured = s.haveTexture && !s.sampleTarget;
            material.sampleTarget = s.sampleTarget;
            material.tex0 = s.tex0;
            material.blend = s.blended ? blendFor(s.batchAlpha) : ps2x::native::Material::Blend::Opaque;
            material.depthWrite = s.mode != Mode::Rain && !s.batchDepthMasked;
            // TEST_1's alpha test (TMB: alpha greater than 0x40, else neither colour nor depth): a sprite's or a strip's
            // clear texels must not write depth over what is drawn behind it afterwards.
            const uint32_t atst = (s.batchTest >> 1) & 7u, aref = (s.batchTest >> 4) & 0xffu;
            if ((s.batchTest & 1u) && (atst == 5u || atst == 6u))
            {
                material.alphaTest = atst == 6u || aref > 0u;
                material.alphaRef = static_cast<uint8_t>(atst == 6u ? aref : aref - 1u);
            }
            material.fog = s.fogged && s.mode != Mode::Rain;
            material.fogColour = s.batchFogColour;
            static const float kScreenSpace[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            ps2x::native::Renderer::instance().draw(material, s.mode == Mode::Sprite || s.mode == Mode::Rain || s.mode == Mode::Ribbon || s.mode == Mode::Particles ||
                                                                s.mode == Mode::LensFlares || s.mode == Mode::Streaks || s.mode == Mode::Trails ||
                                                                s.mode == Mode::TexParticles || s.mode == Mode::HeatHaze ||
                                                                s.mode == Mode::Lightning || s.mode == Mode::Beams
                                                            ? kScreenSpace
                                                            : s.matrix, triangles.data(),
                                                    static_cast<uint32_t>(triangles.size()));
            triangles.clear();
        }

        // The effects image's rain (0x1d40, setup at 0x1c38): VU 20.w drops, each a line from A (p + VU 12) to
        // B (p + VU 12 - VU 14), A = VU 0-3 and B = VU 8-11 with their z rows scaled by 1/16. The drops' p follow a fixed
        // recurrence, so they stay put in the world, tiled every VU 15.w units and falling as VU 12 moves:
        //   u0 = (VU 15 + VU 21) s, u' = wrap(u) VU 21 + VU 15 s (s = VU 15.w), wrap: minus s above s;
        //   p0 = VU 21, p1 = VU 15 s, then wrap(u), each plus s below VU 13 (the box around the camera).
        // The head's alpha is 255 - 3.1875 w (the tail's w), the tail's 1; lines that fade out, leave a 480-pixel guard
        // band, lie behind the camera or are too far (z under 2049 - 480) are dropped. VU1 rounds by truncation, so
        // this sequence drifts from VU1's after some drops: the same rain, not the same drops. Each line is drawn as a
        // one-pixel quad in screen space.
        void rain(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            flush(s, triangles);
            ps2x::native::Renderer::instance().setKey(0);
            const VuImage &vu = s.vu;
            s.mode = Mode::Rain;
            s.tex0 = s.directTex0;
            s.haveTexture = s.directTex0 != 0u;
            s.blended = true;
            s.batchAlpha = s.alpha;
            float a[4][4], b[4][4];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                {
                    a[j][k] = vu.f(j, k) * (k == 2 ? 0.0625f : 1.0f);
                    b[j][k] = vu.f(8 + j, k) * (k == 2 ? 0.0625f : 1.0f);
                }
            const float step = vu.f(15, 3);
            float head[3], tail[3], c[3], mul[3], box[3], p[3], next[3], u[3];
            for (int k = 0; k < 3; ++k)
            {
                head[k] = vu.f(12, k);
                tail[k] = vu.f(12, k) - vu.f(14, k);
                c[k] = vu.f(15, k) * step;
                mul[k] = vu.f(21, k);
                box[k] = vu.f(13, k);
                p[k] = mul[k];
                next[k] = c[k] < box[k] ? c[k] + step : c[k];
                u[k] = (vu.f(15, k) + mul[k]) * step;
            }
            auto transform = [](const float m[4][4], const float *q, const float *offset, float out[4])
            {
                for (int k = 0; k < 4; ++k)
                    out[k] = m[0][k] * (q[0] + offset[0]) + m[1][k] * (q[1] + offset[1]) + m[2][k] * (q[2] + offset[2]) + m[3][k];
            };
            // C = VU 16-19: the texture coordinates, written as ST = (x / w, y / w, z), so the texel is at
            // (x / w / z, y / w / z) (z is 1 in TMB's matrices).
            float c3[4][4];
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    c3[j][k] = vu.f(16 + j, k);
            const float zero[3] = {0.0f, 0.0f, 0.0f};
            const uint32_t drops = std::min(vu.w(20, 3), 65536u);
            for (uint32_t n = 0; n < drops; ++n)
            {
                float h[4], t[4], st[4];
                transform(a, p, head, h);
                transform(b, p, tail, t);
                transform(c3, p, zero, st); // the line's texture coordinates (one ST for both ends)
                for (int k = 0; k < 3; ++k)
                {
                    p[k] = next[k];
                    const float wrapped = u[k] > step ? u[k] - step : u[k];
                    next[k] = wrapped < box[k] ? wrapped + step : wrapped;
                    u[k] = wrapped * mul[k] + c[k];
                }
                if (h[3] <= 0.0f || t[3] <= 0.0f)
                    continue;
                const float alpha = std::round(255.0f - 3.1875f * t[3]);
                const float hx = h[0] / h[3], hy = h[1] / h[3], hz = h[2] / h[3];
                const float tx = t[0] / t[3], ty = t[1] / t[3], tz = t[2] / t[3];
                if (alpha < 1.0f || std::fabs(hx - 2048.0f) > 480.0f || std::fabs(hy - 2048.0f) > 480.0f ||
                    std::fabs(tx - 2048.0f) > 480.0f || std::fabs(ty - 2048.0f) > 480.0f || hz < 1569.0f || tz < 1569.0f)
                    continue;
                const float dx = tx - hx, dy = ty - hy;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (length <= 0.0f)
                    continue;
                const float nx = -dy / length * 0.5f, ny = dx / length * 0.5f;
                const float corner[4][3] = {{hx + nx, hy + ny, hz}, {hx - nx, hy - ny, hz}, {tx + nx, ty + ny, tz}, {tx - nx, ty - ny, tz}};
                ps2x::native::Vertex quad[4];
                for (int i = 0; i < 4; ++i)
                {
                    quad[i].x = corner[i][0];
                    quad[i].y = corner[i][1];
                    quad[i].z = corner[i][2];
                    const float w = std::max(st[3], 0.0f), q = st[2] != 0.0f ? st[2] : 1.0f;
                    quad[i].u = w > 0.0f ? st[0] / w / q : 0.0f;
                    quad[i].v = w > 0.0f ? st[1] / w / q : 0.0f;
                    quad[i].rgba = 0x00808080u | ((i < 2 ? static_cast<uint32_t>(alpha) : 1u) << 24);
                }
                const int order[6] = {0, 1, 2, 1, 2, 3};
                for (int i : order)
                    triangles.push_back(quad[i]);
            }
            // TMB_NATIVE_LOG_RAIN=1: the rain's texture, blend and drawn drops, once a match is under way.
            static const bool s_logRain = []
            {
                const char *value = std::getenv("TMB_NATIVE_LOG_RAIN");
                return value && *value == '1';
            }();
            static uint32_t s_rainFrames = 0;
            if (s_logRain && ++s_rainFrames % 500u == 0u)
                std::fprintf(stderr, "[tmb:native] rain: tex0 %#llx alpha %#llx, %zu vertices, VU 16 (%.3f %.3f %.3f %.3f) VU 19 (%.3f %.3f %.3f %.3f)\n",
                             static_cast<unsigned long long>(s.tex0), static_cast<unsigned long long>(s.batchAlpha), triangles.size(),
                             vu.f(16, 0), vu.f(16, 1), vu.f(16, 2), vu.f(16, 3), vu.f(19, 0), vu.f(19, 1), vu.f(19, 2), vu.f(19, 3));
            flush(s, triangles);
            s.mode = Mode::None;
        }

        // TEX0_1 and ALPHA_1 among a DIRECT packet's A+D writes (PACKED GIF tags).
        void readDirect(State &s, const uint32_t *words, size_t qwords)
        {
            size_t q = 0;
            while (q < qwords)
            {
                const uint64_t lo = words[q * 4] | (static_cast<uint64_t>(words[q * 4 + 1]) << 32);
                const uint64_t regs = words[q * 4 + 2] | (static_cast<uint64_t>(words[q * 4 + 3]) << 32);
                ++q;
                const uint32_t nloop = lo & 0x7fffu, flg = (lo >> 58) & 3u;
                const uint32_t nreg = ((lo >> 60) & 0xfu) ? static_cast<uint32_t>((lo >> 60) & 0xfu) : 16u;
                // TMB_NATIVE_LOG_DIRECT_TAGS=1: the kinds of GIF tags DIRECT packets carry (do they draw?).
                static const bool s_logTags = std::getenv("TMB_NATIVE_LOG_DIRECT_TAGS") != nullptr;
                if (s_logTags)
                {
                    static std::unordered_map<uint64_t, uint64_t> s_kinds;
                    static uint64_t s_tags = 0;
                    const uint64_t kind = (regs & 0xffffffffffull) ^ (static_cast<uint64_t>(flg) << 60) ^ (static_cast<uint64_t>(nreg) << 52) ^
                                          (((lo >> 46) & 1u) ? ((lo >> 47) & 0x7ffu) << 40 : 0u);
                    s_kinds[kind] += nloop;
                    if (++s_tags % 5000u == 0u)
                        for (const auto &[k, loops] : s_kinds)
                            std::fprintf(stderr, "[tmb:native] direct tag kind %#llx: %llu loops\n", static_cast<unsigned long long>(k),
                                         static_cast<unsigned long long>(loops));
                }
                if (flg == 0u)
                {
                    for (uint32_t loop = 0; loop < nloop && q < qwords; ++loop)
                        for (uint32_t r = 0; r < nreg && q < qwords; ++r, ++q)
                            if (((regs >> (4u * r)) & 0xfu) == 0xeu)
                            {
                                const uint64_t value = words[q * 4] | (static_cast<uint64_t>(words[q * 4 + 1]) << 32);
                                const uint32_t address = words[q * 4 + 2] & 0xffu;
                                logRegister("direct", address, value);
                                if (address == 0x4eu)
                                    s.depthMasked = (value >> 32) & 1u;
                                else if (address == 0x47u)
                                    s.test = value;
                                else if (address == 0x06u)
                                    s.directTex0 = s.lastTex0 = value;
                                else if (address == 0x42u)
                                    s.alpha = value;
                                else if (address == 0x3du)
                                    s.fogColour = static_cast<uint32_t>(value & 0xffffffu);
                                else if (address == 0x00u)
                                    s.prim = static_cast<uint32_t>(value & 0x7ffu);
                            }
                }
                else if (flg == 1u)
                    q += (static_cast<size_t>(nloop) * nreg + 1u) / 2u;
                else
                    q += nloop;
            }
        }

        // TMB_NATIVE_CHECK=<n>: each translated strip's triangles (to the screen through the object's matrix) against the
        // triangles VU1's own program draws from the same VU memory (run on a second sandbox, captured), in order: the
        // count, then per vertex the GS position, Z, colour (alpha included) and texture coordinates; the first <n>
        // differences per setup logged ([tmb:check]), and every 2000 strips a summary per setup.
        ps2x::native::Screen screen();

        long checkLimit()
        {
            static const long s_limit = []
            {
                const char *value = std::getenv("TMB_NATIVE_CHECK");
                return value && *value ? std::atol(value) : 0L;
            }();
            return s_limit;
        }

        // The memory the check's VU1 runs on: the adapter's copy itself (a vertex pass's writes, the pool's 1 / w and its
        // screen positions, are what the strips after it read there), but a copy of it for the effects image's programs,
        // whose output buffers (0x110-0x30c) would otherwise overwrite what the next objects' translations read.
        uint8_t *checkMemory(State &s, bool effects)
        {
            if (!effects)
                return reinterpret_cast<uint8_t *>(s.vu.words);
            static std::vector<uint32_t> s_copy;
            s_copy.assign(std::begin(s.vu.words), std::end(s.vu.words));
            return reinterpret_cast<uint8_t *>(s_copy.data());
        }

        bool effectsMode(Mode mode)
        {
            return mode == Mode::Particles || mode == Mode::LensFlares || mode == Mode::Streaks || mode == Mode::Trails ||
                   mode == Mode::TexParticles || mode == Mode::HeatHaze || mode == Mode::Lightning || mode == Mode::Beams;
        }

        void checkTranslation(State &s, const ps2x::native::Vertex *mine, size_t mineCount)
        {
            struct Tally
            {
                uint64_t strips = 0, countDiffs = 0, vertexDiffs = 0;
                long logged = 0;
            };
            static std::unordered_map<uint32_t, Tally> s_tally;
            static uint64_t s_strips = 0;
            Tally &t = s_tally[s.checkSetup];
            ++t.strips;
            const std::vector<ps2x::native::Vertex> &vu = s.checkVertices;
            const auto screenOf = [&](const ps2x::native::Vertex &v, float out[3])
            {
                if (s.mode == Mode::Sprite || s.mode == Mode::Rain || s.mode == Mode::Ribbon || s.mode == Mode::Particles ||
                    s.mode == Mode::LensFlares || s.mode == Mode::Streaks || s.mode == Mode::Trails || s.mode == Mode::TexParticles ||
                    s.mode == Mode::HeatHaze || s.mode == Mode::Lightning || s.mode == Mode::Beams)
                {
                    out[0] = v.x, out[1] = v.y, out[2] = v.z;
                    return;
                }
                const float *m = s.matrix;
                const float c[4] = {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12], m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13],
                                    m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14], m[3] * v.x + m[7] * v.y + m[11] * v.z + m[15]};
                const float q = c[3] != 0.0f ? 1.0f / c[3] : 0.0f;
                out[0] = c[0] * q, out[1] = c[1] * q, out[2] = c[2] * q * screen().gsZScale;
                out[2] -= 16777216.0f * std::floor(out[2] / 16777216.0f); // XYZF2's 24 bits
            };
            const auto log = [&](const char *what, size_t i, const ps2x::native::Vertex *a, const ps2x::native::Vertex *b, const float *sa)
            {
                if (t.logged++ >= checkLimit())
                    return;
                std::fprintf(stderr, "[tmb:check] setup 0x%x mode %d %s: %s at vertex %zu: mine %zu tri, VU1 %zu", s.checkSetup,
                             static_cast<int>(s.mode), s.checkResumed ? "MSCNT" : "MSCAL", what, i, mineCount / 3u, vu.size() / 3u);
                if (a && b && sa)
                    std::fprintf(stderr, " | mine xyz %.1f %.1f %.1f rgba %08x uv %.4f %.4f fog %.2f | VU1 xyz %.1f %.1f %.1f rgba %08x uv %.4f %.4f fog %.2f",
                                 sa[0], sa[1], sa[2], a->rgba, a->u, a->v, a->fog, b->x, b->y, b->z, b->rgba, b->u, b->v, b->fog);
                std::fprintf(stderr, "\n");
            };
            // The draw state: depth write, blending, alpha test (the translation's batch against VU1's material).
            static std::unordered_map<uint32_t, uint64_t> s_stateDiffs;
            if (!s.checkMaterials.empty() && mineCount > 0u)
            {
                const ps2x::native::Material &vm = s.checkMaterials.back();
                const bool myDepthWrite = s.mode != Mode::Rain && !s.batchDepthMasked;
                const bool myBlend = s.blended;
                // Not the blend: the check's VU1 never gets the stream's DIRECT ALPHA (an ALPHA of 0 is opaque).
                const bool vuBlend = vm.blend != ps2x::native::Material::Blend::Opaque;
                if (myDepthWrite != vm.depthWrite)
                {
                    if (++s_stateDiffs[s.checkSetup] <= static_cast<uint64_t>(checkLimit()))
                        std::fprintf(stderr, "[tmb:check] setup 0x%x mode %d: state differs: mine depthWrite %d blend %d (alpha %#llx, depthMasked %d), VU1 depthWrite %d blend %d alphaTest %d\n",
                                     s.checkSetup, static_cast<int>(s.mode), myDepthWrite ? 1 : 0, myBlend ? 1 : 0,
                                     static_cast<unsigned long long>(s.batchAlpha), s.batchDepthMasked ? 1 : 0, vm.depthWrite ? 1 : 0,
                                     vuBlend ? 1 : 0, vm.alphaTest ? 1 : 0);
                }
            }
            if (mineCount != vu.size())
            {
                ++t.countDiffs;
                log("triangle count differs", 0, nullptr, nullptr, nullptr);
                // Why: the strip's vertices against the translation's pool (an index past it breaks the strip).
                if (mineCount < vu.size() && t.logged <= checkLimit() && s.mode != Mode::Sprite && s.mode != Mode::SkinDirectional && s.mode != Mode::SkinUnlit)
                {
                    const uint32_t top = s.top, nloop = s.vu.w(top, 0) & 0x7fffu;
                    uint32_t outside = 0, maxIndex = 0;
                    for (uint32_t v = 0; v < nloop; ++v)
                    {
                        const uint32_t index = s.vu.w(top + 1u + 3u * v, 3) & 0x7fffu;
                        maxIndex = std::max(maxIndex, index);
                        outside += static_cast<size_t>(index) * 3u + 2u >= s.pool.size() ? 1u : 0u;
                    }
                    std::fprintf(stderr, "[tmb:check]   strip: tag %08x %08x %08x %08x, %u vertices, pool %zu, %u indexes past it (max %u)\n",
                                 s.vu.w(top, 0), s.vu.w(top, 1), s.vu.w(top, 2), s.vu.w(top, 3), nloop, s.pool.size() / 3u, outside, maxIndex);
                }
            }
            const size_t n = std::min(mineCount, vu.size());
            for (size_t i = 0; i < n; ++i)
            {
                float sa[3];
                screenOf(mine[i], sa);
                const ps2x::native::Vertex &b = vu[i];
                const auto channel = [](uint32_t c, int k) { return static_cast<int>((c >> (8 * k)) & 0xffu); };
                int colour = 0;
                for (int k = 0; k < 4; ++k)
                    colour = std::max(colour, std::abs(channel(mine[i].rgba, k) - channel(b.rgba, k)));
                const float dz = std::fabs(sa[2] - b.z), zTol = std::max(2.0f, std::fabs(b.z) * 1e-3f);
                const bool bad = std::fabs(sa[0] - b.x) > 0.6f || std::fabs(sa[1] - b.y) > 0.6f || dz > zTol || colour > 2 ||
                                 std::fabs(mine[i].u - b.u) > 0.01f || std::fabs(mine[i].v - b.v) > 0.01f;
                if (bad)
                {
                    ++t.vertexDiffs;
                    log("vertex differs", i, &mine[i], &b, sa);
                    break; // the first difference of the strip
                }
            }
            if (++s_strips % 2000u == 0u)
                for (const auto &[setup, tally] : s_tally)
                    std::fprintf(stderr, "[tmb:check] setup 0x%x: %llu strips, %llu with another triangle count, %llu with a vertex differing\n",
                                 setup, static_cast<unsigned long long>(tally.strips), static_cast<unsigned long long>(tally.countDiffs),
                                 static_cast<unsigned long long>(tally.vertexDiffs));
        }

        void runVif(State &s, const uint32_t *words, size_t count)
        {
            std::vector<ps2x::native::Vertex> triangles;
            size_t i = 0;
            while (i < count)
            {
                const uint32_t code = words[i++];
                const uint32_t cmd = (code >> 24) & 0x7fu, num = (code >> 16) & 0xffu, imm = code & 0xffffu;
                if ((cmd & 0x60u) == 0x60u)
                {
                    // As VIF1 unpacks it (masks, row / column, add modes): the translations read this memory.
                    s.vif.cycle = s.cl | (s.wl << 8);
                    s.vif.tops = s.tops;
                    const uint32_t used = ps2x::native::vifUnpack(s.vif, reinterpret_cast<uint8_t *>(s.vu.words), sizeof(s.vu.words), code,
                                                                  reinterpret_cast<const uint8_t *>(words + i), static_cast<uint32_t>((count - i) * 4u));
                    i += (used + 3u) / 4u;
                    continue;
                }
                switch (cmd)
                {
                case 0x01: s.cl = imm & 0xffu; s.wl = (imm >> 8) & 0xffu; if (s.wl == 0) s.wl = 1; break;
                case 0x02: s.offset = imm & 0x3ffu; s.tops = s.base; break;
                case 0x03: s.base = imm & 0x3ffu; s.tops = s.base; break;
                case 0x04: s.itops = imm & 0x3ffu; break; // ITOP
                case 0x05: s.vif.mode = imm & 3u; s.sawMode = true; break; // STMOD
                case 0x14: // MSCAL
                case 0x15: // MSCALF
                case 0x17: // MSCNT
                {
                    s.top = s.tops;
                    s.tops = s.tops == s.base ? s.base + s.offset : s.base;
                    // TMB_NATIVE_DUMP_MIRROR=<file>,<pc>: the adapter's copy of VU1 data memory at each MSCAL of that
                    // program and its MSCNTs (pc, TOP, then 16 KB), to compare with a PS2X_VU1_RECORD trace of VU1.
                    {
                        static FILE *s_mirror = nullptr;
                        static uint32_t s_mirrorPc = ~0u;
                        static bool s_following = false;
                        static const bool s_open = [&]
                        {
                            const char *value = std::getenv("TMB_NATIVE_DUMP_MIRROR");
                            if (!value || !*value)
                                return false;
                            std::string spec(value);
                            const size_t comma = spec.rfind(',');
                            if (comma == std::string::npos)
                                return false;
                            s_mirrorPc = static_cast<uint32_t>(std::strtoul(spec.c_str() + comma + 1, nullptr, 16));
                            s_mirror = std::fopen(spec.substr(0, comma).c_str(), "wb");
                            return s_mirror != nullptr;
                        }();
                        if (s_open)
                        {
                            if (cmd != 0x17)
                                s_following = imm * 8u == s_mirrorPc;
                            static uint32_t s_dumped = 0;
                            if (s_following && ++s_dumped <= 400u) // the first 400 runs
                            {
                                const uint32_t header[2] = {cmd == 0x17 ? 0xffffffffu : imm * 8u, s.top};
                                std::fwrite(header, sizeof(header), 1, s_mirror);
                                std::fwrite(s.vu.words, sizeof(s.vu.words), 1, s_mirror);
                                std::fflush(s_mirror);
                            }
                        }
                    }
                    if (cmd == 0x17 && s.check && s.checkSetup != 0u && s.mode != Mode::None)
                    {
                        // Every triangle the strip makes, the batches it flushes on the way (a TEX0 per sprite) too.
                        s.checkMine.clear();
                        s.particleFrom = triangles.size();
                        s.collecting = true;
                        strip(s, triangles);
                        s.collecting = false;
                        s.checkMine.insert(s.checkMine.end(), triangles.begin() + static_cast<std::ptrdiff_t>(std::min(s.particleFrom, triangles.size())),
                                           triangles.end());
                        s.checkVertices.clear();
                        s.checkMaterials.clear();
                        s.checkResumed = true;
                        ps2x::native::Vu1Sandbox &vu1 = effectsMode(s.mode) ? *s.checkEffects : *s.check;
                        vu1.resume(s.code, sizeof(s.code), checkMemory(s, effectsMode(s.mode)), sizeof(s.vu.words), s.top, s.itops);
                        vu1.gif().flush();
                        checkTranslation(s, s.checkMine.data(), s.checkMine.size());
                    }
                    else if (cmd == 0x17)
                        strip(s, triangles);
                    else
                    {
                        const uint32_t pc = imm * 8u;
                        // TMB_NATIVE_VU1_PCS=<pc>[,<pc>...] (hex): setups left on the real VU1 (with their vertex passes and
                        // MSCNTs): a PS2X_VU1_RECORD reference for a translation (a diagnostic).
                        static const std::vector<uint32_t> s_onVu1 = []
                        {
                            std::vector<uint32_t> pcs;
                            if (const char *value = std::getenv("TMB_NATIVE_VU1_PCS"))
                                for (char *end = nullptr; *value; value = *end ? end + 1 : end)
                                {
                                    pcs.push_back(static_cast<uint32_t>(std::strtoul(value, &end, 16)));
                                    if (end == value)
                                        break;
                                }
                            return pcs;
                        }();
                        if (!matches(kVertexPasses, s.code, pc))
                            s.setupOnVu1 = std::find(s_onVu1.begin(), s_onVu1.end(), pc) != s_onVu1.end();
                        // The translated programs in the menus too (their 3D: the car and driver previews, the 2D
                        // drawn by the same strip setups); TMB_NATIVE_MENUS=0 leaves the menus to VU1. A setup in
                        // TMB_NATIVE_VU1_PCS stays on VU1 even when translated (a PS2X_VU1_RECORD reference).
                        bool native = false;
                        if (s.setupOnVu1)
                            s.lastSetupNative = false;
                        else
                            native = decide(s.lastSetupNative, s.code, pc) && (inMatch() || menusNative());
                        // A program the adapter does not translate is not run at all (no VU1, no interpreter of its
                        // microcode): VU1 skips it and nothing draws it, said once per program so that it shows. Only
                        // TMB_NATIVE_VU1_PCS (a diagnostic) leaves a setup to VU1.
                        const bool active = (inMatch() || menusNative()) && !s.setupOnVu1;
                        noteDecision(pc, native || active);
                        if (!native)
                        {
                            flush(s, triangles);
                            s.mode = Mode::None; // its MSCNTs are skipped too
                            if (active)
                            {
                                static std::unordered_map<uint64_t, bool> s_reported;
                                uint32_t lower = 0;
                                if (pc + 8u <= sizeof(s.code))
                                    std::memcpy(&lower, s.code + pc, 4);
                                if (!s_reported[(static_cast<uint64_t>(pc) << 32) | lower])
                                {
                                    s_reported[(static_cast<uint64_t>(pc) << 32) | lower] = true;
                                    std::fprintf(stderr, "[tmb:native] VU1 program %#x (first word %08x) not translated: not drawn\n", pc, lower);
                                }
                            }
                            if (!matches(kVertexPasses, s.code, pc))
                            {
                                ++g_stats.objects;
                                ++g_stats.skipped;
                                static const bool s_logVu1 = std::getenv("TMB_NATIVE_LOG_VU1") != nullptr;
                                if (s_logVu1)
                                {
                                    static std::unordered_map<uint64_t, uint32_t> s_counts;
                                    uint32_t lower = 0;
                                    if (pc + 8u <= sizeof(s.code))
                                        std::memcpy(&lower, s.code + pc, 4);
                                    const uint64_t key = (static_cast<uint64_t>(pc) << 32 | lower) ^ (inMatch() ? 1ull << 63 : 0u);
                                    // TMB_NATIVE_DUMP_CODE=<dir>: the code image of each program's first call, as
                                    // <dir>/vu1_<pc>.bin (for tools/vu_disasm.py).
                                    static const char *s_dumpDir = std::getenv("TMB_NATIVE_DUMP_CODE");
                                    if (s_dumpDir && s_counts[key] == 0u)
                                    {
                                        char path[1024];
                                        std::snprintf(path, sizeof(path), "%s/vu1_%04x.bin", s_dumpDir, pc);
                                        if (FILE *file = std::fopen(path, "wb"))
                                        {
                                            std::fwrite(s.code, 1, sizeof(s.code), file);
                                            std::fclose(file);
                                        }
                                    }
                                    if (++s_counts[key] % 2000u == 1u)
                                        std::fprintf(stderr, "[tmb:native] on VU1 (%s): %#x (lower %#x) x%u, last setup %s\n", inMatch() ? "match" : "menu",
                                                     pc, lower, s_counts[key], s.lastSetupNative ? "native" : "VU1");
                                }
                            }
                        }
                        else
                        {
                            // A program VU1 skips: its draws are the next group of the GS stream (the VIF1
                            // interpreter numbers the programs it skips the same way).
                            flush(s, triangles);
                            ps2x::native::Renderer::instance().beginGroup(++s.group);
                            if (pc == 0x1d40u)
                                rain(s, triangles);
                            else if (pc == 0x0000u && !matches(kVertexPasses, s.code, pc))
                            {
                                // The effects image's constants (its 0x0): VU 4-7, which its programs keep in registers.
                                for (int j = 0; j < 4; ++j)
                                    for (int k = 0; k < 4; ++k)
                                        s.fxConst[j][k] = s.vu.f(4 + j, k);
                                s.mode = Mode::None;
                            }
                            else if (pc == 0x0cf8u)
                            {
                                ps2x::native::Renderer::instance().setKey(0);
                                s.mode = Mode::Beams;
                                s.checkMine.clear();
                                s.particleFrom = triangles.size();
                                s.collecting = s.check != nullptr;
                                beams(s, triangles);
                                s.collecting = false;
                            }
                            else if (pc == 0x1198u)
                            {
                                ps2x::native::Renderer::instance().setKey(0);
                                s.mode = Mode::Lightning;
                                s.checkMine.clear();
                                s.particleFrom = triangles.size();
                                s.collecting = s.check != nullptr;
                                lightning(s, triangles);
                                s.collecting = false;
                            }
                            else if (pc == 0x0a08u)
                            {
                                ps2x::native::Renderer::instance().setKey(0);
                                s.mode = Mode::HeatHaze;
                                s.checkMine.clear();
                                s.particleFrom = triangles.size();
                                s.collecting = s.check != nullptr;
                                heatHaze(s, triangles);
                                s.collecting = false;
                            }
                            else if (pc == 0x0798u)
                            {
                                ps2x::native::Renderer::instance().setKey(0);
                                s.mode = Mode::TexParticles;
                                s.checkMine.clear();
                                s.particleFrom = triangles.size();
                                s.collecting = s.check != nullptr;
                                texParticles(s, triangles, true);
                                s.collecting = false;
                            }
                            else if (pc == 0x1830u)
                            {
                                ps2x::native::Renderer::instance().setKey(0);
                                s.mode = Mode::Trails;
                                s.checkMine.clear();
                                s.particleFrom = triangles.size();
                                s.collecting = s.check != nullptr;
                                trails(s, triangles);
                                s.collecting = false;
                            }
                            else if (pc == 0x0118u)
                            {
                                ps2x::native::Renderer::instance().setKey(0);
                                s.mode = Mode::Streaks;
                                s.checkMine.clear();
                                s.particleFrom = triangles.size();
                                s.collecting = s.check != nullptr;
                                streaks(s, triangles);
                                s.collecting = false;
                            }
                            else if (pc == 0x2338u)
                            {
                                ps2x::native::Renderer::instance().setKey(0);
                                s.mode = Mode::LensFlares;
                                s.checkMine.clear();
                                s.particleFrom = triangles.size();
                                s.collecting = s.check != nullptr;
                                lensFlares(s, triangles);
                                s.collecting = false;
                            }
                            else if (pc == 0x0390u)
                            {
                                ps2x::native::Renderer::instance().setKey(0);
                                s.mode = Mode::Particles;
                                s.checkMine.clear();
                                s.particleFrom = triangles.size();
                                s.collecting = s.check != nullptr;
                                particles(s, triangles, true);
                                s.collecting = false;
                            }
                            else if (pc == 0x0000u)
                                vertexPass(s);
                            else if (pc == 0x0010u)
                                morphPass(s);
                            else if (pc == 0x0020u || pc == 0x0030u || pc == 0x0040u)
                                skinnedPass(s, pc == 0x0020u ? 3 : pc == 0x0030u ? 2 : 1);
                            else
                                setup(s, pc);
                            if (s.check && pc != 0x1d40u)
                            {
                                if (!matches(kVertexPasses, s.code, pc))
                                    s.checkSetup = pc;
                                s.checkVertices.clear();
                                s.checkMaterials.clear();
                                const bool effects = effectsMode(s.mode) || (pc == 0x0000u && !matches(kVertexPasses, s.code, pc));
                                ps2x::native::Vu1Sandbox &vu1 = effects ? *s.checkEffects : *s.check;
                                vu1.call(s.code, sizeof(s.code), checkMemory(s, effects), sizeof(s.vu.words), pc, s.top, s.itops);
                                vu1.gif().flush();
                                // The particles' MSCAL draws (the setups do not): checked like a strip.
                                s.checkResumed = false;
                                if ((s.mode == Mode::Particles && pc == 0x0390u) || (s.mode == Mode::LensFlares && pc == 0x2338u) ||
                                    (s.mode == Mode::Streaks && pc == 0x0118u) || (s.mode == Mode::Trails && pc == 0x1830u) ||
                                    (s.mode == Mode::TexParticles && pc == 0x0798u) || (s.mode == Mode::HeatHaze && pc == 0x0a08u) ||
                                    (s.mode == Mode::Lightning && pc == 0x1198u) || (s.mode == Mode::Beams && pc == 0x0cf8u))
                                {
                                    s.checkMine.insert(s.checkMine.end(), triangles.begin() + static_cast<std::ptrdiff_t>(std::min(s.particleFrom, triangles.size())),
                                                       triangles.end());
                                    checkTranslation(s, s.checkMine.data(), s.checkMine.size());
                                }
                            }
                        }
                    }
                    break;
                }
                case 0x20: // STMASK
                    s.sawMask = true;
                    if (i < count)
                        s.vif.mask = words[i];
                    i += 1;
                    break;
                case 0x30: // STROW
                case 0x31: // STCOL
                    (cmd == 0x30 ? s.sawRow : s.sawCol) = true;
                    if (i + 4u <= count)
                        std::memcpy(cmd == 0x30 ? s.vif.row : s.vif.col, words + i, 16);
                    i += 4;
                    break;
                case 0x4a: // MPG: keep the code image, to know which program an MSCAL starts
                {
                    const size_t size = static_cast<size_t>(num ? num : 256u) * 2u;
                    const size_t at = static_cast<size_t>(imm) * 8u, bytes = std::min(size, count - i) * 4u;
                    if (at < sizeof(s.code))
                        std::memcpy(s.code + at, words + i, std::min(bytes, sizeof(s.code) - at));
                    i += size;
                    break;
                }
                case 0x50: case 0x51: // DIRECT: drawn here when it only draws (particles), else the GS draws it
                {
                    const size_t size = static_cast<size_t>(imm ? imm : 65536u) * 4u;
                    const size_t have = std::min(size, count - i);
                    readDirect(s, words + i, have / 4u);
                    const bool native = s.direct && inMatch() && have == size && drawsOnly(words + i, have / 4u);
                    // TMB_NATIVE_LOG_DIRECT=1: the A+D addresses of some DIRECT packets in a match.
                    static const bool s_logDirect = std::getenv("TMB_NATIVE_LOG_DIRECT") != nullptr;
                    static uint32_t s_directLogged = 0;
                    if (s_logDirect && inMatch() && ++s_directLogged % 200u == 1u && s_directLogged < 20000u)
                    {
                        std::string list;
                        for (size_t q = 1; q < have / 4u && list.size() < 300u; ++q)
                        {
                            char item[24];
                            std::snprintf(item, sizeof(item), " %02x", words[i + q * 4 + 2] & 0xffu);
                            list += item;
                        }
                        std::fprintf(stderr, "[tmb:native] DIRECT %zu qw, tag %08x %08x %08x %08x, native %d, A+D:%s\n", have / 4u, words[i], words[i + 1],
                                     words[i + 2], words[i + 3], native ? 1 : 0, list.c_str());
                    }
                    noteDecision(imm ? imm : 65536u, native, true);
                    if (native)
                    {
                        flush(s, triangles);
                        ps2x::native::Renderer::instance().beginGroup(++s.group);
                        ps2x::native::Renderer::instance().setKey(0);
                        s.direct->setSource(0xd1ec7u);
                        s.direct->packet(reinterpret_cast<const uint8_t *>(words + i), have * 4u);
                        s.direct->flush();
                    }
                    else if (s.direct)
                        s.direct->registers(reinterpret_cast<const uint8_t *>(words + i), have * 4u);
                    i += size;
                    break;
                }
                default: break;
                }
            }
            flush(s, triangles);
        }

        // TMB draws a 640 x 256 field in PAL, 640 x 224 in NTSC (it shows each one twice as tall): the screen is that
        // many lines below 2048 - 128 (2048 - 112).
        ps2x::native::Screen screen()
        {
            ps2x::native::Screen screen;
#if defined(TMB_REGION_US)
            screen.originY = 2048.0f - 112.0f; // NTSC: a 640 x 224 field
            screen.height = 224.0f;
#else
            screen.originY = 2048.0f - 128.0f;
            screen.height = 256.0f;
#endif
            // The microprograms write Z as FTOI4 of z / w, but into XYZF2 (the strips, the sprites: they carry fog),
            // whose 24-bit Z is that word over 16: GS Z is z / w itself. With 16, the adapter's scenery sat 16 times
            // nearer than the effects drawn over it, and hid most of them.
            screen.gsZScale = 1.0f;
            return screen;
        }

        State &state()
        {
            static State s;
            return s;
        }
    }

    void setGuestMemory(const uint8_t *rdram)
    {
        g_rdram = rdram;
    }

    bool enabled()
    {
        static const bool on = []
        {
            const char *value = std::getenv("TMB_NATIVE");
            return value && *value == '1';
        }();
        return on;
    }

    namespace
    {
        void onVif1(const uint8_t *stream, size_t bytes)
        {
            State &s = state();
            if (const VIFRegisters *regs = ps2x::native::vif1Registers())
            {
                if (!s.sawMask)
                    s.vif.mask = regs->mask;
                if (!s.sawRow)
                    std::memcpy(s.vif.row, regs->row, sizeof(s.vif.row));
                if (!s.sawCol)
                    std::memcpy(s.vif.col, regs->col, sizeof(s.vif.col));
                if (!s.sawMode)
                    s.vif.mode = regs->mode;
            }
            runVif(s, reinterpret_cast<const uint32_t *>(stream), bytes / 4u);
            ++g_stats.chains;
        }

    }

    void install()
    {
        if (!enabled())
            return;
        ps2x::native::Renderer::instance().setEnabled(true);
        ps2x::native::Renderer::instance().beginFrame(screen());
        ps2x::native::setVif1Observer(&onVif1);
        // TMB_NATIVE_VU1=1 keeps VU1 running them too (to compare with the GS frame).
        if (const char *keep = std::getenv("TMB_NATIVE_VU1"); !(keep && *keep == '1'))
        {
            state().direct = std::make_unique<ps2x::native::GifToNative>();
            if (checkLimit() > 0)
            {
                State &st = state();
                st.check = std::make_unique<ps2x::native::Vu1Sandbox>();
                st.check->gif().setCapture(&st.checkVertices, &st.checkMaterials);
                st.checkEffects = std::make_unique<ps2x::native::Vu1Sandbox>();
                st.checkEffects->gif().setCapture(&st.checkVertices, &st.checkMaterials);
            }
            ps2x::native::setVu1Gate(&drawnNatively);
            ps2x::native::setDirectGate(&directDrawnNatively);
            // In the GS stream where VU1's output would have been (with PS2X_GS_RENDERER=native: particles, the 2D
            // and the other GS draws sort with the 3D); with other GS renderers present() draws over the GS frame.
            ps2x::native::Renderer::instance().setStreamDraws(true);
        }
    }

    namespace
    {
        // Publishes what was submitted and starts the next frame.
        void publish()
        {
            ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
            static uint32_t s_frames = 0;
            if (++s_frames % 250u == 0u)
            {
                const State &s = state();
                std::fprintf(stderr, "[tmb:native] per 250 frames: %llu chains, %llu objects (%llu not native), %llu strips, %llu triangles, %llu textured batches; matrix col3 (%.1f %.1f %.1f %.1f)\n",
                             static_cast<unsigned long long>(g_stats.chains), static_cast<unsigned long long>(g_stats.objects),
                             static_cast<unsigned long long>(g_stats.skipped), static_cast<unsigned long long>(g_stats.strips),
                             static_cast<unsigned long long>(g_stats.triangles), static_cast<unsigned long long>(g_stats.textured),
                             s.matrix[12], s.matrix[13], s.matrix[14], s.matrix[15]);
                g_stats = Stats{};
            }
            renderer.endFrame();
            renderer.beginFrame(screen());
            state().keysThisFrame.clear();
        }
    }

    // TMB's lens flares (particleLensFlareTestZbuffer) read the GS Z buffer back around each light (a StoreImage of a
    // few pixels) to see whether something hides it; the native frame is not in the GS Z buffer, so in a match the
    // samples are replaced by the native frame's depth there (from the frame presented last).
    void patchFlareDepth(uint8_t *rdram, uint32_t image, uint32_t dest)
    {
        // TMB_NATIVE_FLARE_DEPTH=0: the flares keep the GS's samples (no depth read back from the native frame).
        // With the native GS renderer (PS2X_GS_RENDERER=native, the default) the native frame is drawn into the GS's
        // own targets, Z buffer included, so the GS's samples are right as they are; the native renderer's depth copy
        // (present()) is then never refreshed, and its stale values made headlight flares shine through buildings.
        static const bool s_on = []
        {
            const char *value = std::getenv("TMB_NATIVE_FLARE_DEPTH");
            const char *gs = std::getenv("PS2X_GS_RENDERER");
            if (value && *value)
                return *value != '0';
            return !(gs && std::string_view(gs) == "native");
        }();
        // TMB_NATIVE_LOG_FLARES=1: what the GS read back for the flares (format, size, the first samples), whether or not
        // they are then replaced, every 50th read.
        static const bool s_logGs = std::getenv("TMB_NATIVE_LOG_FLARES") != nullptr;
        if (s_logGs)
        {
            static uint32_t s_reads = 0;
            if (++s_reads % 50u == 1u)
            {
                uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
                for (uint32_t q = 0; q < 7u; ++q)
                {
                    uint64_t value;
                    std::memcpy(&value, rdram + ((image + q * 16u) & 0x1ffffffu), 8);
                    const uint8_t address = rdram[(image + q * 16u + 8u) & 0x1ffffffu];
                    if (address == 0x50u)
                        bitbltbuf = value;
                    else if (address == 0x51u)
                        trxpos = value;
                    else if (address == 0x52u)
                        trxreg = value;
                }
                uint32_t first[4];
                std::memcpy(first, rdram + (dest & 0x1ffffffu), sizeof(first));
                std::fprintf(stderr, "[tmb:flare] GS read sbp %#llx psm %#llx at (%llu, %llu) %llux%llu: %08x %08x %08x %08x%s\n",
                             static_cast<unsigned long long>(bitbltbuf & 0x3fffu), static_cast<unsigned long long>((bitbltbuf >> 24) & 0x3fu),
                             static_cast<unsigned long long>(trxpos & 0x7ffu), static_cast<unsigned long long>((trxpos >> 16) & 0x7ffu),
                             static_cast<unsigned long long>(trxreg & 0xfffu), static_cast<unsigned long long>((trxreg >> 32) & 0xfffu),
                             first[0], first[1], first[2], first[3], s_on ? " (then replaced)" : "");
            }
        }
        if (!s_on || !enabled() || !inMatch())
            return;
        ps2x::native::Renderer &renderer = ps2x::native::Renderer::instance();
        renderer.wantDepth();
        uint64_t bitbltbuf = 0, trxpos = 0, trxreg = 0;
        for (uint32_t q = 0; q < 7u; ++q) // the sceGsStoreImage packet: its A+D writes
        {
            uint64_t value;
            std::memcpy(&value, rdram + ((image + q * 16u) & 0x1ffffffu), 8);
            const uint8_t address = rdram[(image + q * 16u + 8u) & 0x1ffffffu];
            if (address == 0x50u)
                bitbltbuf = value;
            else if (address == 0x51u)
                trxpos = value;
            else if (address == 0x52u)
                trxreg = value;
        }
        const uint32_t psm = (bitbltbuf >> 24) & 0x3fu, x0 = trxpos & 0x7ffu, y0 = (trxpos >> 16) & 0x7ffu;
        const uint32_t width = trxreg & 0xfffu, height = (trxreg >> 32) & 0xfffu;
        const bool sixteen = psm == 0x32u || psm == 0x3au;
        if (width == 0u || height == 0u || width * height > 256u || (!sixteen && psm != 0x30u && psm != 0x31u))
            return;
        const ps2x::native::Screen view = screen();
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                uint32_t z = 0;
                if (!renderer.depthAt(view.originX + static_cast<float>(x0 + x) + 0.5f, view.originY + static_cast<float>(y0 + y) + 0.5f, z))
                    return;
                const uint32_t at = (dest + (y * width + x) * (sixteen ? 2u : 4u)) & 0x1ffffffu;
                // TMB_NATIVE_LOG_FLARES=1: some samples, the GS's then the native one.
                static const bool s_log = std::getenv("TMB_NATIVE_LOG_FLARES") != nullptr;
                static uint32_t s_logged = 0;
                if (s_log && x == 0u && y == 0u && ++s_logged % 50u == 1u)
                {
                    uint32_t gs = 0;
                    std::memcpy(&gs, rdram + at, sixteen ? 2u : 4u);
                    std::fprintf(stderr, "[tmb:native] flare sample at (%u, %u) %ux%u psm %#x: GS %#x, native %#x\n", x0, y0, width, height, psm,
                                 gs, z);
                }
                if (sixteen)
                {
                    const uint16_t z16 = static_cast<uint16_t>(std::min<uint32_t>(z >> 8, 0xffffu));
                    std::memcpy(rdram + at, &z16, 2);
                }
                else
                {
                    const uint32_t z32 = psm == 0x31u ? std::min<uint32_t>(z, 0xffffffu) : z;
                    std::memcpy(rdram + at, &z32, 4);
                }
            }
    }

    // The end of a frame's real-time processing (a match frame): publish what was submitted, start the next frame.
    void onFrameEnd()
    {
        if (!enabled())
            return;
        g_lastMatchFrame = hostSeconds();
        publish();
    }

    // A native frame end (timer 1 or sceGsSyncV) outside a match: the end of a menu frame; publishes what was drawn
    // of it (empty with TMB_NATIVE_MENUS=0, so the last match frame does not stay over a menu).
    void onMenuFrameEnd()
    {
        if (!enabled() || inMatch())
            return;
        g_lastMenuFrame = hostSeconds();
        publish();
    }
}
