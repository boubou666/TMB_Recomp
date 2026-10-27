#!/usr/bin/env python3
"""Rebuilds TMB's 3D objects from a PS2X_VU1_RECORD trace as plain meshes (the native PC port's renderer input), and
checks them against the GS vertices VU1 really produced.

  tools/tmb_mesh_decode.py <trace> check [first-record [count]]   decode objects, compare with VU1's GS output
  tools/tmb_mesh_decode.py <trace> render <out.png> [first [count]] draw the decoded triangles (untextured)
  tools/tmb_mesh_decode.py <trace> render-vu1-colours <out.png> ...  the same with VU1's colours (geometry check)

Per object (code image 97bcf402, docs/RECOMP_NOTES.md "Native PC port"): a vertex pass (MSCAL 0x0000) projects the
pool at VU 181.. (count in 180.w) with the matrix in vf1-vf4; strip batches (MSCNT) hold, after their GIF tag, three
quadwords per vertex: normal (unsigned bytes) with the pool index in w, colour (RGBA), texture coordinates (4.12).
VU1 outputs STQ = (s + u0, t + v0, 1) * 1/w, XYZ = (M * p) / w in GS 12.4, and RGBA by one of four shading modes:
  unlit      colour + ambient(vf11)                                         (0x1368, resumed at 0x10a8)
  scaled     colour * scale(vf31) + ambient(vf11)                           (0x1098, 0x0f60)
  directional colour * scale(vf31) * sum(light colour 17-19 * max(0, n . dir 6-8)) + ambient(vf11), n = 1 - v / 128
                                                                            (0x16f0, 0x1378)
  point      colour * VU5 + ambient(vf29) + three spot lights (169-175)     (0x1ad0, 0x1748), n = (v - 128) / 127
             (92.7% of vertices exact, the rest within a few levels: the light term is not exact yet)
0x0f50 resumes into whichever of them the previous batch left in vi13.
"""
import math
import struct
import sys

sys.path.insert(0, __file__.rsplit('\\', 1)[0].rsplit('/', 1)[0])
import vu1_trace as T

IMAGE = '97bcf402'
MODES = {0x1368: 'unlit', 0x10a8: 'unlit', 0x1098: 'scaled', 0x0f60: 'scaled', 0x16f0: 'directional',
         0x1378: 'directional', 0x1ad0: 'point', 0x1748: 'point'}


def s32(x):
    return x - 0x100000000 if x & 0x80000000 else x


