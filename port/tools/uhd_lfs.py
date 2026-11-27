#!/usr/bin/env python3
"""Unpack RE4 UHD (Steam) .lfs files: the PC release's compressed copies of its data files (.udas.lfs, .evd.lfs ...).

    uhd_lfs.py IN.lfs [OUT]          (OUT default: IN without .lfs)

Layout (after emoose's re4lfs.cpp, re4-research): a 20-byte header "RDLX", 0xFEEEBAAA, the size unpacked, the size
packed, the number of 64 KiB chunks; then per chunk u16 size packed (0 = 64 KiB), u16 size unpacked (0 = 64 KiB),
u32 offset from the chunk table (bit 0: packed). A packed chunk is one XMemDecompress buffer (codec 1, LZX, 128 KiB
window): frames of 32 KiB output, each led by its packed size (big-endian u16; 0xFF first: its output size, then the
packed size), the LZX bit stream continuing over the frames. Our own LZX decoder (the CAB format's, as libmspack's
lzxd reads it), no code of the game or of a tool of the mod scene (DevJunior, 26.11.2026: the Latin American dub of
IG Studios is made for the Steam release; its text and event files come as .lfs).
"""
import struct
import sys

WINDOW_BITS = 17
FRAME = 32768


def _extra():
    extra, base = [], []
    b = 0
    for i in range(51):
        e = 0 if i < 4 else min((i - 2) >> 1, 17)
        extra.append(e)
        base.append(b)
        b += 1 << e
    return extra, base


EXTRA, BASE = _extra()


class Bits:
    """16-bit little-endian words, read from the top bit down."""

    def __init__(self, data):
        self.d = data
        self.p = 0
        self.buf = 0
        self.n = 0

    def _fill(self):
        while self.n <= 16:
            if self.p + 1 < len(self.d):
                w = self.d[self.p] | (self.d[self.p + 1] << 8)
            else:
                w = 0
            self.p += 2
            self.buf = (self.buf << 16) | w
            self.n += 16

    def peek(self, k):
        if self.n < k:
            self._fill()
        return (self.buf >> (self.n - k)) & ((1 << k) - 1)

    def skip(self, k):
        self.n -= k
        self.buf &= (1 << self.n) - 1

    def read(self, k):
        if k == 0:
            return 0
        if k > 16:
            hi = self.read(16)
            return (hi << (k - 16)) | self.read(k - 16)
        v = self.peek(k)
        self.skip(k)
        return v

    def align(self):
        """For an uncompressed block: drop the bits left of the current word (libmspack's realignment)."""
        self.skip(self.n % 16)
        self.p -= self.n // 8
        self.buf = 0
        self.n = 0


class Tree:
    def __init__(self, lens):
        self.lens = list(lens)
        maxl = max(self.lens) if any(self.lens) else 1
        self.maxl = maxl
        size = 1 << maxl
        self.sym = [0] * size
        self.ln = [0] * size
        code = 0
        for L in range(1, maxl + 1):
            for s, l in enumerate(self.lens):
                if l == L:
                    shift = maxl - L
                    start = code << shift
                    for k in range(start, start + (1 << shift)):
                        self.sym[k] = s
                        self.ln[k] = L
                    code += 1
            code <<= 1

    def decode(self, bits):
        k = bits.peek(self.maxl)
        bits.skip(self.ln[k])
        return self.sym[k]


def _read_lengths(bits, lens, first, last):
    pre = Tree([bits.read(4) for _ in range(20)])
    x = first
    while x < last:
        z = pre.decode(bits)
        if z == 17:
            run = bits.read(4) + 4
            for _ in range(run):
                lens[x] = 0
                x += 1
        elif z == 18:
            run = bits.read(5) + 20
            for _ in range(run):
                lens[x] = 0
                x += 1
        elif z == 19:
            run = bits.read(1) + 4
            z = pre.decode(bits)
            z = (lens[x] - z + 17) % 17
            for _ in range(run):
                lens[x] = z
                x += 1
        else:
            lens[x] = (lens[x] - z + 17) % 17
            x += 1


