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
            default: return Mode::None; // other programs (0x20d0, 0x1db8): not native yet
            }
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
            ++g_stats.objects;
            g_stats.skipped += s.mode == Mode::None ? 1u : 0u;
            // M = camera (VU 9-12) x object (VU 0-3): column j of M is camera applied to column j of the object's.
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    s.matrix[j * 4 + k] = s.vu.f(9, k) * s.vu.f(j, 0) + s.vu.f(10, k) * s.vu.f(j, 1) +
                                          s.vu.f(11, k) * s.vu.f(j, 2) + s.vu.f(12, k) * s.vu.f(j, 3);
            s.haveTexture = false;
        }

        void vertexPass(State &s)
        {
            const uint32_t count = s.vu.w(180, 3) & 0xffffu;
            s.pool.resize(static_cast<size_t>(count) * 3u);
            for (uint32_t i = 0; i < count; ++i)
                for (int k = 0; k < 3; ++k)
                    s.pool[i * 3u + k] = s.vu.f(181 + i, k);
        }

        void flush(State &s, std::vector<ps2x::native::Vertex> &triangles);

        // A strip's or sprite's material: its texture (an A+D block at 149 + header, TEX0 among it, when it brings one)
        // and whether it blends (PRIM.ABE in its GIF tag). A new material ends the batch so far. False when the GIF tag
        // is not an STQ / RGBAQ / XYZF2 one.
        bool useMaterial(State &s, uint32_t top, std::vector<ps2x::native::Vertex> &triangles)
        {
            const uint32_t tag1 = s.vu.w(top, 1), tag2 = s.vu.w(top, 2);
            if (tag2 != 0x412u)
                return false;
            uint64_t tex0 = s.tex0;
            bool textured = s.haveTexture;
            const uint32_t header = s.vu.w(top, 3);
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
            const bool blended = ((tag1 >> 15) & 0x40u) != 0u;
            if (tex0 != s.tex0 || textured != s.haveTexture || blended != s.blended)
            {
                flush(s, triangles);
                s.tex0 = tex0;
                s.haveTexture = textured;
                s.blended = blended;
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
            if (!useMaterial(s, top, triangles))
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
            if (!useMaterial(s, top, triangles))
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
            }
            const int order[6] = {0, 1, 2, 1, 2, 3};
            for (int i : order)
                triangles.push_back(quad[i]);
        }

        void strip(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            if (s.mode == Mode::None)
                return;
            if (s.mode == Mode::Sprite || s.mode == Mode::Ribbon)
            {
                (s.mode == Mode::Sprite ? sprite : ribbon)(s, triangles);
                return;
            }
            ++g_stats.strips;
            const uint32_t top = s.top;
            const uint32_t tag0 = s.vu.w(top, 0);
            if (!useMaterial(s, top, triangles))
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

        void flush(State &s, std::vector<ps2x::native::Vertex> &triangles)
        {
            if (triangles.empty())
                return;
            g_stats.triangles += triangles.size() / 3u;
            g_stats.textured += s.haveTexture ? 1u : 0u;
            ps2x::native::Material material;
            material.textured = s.haveTexture;
            material.tex0 = s.tex0;
            material.blend = s.blended ? ps2x::native::Material::Blend::Alpha : ps2x::native::Material::Blend::Opaque;
            static const float kScreenSpace[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            ps2x::native::Renderer::instance().draw(material, s.mode == Mode::Sprite || s.mode == Mode::Ribbon ? kScreenSpace : s.matrix, triangles.data(),
                                                    static_cast<uint32_t>(triangles.size()));
            triangles.clear();
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
                        if (pc == 0x0000u)
                            vertexPass(s);
                        else
                        {
                            flush(s, triangles);
                            setup(s, pc);
                        }
                    }
                    break;
                }
                case 0x20: i += 1; break;                                  // STMASK
                case 0x30: case 0x31: i += 4; break;                       // STROW / STCOL
                case 0x4a: i += static_cast<size_t>(num ? num : 256u) * 2u; break; // MPG
                case 0x50: case 0x51: i += static_cast<size_t>(imm ? imm : 65536u) * 4u; break; // DIRECT: the GS front end sees it
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

        // The hier microprograms this adapter draws, recognised by the lower instruction at each entry: the vertex
        // pass, the four strip setups, the sprites' and the ribbons'. VU1 skips them (and the strips that continue them); the others still run.
        constexpr struct
        {
            uint32_t pc, lower;
        } kNativePrograms[] = {{0x0000u, 0x400001d4u}, {0x0cc8u, 0x420f01e7u}, {0x0ce8u, 0x420f01e3u},
                               {0x0d08u, 0x420f01dfu}, {0x1700u, 0x420f00a0u}, {0x3000u, 0x420f0580u},
                               {0x3270u, 0x420f0532u}};

        bool drawnNatively(const uint8_t *vu1Code, uint32_t startPc)
        {
            for (const auto &program : kNativePrograms)
                if (program.pc == startPc)
                {
                    uint32_t lower;
                    std::memcpy(&lower, vu1Code + startPc, 4);
                    return lower == program.lower;
                }
            return false;
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
            ps2x::native::setVu1Gate(&drawnNatively);
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
    }
}