class Strip:
    """One triangle strip: its shading mode, the constants that mode reads, and its vertices
    (pool index, raw normal bytes, rgba, (s, t))."""

    def __init__(self, mode, r):
        vf = r.before.vf
        self.mode = mode
        self.uv_offset = vf[19][:2]
        if mode == 'point':
            self.uv_offset = r.qf(4)[:2]  # this loop reloads vf19 from VU 4 before adding it
            self.scale, self.ambient = r.qf(5), vf[29]  # the loop reloads vf17 from VU 5
            self.lights = (vf[9], vf[10], vf[11])  # x, y, z of the three lights (transposed)
            self.spot = (r.qf(165), r.qf(166), r.qf(167), r.qf(168))
            self.light_colours = [r.qf(172 + i) for i in range(3)]
            self.light_range = r.qf(175)
        else:
            self.scale, self.ambient = vf[31], vf[11]
            self.dirs = (r.qf(6), r.qf(7), r.qf(8))
            self.light_colours = [r.qf(17), r.qf(18), r.qf(19)]
        self.vertices = []

    def shade(self, normal_bytes, rgba, pos):
        if self.mode == 'unlit':
            return [min(255, int(rgba[k] + self.ambient[k])) for k in range(4)]
        if self.mode == 'scaled':
            return [min(255, int(rgba[k] * self.scale[k] + self.ambient[k])) for k in range(4)]
        if self.mode == 'directional':
            n = [1 - (c & 0xff) / 128 for c in normal_bytes]
            d = [max(0.0, self.dirs[0][i] * n[0] + self.dirs[1][i] * n[1] + self.dirs[2][i] * n[2]) for i in range(3)]
            c = self.light_colours
            light = [(c[0][k] * d[0] + c[1][k] * d[1] + c[2][k] * d[2]) * self.scale[k] for k in range(3)] + [self.scale[3]]
            return [min(255, int(rgba[k] * light[k] + self.ambient[k])) for k in range(4)]
        # point lights
        n = [(c & 0xff) / 127 - 128 / 127 for c in normal_bytes]
        col = [rgba[k] * self.scale[k] + self.ambient[k] for k in range(4)]
        LX, LY, LZ = self.lights
        D0, D1, D2, CUT = self.spot
        for i in range(3):
            d = (-(LX[i] + pos[0]), -(LY[i] + pos[1]), -(LZ[i] + pos[2]))  # vertex to light
            d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2]
            if d2 <= 0:
                continue
            inv = 1 / math.sqrt(d2)
            facing = max(0.0, (d[0] * n[0] + d[1] * n[1] + d[2] * n[2]) * inv)
            att = min(0.5, facing * inv * inv * self.light_range[i])
            cone = max(0.0, (d[0] * D0[i] + d[1] * D1[i] + d[2] * D2[i]) * inv + CUT[i]) ** 4
            for k in range(3):
                col[k] += self.light_colours[i][k] * att * cone
        return [min(255, int(c)) for c in col]


class Mesh:
    """One object: its pool (positions in the object's space), the object matrix, and its strips."""

    def __init__(self, pool, matrix):
        self.pool, self.matrix = pool, matrix
        self.strips = []

    def project(self, i):
        p, M = self.pool[i], self.matrix
        c = [M[0][k] * p[0] + M[1][k] * p[1] + M[2][k] * p[2] + M[3][k] for k in range(4)]
        q = 1.0 / c[3]
        return c[0] * q, c[1] * q, c[2] * q, q


def objects(path, first=0, count=None):
    """Yields (Mesh, [(strip, gs_vertices)]) per object; gs_vertices are VU1's (S, T, Q, R, G, B, A, X, Y, Z)."""
    mesh = None
    batches = []
    for r in T.records(path):
        if r.index < first:
            continue
        if count is not None and r.index >= first + count:
            break
        if r.code_hash != IMAGE:
            continue
        if not r.resume and r.start_pc == 0x0000:
            if mesh is not None:
                yield mesh, batches
            n = r.qw(180)[3] & 0xffff
            vf = r.before.vf
            mesh = Mesh([r.qf(181 + i)[:3] for i in range(n)], [vf[1], vf[2], vf[3], vf[4]])
            batches = []
            continue
        if not r.resume or mesh is None or not r.packets:
            continue
        pc = r.before.vi[13] * 8 if r.start_pc == 0x0f50 else r.start_pc
        mode = MODES.get(pc)
        top = r.top
        tag = r.qw(top)
        nloop = tag[0] & 0x7fff
        if mode is None or tag[2] != 0x412 or nloop == 0:
            continue  # another program, or not a STQ / RGBAQ / XYZF2 strip
        strip = Strip(mode, r)
        for v in range(nloop):
            q0, q1, q2 = r.qw(top + 1 + 3 * v), r.qw(top + 2 + 3 * v), r.qw(top + 3 + 3 * v)
            strip.vertices.append((q0[3] & 0x7fff, q0[:3], q1, (s32(q2[0]) / 4096.0, s32(q2[1]) / 4096.0)))
        p = r.packets[0]
        off = next((i for i in range(0, len(p), 16) if struct.unpack_from('<3I', p, i) == tag[:3]), None)
        gs = []
        if off is not None:
            for v in range(nloop):
                base = off + 16 + v * 48
                if base + 48 > len(p):
                    break
                S, Tt, Q = struct.unpack_from('<3f', p, base)
                R, G, B, A = struct.unpack_from('<4I', p, base + 16)
                X, Y, Z = struct.unpack_from('<3I', p, base + 32)
                gs.append((S, Tt, Q, R, G, B, A, (X & 0xffff) / 16.0, (Y & 0xffff) / 16.0, Z))
        mesh.strips.append(strip)
        batches.append((strip, gs))
    if mesh is not None:
        yield mesh, batches