def lzx_decompress(data, out_size):
    """One XMemDecompress buffer to `out_size` bytes."""
    slots = WINDOW_BITS * 2
    main_n = 256 + slots * 8
    main_len = [0] * main_n
    len_len = [0] * 249
    out = bytearray()
    r = [1, 1, 1]
    block_type = 0
    remaining = 0
    main = length = aligned = None
    header = True
    pos = 0
    bits = None
    done = 0  # the output's size at the frames' boundaries
    while done < out_size:
        # the frame's header
        if data[pos] == 0xFF:
            frame_out = (data[pos + 1] << 8) | data[pos + 2]
            frame_in = (data[pos + 3] << 8) | data[pos + 4]
            pos += 5
        else:
            frame_out = FRAME
            frame_in = (data[pos] << 8) | data[pos + 1]
            pos += 2
        frame_out = min(frame_out, out_size - done)
        bits = Bits(data[pos:pos + frame_in])
        pos += frame_in
        if header:
            header = False
            if bits.read(1):
                bits.read(32)  # the Intel E8 translation's size (not undone: the game's data has no x86 code)
        end = done + frame_out
        done = end
        while len(out) < end:
            if remaining == 0:
                block_type = bits.read(3)
                remaining = (bits.read(16) << 8) | bits.read(8)
                if block_type == 2:
                    aligned = Tree([bits.read(3) for _ in range(8)])
                if block_type in (1, 2):
                    _read_lengths(bits, main_len, 0, 256)
                    _read_lengths(bits, main_len, 256, main_n)
                    main = Tree(main_len)
                    _read_lengths(bits, len_len, 0, 249)
                    length = Tree(len_len)
                elif block_type == 3:
                    bits.align()
                    q = bits.p
                    r = list(struct.unpack_from("<III", bits.d, q))
                    bits.p = q + 12
                else:
                    raise ValueError("lzx: block type %d" % block_type)
            if block_type == 3:
                take = min(remaining, end - len(out))
                q = bits.p
                out += bits.d[q:q + take]
                bits.p = q + take
                remaining -= take
                if remaining == 0 and take & 1:
                    bits.p += 1
                continue
            while remaining > 0 and len(out) < end:
                m = main.decode(bits)
                if m < 256:
                    out.append(m)
                    remaining -= 1
                    continue
                m -= 256
                ml = m & 7
                if ml == 7:
                    ml += length.decode(bits)
                ml += 2
                slot = m >> 3
                if slot > 2:
                    e = EXTRA[slot]
                    if block_type == 2 and e >= 3:
                        v = (bits.read(e - 3) << 3) if e > 3 else 0
                        off = BASE[slot] - 2 + v + aligned.decode(bits)
                    else:
                        off = BASE[slot] - 2 + bits.read(e)
                    r = [off, r[0], r[1]]
                elif slot == 0:
                    off = r[0]
                elif slot == 1:
                    off = r[1]
                    r = [off, r[0], r[2]]
                else:
                    off = r[2]
                    r = [off, r[1], r[0]]
                src = len(out) - off
                if src < 0:
                    raise ValueError("lzx: match before the start")
                if off >= ml:
                    out += out[src:src + ml]
                else:
                    for k in range(ml):
                        out.append(out[src + k])
                remaining -= ml
            # a match may run over the frame's end: the rest stays in `out`, the next frame counts from its true start
    return bytes(out[:out_size])


def unpack(raw):
    magic = raw[:4]
    if magic == b"RDLX":
        e = "<"
    elif magic == b"XLDR":
        e = ">"
    else:
        raise ValueError("not an LFS file")
    _m2, size, _packed, n = struct.unpack_from(e + "IIII", raw, 4)
    out = bytearray()
    for i in range(n):
        cs, us, off = struct.unpack_from(e + "HHI", raw, 20 + 8 * i)
        cs = cs or 0x10000
        us = us or 0x10000
        at = 20 + (off & ~1)
        if off & 1:
            out += lzx_decompress(raw[at:at + cs], us)
        else:
            out += raw[at:at + us]
    if len(out) != size:
        raise ValueError("lfs: %d bytes unpacked, the header says %d" % (len(out), size))
    return bytes(out)


def main(argv=None):
    argv = argv or sys.argv[1:]
    src = argv[0]
    dst = argv[1] if len(argv) > 1 else (src[:-4] if src.lower().endswith(".lfs") else src + ".out")
    data = unpack(open(src, "rb").read())
    open(dst, "wb").write(data)
    print("%s: %d bytes" % (dst, len(data)))


if __name__ == "__main__":
    main()
