#!/usr/bin/env python3
"""Reads a PS2X_VU1_RECORD trace (the fork's ps2_vu1_trace.h format): one record per VU1 microprogram run, with the
VU1 data memory and registers before and after, the code image when it changed, and the GIF packets the run kicked.

  tools/vu1_trace.py <trace>            summary: runs per (code image, entry pc, MSCAL/MSCNT)

As a module: records(path) yields Record objects (code is the image in effect, shared between records).
"""
import collections, hashlib, struct, sys

MAGIC = 0x52315556
MEM = 16 * 1024
STATE = 664  # sizeof(VU1State): vf[32][4], vi[16], acc, q p i r pc mac clip status, cycles, flags, top, itop, branch


class State:
    def __init__(self, raw):
        self.vf = [struct.unpack_from('<4f', raw, i * 16) for i in range(32)]
        self.vi = list(struct.unpack_from('<16i', raw, 512))
        self.acc = struct.unpack_from('<4f', raw, 576)
        self.q, self.p, self.i = struct.unpack_from('<3f', raw, 592)
        self.r, self.pc, self.mac, self.clip, self.status = struct.unpack_from('<5I', raw, 604)
        self.top, self.itop = struct.unpack_from('<2I', raw, 640)


class Record:
    __slots__ = ('index', 'resume', 'start_pc', 'top', 'itop', 'code', 'code_hash', 'before', 'data_before', 'after',
                 'data_after', 'packets')

    def qw(self, addr, after=False):
        """The quadword at VU1 data address addr (in quadwords) as four 32-bit words."""
        data = self.data_after if after else self.data_before
        return struct.unpack_from('<4I', data, (addr & 0x3FF) * 16)

    def qf(self, addr, after=False):
        data = self.data_after if after else self.data_before
        return struct.unpack_from('<4f', data, (addr & 0x3FF) * 16)


def records(path):
    code, code_hash = None, None
    with open(path, 'rb') as f:
        index = 0
        while True:
            head = f.read(28)
            if len(head) < 28:
                return
            magic, resume, start_pc, top, itop, _max_cycles, has_code = struct.unpack('<7I', head)
            if magic != MAGIC:
                raise ValueError(f'bad magic at record {index}')
            if has_code:
                code = f.read(MEM)
                code_hash = hashlib.sha1(code).hexdigest()[:8]
            r = Record()
            r.index, r.resume, r.start_pc, r.top, r.itop = index, resume, start_pc, top, itop
            r.code, r.code_hash = code, code_hash
            r.before = State(f.read(STATE))
            r.data_before = f.read(MEM)
            r.after = State(f.read(STATE))
            r.data_after = f.read(MEM)
            (count,) = struct.unpack('<I', f.read(4))
            r.packets = []
            for _ in range(count):
                (size,) = struct.unpack('<I', f.read(4))
                r.packets.append(f.read(size))
            yield r
            index += 1


if __name__ == '__main__':
    runs = collections.Counter()
    kick_bytes = collections.Counter()
    for r in records(sys.argv[1]):
        key = (r.code_hash, r.start_pc, 'MSCNT' if r.resume else 'MSCAL')
        runs[key] += 1
        kick_bytes[key] += sum(len(p) for p in r.packets)
    for key, n in runs.most_common():
        print(f'image {key[0]} pc {key[1]:#06x} {key[2]}: {n} runs, {kick_bytes[key] / n:.0f} bytes kicked per run')