def check(path, first, count):
    import collections
    stats = collections.defaultdict(lambda: [0, 0, 0, 0])  # vertices, position, st, colour
    for mesh, batches in objects(path, first, count):
        for strip, gs in batches:
            s = stats[strip.mode]
            for (idx, normal, rgba, (u, v)), g in zip(strip.vertices, gs):
                if idx >= len(mesh.pool):
                    continue
                x, y, z, q = mesh.project(idx)
                s[0] += 1
                s[1] += max(abs(x - g[7]), abs(y - g[8])) < 1.0 / 16 + 1e-3
                S, Tt = (u + strip.uv_offset[0]) * q, (v + strip.uv_offset[1]) * q
                s[2] += abs(S - g[0]) <= 1e-6 + 1e-4 * abs(g[0]) and abs(Tt - g[1]) <= 1e-6 + 1e-4 * abs(g[1])
                c = strip.shade(normal, rgba, mesh.pool[idx])
                s[3] += all(abs(c[k] - g[3 + k]) <= 1 for k in range(4))
    for mode, (n, pos, st, col) in sorted(stats.items()):
        print(f'{mode:12s} {n:6d} vertices: position {pos / n:6.1%}  texture coordinates {st / n:6.1%}  colour {col / n:6.1%}')


def render(path, out, first, count, colours='decoded'):
    from PIL import Image, ImageDraw
    img = Image.new('RGB', (640, 512), (0, 0, 0))
    draw = ImageDraw.Draw(img)
    triangles = []
    for mesh, batches in objects(path, first, count):
        for strip, gs in batches:
            pts = []
            for v, (idx, normal, rgba, st) in enumerate(strip.vertices):
                if idx >= len(mesh.pool):
                    pts.append(None)
                    continue
                x, y, z, q = mesh.project(idx)
                lit = tuple(strip.shade(normal, rgba, mesh.pool[idx])[:3])
                if colours == 'vu1' and v < len(gs):
                    lit = gs[v][3:6]  # what VU1 computed, to look at the geometry alone
                pts.append((x - 2048 + 320, y - 2048 + 256, z, lit))
            for a, b, c in zip(pts, pts[1:], pts[2:]):
                # outside the GS guard band: a vertex behind the camera, which VU1's clipper handles
                if a and b and c and all(-1728 <= p[0] <= 2368 and -1792 <= p[1] <= 2304 for p in (a, b, c)):
                    triangles.append((a, b, c))
    triangles.sort(key=lambda tri: tri[0][2] + tri[1][2] + tri[2][2])  # GS depth grows toward the camera: far first
    for a, b, c in triangles:
        colour = tuple((a[3][k] + b[3][k] + c[3][k]) // 3 for k in range(3))
        draw.polygon([(a[0], a[1]), (b[0], b[1]), (c[0], c[1])], fill=colour)
    img.save(out)
    print(f'{len(triangles)} triangles -> {out}')


if __name__ == '__main__':
    path, mode = sys.argv[1], sys.argv[2]
    if mode == 'check':
        check(path, int(sys.argv[3]) if len(sys.argv) > 3 else 0, int(sys.argv[4]) if len(sys.argv) > 4 else None)
    elif mode in ('render', 'render-vu1-colours'):
        render(path, sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 0,
               int(sys.argv[5]) if len(sys.argv) > 5 else None, 'vu1' if mode.endswith('colours') else 'decoded')
