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

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
            Ribbon, // screen-wide quads between two points (trails): 0x3270, one per MSCNT
            Rain,   // the effects image's rain lines (0x1d40)
            SkinDirectional, // skinned strips (0x640, after setup 0x50 / 0x70): directional light on the pool's normals
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
            case 0x0050:
            case 0x0070: return Mode::SkinDirectional;
            default: return Mode::None; // other programs (0x20d0, 0x1db8): not native yet
            }
        }

        // The microprograms this adapter draws, recognised by the lower instruction at each entry (so only while their
        // code image is loaded). Setups (and programs that stand alone): the hier image's four strip setups, its two
        // skinned setups (0x50, 0x70: strips at 0x640), sprites and ribbons, and the effects image's rain. Vertex
        // passes (0x00 rigid, 0x10 morph, 0x20 / 0x30 / 0x40 skinned with three, two, one bones) run after a setup
        // and are drawn natively exactly when that setup is: a VU1 setup keeps its VU1 vertex pass. VU1 skips what is
        // drawn here (and the MSCNTs that continue it); the others still run.
        struct Program
        {
            uint32_t pc, lower;
        };
        constexpr Program kSetups[] = {{0x0cc8u, 0x420f01e7u}, {0x0ce8u, 0x420f01e3u}, {0x0d08u, 0x420f01dfu},
                                       {0x1700u, 0x420f00a0u}, {0x0050u, 0x420f0376u}, {0x0070u, 0x420f0372u},
                                       {0x3000u, 0x420f0580u}, {0x3270u, 0x420f0532u}, {0x1d40u, 0x420f07deu}};
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
            if (matches(kVertexPasses, vu1Code, startPc))
                return lastSetupNative;
            lastSetupNative = matches(kSetups, vu1Code, startPc);
            return lastSetupNative;
        }

        bool drawnNatively(const uint8_t *vu1Code, uint32_t startPc)
        {
            static bool s_lastSetupNative = false; // the VIF1 thread's
            return decide(s_lastSetupNative, vu1Code, startPc);
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

        struct State
        {
            VuImage vu;
            uint32_t cl = 1, wl = 1;   // STCYCL
            uint32_t base = 0, offset = 0, tops = 0, top = 0;
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
            uint32_t fogColour = 0;        // FOGCOL as the stream's DIRECT packets set it
            uint32_t batchFogColour = 0;
        };

        struct Stats
        {
            uint64_t chains = 0, objects = 0, strips = 0, triangles = 0, textured = 0, skipped = 0;
        } g_stats;

        int32_t signExtend(uint32_t value, uint32_t bits)
        {
            const uint32_t shift = 32u - bits;
            return static_cast<int32_t>(value << shift) >> shift;
        }

        // UNPACK: num vectors of (vn + 1) components of vl bits, written every CL quadwords in groups of WL.
        void unpack(State &s, uint32_t cmd, uint32_t num, uint32_t imm, const uint32_t *data, size_t dataWords)
        {
            const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u;
            const bool usn = (imm >> 14) & 1u;
            uint32_t addr = imm & 0x3ffu;
            if ((imm >> 15) & 1u)
                addr += s.tops;
            const uint32_t n = num ? num : 256u;
            const uint32_t components = vn + 1u;
            const uint32_t bits = vl == 0 ? 32u : vl == 1 ? 16u : vl == 2 ? 8u : 5u;
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(data);
            const size_t available = dataWords * 4u;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t qw = s.cl >= s.wl ? addr + (i / s.wl) * s.cl + (i % s.wl) : addr + i;
                for (uint32_t c = 0; c < 4u; ++c)
                {
                    if (vl == 3u) // V4-5: one 16-bit word, 5:5:5:1
                    {
                        if ((i * 2u + 2u) > available)
                            return;
                        const uint32_t v = bytes[i * 2] | (bytes[i * 2 + 1] << 8);
                        const uint32_t field = c < 3u ? ((v >> (5u * c)) & 0x1fu) << 3 : ((v >> 15) & 1u) << 7;
                        s.vu.words[(qw & 0x3ffu) * 4 + c] = field;
                        continue;
                    }
                    if (c >= components)
                        continue; // V2 / V3: the lanes above are left (or masked by the game: TMB reads x, y only)
                    const size_t bit = (static_cast<size_t>(i) * components + c) * bits;
                    if ((bit + bits) / 8u > available)
                        return;
                    uint32_t raw = 0;
                    if (bits == 32u)
                        std::memcpy(&raw, bytes + bit / 8u, 4);
                    else if (bits == 16u)
                        raw = bytes[bit / 8u] | (bytes[bit / 8u + 1] << 8);
                    else
                        raw = bytes[bit / 8u];
                    s.vu.words[(qw & 0x3ffu) * 4 + c] = (bits == 32u || usn) ? raw : static_cast<uint32_t>(signExtend(raw, bits));
                }
                if (vn == 0u && vl != 3u) // S-xx fills all four lanes with the one value
                    for (uint32_t c = 1; c < 4u; ++c)
                        s.vu.words[(qw & 0x3ffu) * 4 + c] = s.vu.words[(qw & 0x3ffu) * 4];
            }
        }

        // The fog factor a microprogram writes in XYZF2's F, over 255: clamp(VU 4.w + VU 21.x + VU 21.w / w, 0, VU 21.y)
        // (the same in the strip, sprite and ribbon programs; checked on 3000 strip vertices of the match trace).
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
            if (s.mode == Mode::Unlit)
                for (int k = 0; k < 4; ++k)
                    c[k] = static_cast<float>(rgba[k]) + vu.f(20, k);
            else if (s.mode == Mode::Scaled)
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
            ps2x::native::Renderer::instance().setKey(0); // sprites and ribbons: screen space, not interpolated
            ++g_stats.objects;
            g_stats.skipped += s.mode == Mode::None ? 1u : 0u;
            // M = camera (VU 9-12) x object (VU 0-3): column j of M is camera applied to column j of the object's.
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    s.matrix[j * 4 + k] = s.vu.f(9, k) * s.vu.f(j, 0) + s.vu.f(10, k) * s.vu.f(j, 1) +
                                          s.vu.f(11, k) * s.vu.f(j, 2) + s.vu.f(12, k) * s.vu.f(j, 3);
            s.haveTexture = false;
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
        bool useMaterial(State &s, uint32_t tagQw, uint32_t header, std::vector<ps2x::native::Vertex> &triangles)
        {
            const uint32_t tag1 = s.vu.w(tagQw, 1), tag2 = s.vu.w(tagQw, 2);
            if (tag2 != 0x412u)
                return false;
            uint64_t tex0 = s.tex0;
            bool textured = s.haveTexture;
            if (header != 0u)
                for (uint32_t q = 0; q < 3u; ++q)
                {
                    const uint32_t qw = 149u + header + q;
                    if ((s.vu.w(qw, 2) & 0xffu) == 0x06u) // TEX0_1
                    {
                        tex0 = static_cast<uint64_t>(s.vu.w(qw, 0)) | (static_cast<uint64_t>(s.vu.w(qw, 1)) << 32);
                        textured = true;
                    }
                }
            const bool blended = ((tag1 >> 15) & 0x40u) != 0u, fogged = ((tag1 >> 15) & 0x20u) != 0u;
            if (tex0 != s.tex0 || textured != s.haveTexture || blended != s.blended || (blended && s.alpha != s.batchAlpha) ||
                fogged != s.fogged || (fogged && s.fogColour != s.batchFogColour))
            {
                flush(s, triangles);
                s.tex0 = tex0;
                s.haveTexture = textured;
                s.blended = blended;
                s.batchAlpha = s.alpha;
                s.fogged = fogged;
                s.batchFogColour = s.fogColour;
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

        // One segment of the 0x3270 program (its loop at 0x3290, re-entered at 0x3500): two points (VU top+1 and +4,
        // the half width in w), each drawn as two vertices 608 pixels per unit of width and of 1/w left and right of
        // it; vertex k takes its colour from top+2+3k (scaled like the scaled mode) and its texture coordinates from
        // top+3+3k (4.12, plus VU 4). In screen space like the sprites.
        void ribbon(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            ++g_stats.strips;
            const uint32_t top = s.top;
            if (!useMaterial(s, top, s.vu.w(top, 3), triangles))
                return;
            const VuImage &vu = s.vu;
            ps2x::native::Vertex quad[4];
            for (int k = 0; k < 4; ++k)
            {
                const uint32_t point = top + 1u + 3u * static_cast<uint32_t>(k / 2);
                float c[4];
                for (int j = 0; j < 4; ++j)
                    c[j] = s.matrix[j] * vu.f(point, 0) + s.matrix[4 + j] * vu.f(point, 1) + s.matrix[8 + j] * vu.f(point, 2) +
                           s.matrix[12 + j];
                if (c[3] <= 0.0f)
                    return; // behind the camera (VU1 clips it)
                const float q = 1.0f / c[3], half = 608.0f * vu.f(point, 3) * q;
                quad[k].x = c[0] * q + (k % 2 == 0 ? half : -half);
                quad[k].y = c[1] * q;
                quad[k].z = c[2] * q * vu.f(4, 2);
                const uint32_t colourQw = top + 2u + 3u * static_cast<uint32_t>(k), uvQw = colourQw + 1u;
                float rgba[4];
                for (int j = 0; j < 4; ++j)
                    rgba[j] = static_cast<float>(vu.w(colourQw, j)) * vu.f(5, j) + vu.f(20, j);
                quad[k].rgba = clampByte(rgba[0]) | (clampByte(rgba[1]) << 8) | (clampByte(rgba[2]) << 16) | (clampByte(rgba[3]) << 24);
                quad[k].u = static_cast<float>(static_cast<int32_t>(vu.w(uvQw, 0))) / 4096.0f + vu.f(4, 0);
                quad[k].v = static_cast<float>(static_cast<int32_t>(vu.w(uvQw, 1))) / 4096.0f + vu.f(4, 1);
                quad[k].fog = fogFactor(vu, q);
            }
            const int order[6] = {0, 1, 2, 1, 2, 3};
            for (int i : order)
                triangles.push_back(quad[i]);
        }

        // A skinned strip (0x640): with a texture header, VU memory holds an A+D GIF tag at TOP (the header in its w) and
        // the strip's tag at TOP + 4, else the strip's tag at TOP; vertex k at TOP + 17 + 3k: the pool offset (w), the
        // colour, the texture coordinates (4.12, plus VU 4). Directional light on the pool's blended normal:
        // colour * (VU 17-19 . max(0, VU 6-8 . n)) * VU 5 + VU 20. All 4005 checked vertices of the recorded match
        // (colour, screen position, texture coordinates) match VU1.
        void skinnedStrip(State &s, std::vector<ps2x::native::Vertex> &triangles)
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
                    c[k2] = static_cast<float>(vu.w(q + 1, k2)) * (vu.f(17, k2) * d[0] + vu.f(18, k2) * d[1] + vu.f(19, k2) * d[2]) * vu.f(5, k2) +
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

        void strip(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            if (s.mode == Mode::None)
                return;
            if (s.mode == Mode::SkinDirectional)
            {
                skinnedStrip(s, triangles);
                return;
            }
            if (s.mode == Mode::Sprite || s.mode == Mode::Ribbon)
            {
                (s.mode == Mode::Sprite ? sprite : ribbon)(s, triangles);
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
            if (triangles.empty())
                return;
            g_stats.triangles += triangles.size() / 3u;
            g_stats.textured += s.haveTexture ? 1u : 0u;
            ps2x::native::Material material;
            material.textured = s.haveTexture;
            material.tex0 = s.tex0;
            material.blend = s.blended ? blendFor(s.batchAlpha) : ps2x::native::Material::Blend::Opaque;
            material.depthWrite = s.mode != Mode::Rain;
            material.fog = s.fogged && s.mode != Mode::Rain;
            material.fogColour = s.batchFogColour;
            static const float kScreenSpace[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            ps2x::native::Renderer::instance().draw(material, s.mode == Mode::Sprite || s.mode == Mode::Ribbon || s.mode == Mode::Rain ? kScreenSpace : s.matrix, triangles.data(),
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
            const uint32_t drops = std::min(vu.w(20, 3), 65536u);
            for (uint32_t n = 0; n < drops; ++n)
            {
                float h[4], t[4];
                transform(a, p, head, h);
                transform(b, p, tail, t);
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
                    quad[i].rgba = 0x00808080u | ((i < 2 ? static_cast<uint32_t>(alpha) : 1u) << 24);
                }
                const int order[6] = {0, 1, 2, 1, 2, 3};
                for (int i : order)
                    triangles.push_back(quad[i]);
            }
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
                if (flg == 0u)
                {
                    for (uint32_t loop = 0; loop < nloop && q < qwords; ++loop)
                        for (uint32_t r = 0; r < nreg && q < qwords; ++r, ++q)
                            if (((regs >> (4u * r)) & 0xfu) == 0xeu)
                            {
                                const uint64_t value = words[q * 4] | (static_cast<uint64_t>(words[q * 4 + 1]) << 32);
                                const uint32_t address = words[q * 4 + 2] & 0xffu;
                                if (address == 0x06u)
                                    s.directTex0 = value;
                                else if (address == 0x42u)
                                    s.alpha = value;
                                else if (address == 0x3du)
                                    s.fogColour = static_cast<uint32_t>(value & 0xffffffu);
                            }
                }
                else if (flg == 1u)
                    q += (static_cast<size_t>(nloop) * nreg + 1u) / 2u;
                else
                    q += nloop;
            }
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
                    const uint32_t vn = (cmd >> 2) & 3u, vl = cmd & 3u, n = num ? num : 256u;
                    const uint32_t bits = vl == 3u ? 16u : (vn + 1u) * (vl == 0 ? 32u : vl == 1 ? 16u : 8u);
                    const size_t size = (static_cast<size_t>(n) * bits + 31u) / 32u;
                    unpack(s, cmd, num, imm, words + i, std::min(size, count - i));
                    i += size;
                    continue;
                }
                switch (cmd)
                {
                case 0x01: s.cl = imm & 0xffu; s.wl = (imm >> 8) & 0xffu; if (s.wl == 0) s.wl = 1; break;
                case 0x02: s.offset = imm & 0x3ffu; s.tops = s.base; break;
                case 0x03: s.base = imm & 0x3ffu; s.tops = s.base; break;
                case 0x14: // MSCAL
                case 0x15: // MSCALF
                case 0x17: // MSCNT
                {
                    s.top = s.tops;
                    s.tops = s.tops == s.base ? s.base + s.offset : s.base;
                    if (cmd == 0x17)
                        strip(s, triangles);
                    else
                    {
                        const uint32_t pc = imm * 8u;
                        if (!decide(s.lastSetupNative, s.code, pc))
                        {
                            if (!matches(kVertexPasses, s.code, pc))
                            {
                                flush(s, triangles);
                                s.mode = Mode::None; // another program, or another code image's: still on VU1
                                ++g_stats.objects;
                                ++g_stats.skipped;
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
                            else if (pc == 0x0000u)
                                vertexPass(s);
                            else if (pc == 0x0010u)
                                morphPass(s);
                            else if (pc == 0x0020u || pc == 0x0030u || pc == 0x0040u)
                                skinnedPass(s, pc == 0x0020u ? 3 : pc == 0x0030u ? 2 : 1);
                            else
                                setup(s, pc);
                        }
                    }
                    break;
                }
                case 0x20: i += 1; break;                                  // STMASK
                case 0x30: case 0x31: i += 4; break;                       // STROW / STCOL
                case 0x4a: // MPG: keep the code image, to know which program an MSCAL starts
                {
                    const size_t size = static_cast<size_t>(num ? num : 256u) * 2u;
                    const size_t at = static_cast<size_t>(imm) * 8u, bytes = std::min(size, count - i) * 4u;
                    if (at < sizeof(s.code))
                        std::memcpy(s.code + at, words + i, std::min(bytes, sizeof(s.code) - at));
                    i += size;
                    break;
                }
                case 0x50: case 0x51: // DIRECT: the GS front end draws it; the adapter only notes its TEX0
                {
                    const size_t size = static_cast<size_t>(imm ? imm : 65536u) * 4u;
                    readDirect(s, words + i, std::min(size, count - i) / 4u);
                    i += size;
                    break;
                }
                default: break;
                }
            }
            flush(s, triangles);
        }

        // TMB draws a 640 x 256 field (it shows each one twice as tall): the screen is 256 lines below 2048 - 128.
        ps2x::native::Screen screen()
        {
            ps2x::native::Screen screen;
            screen.originY = 2048.0f - 128.0f;
            screen.height = 256.0f;
            screen.gsZScale = 16.0f; // the microprograms write Z as FTOI4 of z / w
            return screen;
        }

        State &state()
        {
            static State s;
            return s;
        }
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
            runVif(state(), reinterpret_cast<const uint32_t *>(stream), bytes / 4u);
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
            ps2x::native::setVu1Gate(&drawnNatively);
            // In the GS stream where VU1's output would have been (with PS2X_GS_RENDERER=native: particles, the 2D
            // and the other GS draws sort with the 3D); with other GS renderers present() draws over the GS frame.
            ps2x::native::Renderer::instance().setStreamDraws(true);
        }
    }

    // The end of a frame's real-time processing: publish what was submitted, start the next frame.
    void onFrameEnd()
    {
        if (!enabled())
            return;
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
