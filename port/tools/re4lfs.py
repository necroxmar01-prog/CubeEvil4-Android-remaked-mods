#!/usr/bin/env python3
"""Unpacks the PC release's .lfs files (RE4 UHD, Steam 2014: BIO4/**/*.lfs; mods ship them too).

    port/tools/re4lfs.py IN.lfs [OUT]      (OUT: IN without .lfs)

An .lfs file: magic "RDLX" (the word "XLDR"), 0xFEEEBAAA, u32 unpacked size, u32 packed size, u32 chunk count, then
per chunk u32 (bits 0-15 packed size, 16-31 unpacked size, 0 = 0x10000) and u32 offset (+0xF in
the file: a zero word, then the packed data); each chunk is unpacked on its own (64 KB) with Microsoft's XMemCompress LZX (window
128 KB): frames of up to 32 KB, each behind its packed size (u16 big-endian; 0xFF, u16 unpacked
size, u16 packed size for a shorter one), the LZX bit stream of the cabinet format (16-bit
little-endian words, most significant bit first; blocks verbatim / aligned / uncompressed).
A chunk whose packed size equals its unpacked size is stored as it is.

Written for the skin converter (port/tools/uhd_skin_convert.py); the files are the player's.
"""

import struct
import sys

MAGIC = b"RDLX"  # the word 0x584C4452, "XLDR" read little-endian


class LzxError(ValueError):
    pass


EXTRA = [0, 0, 0, 0] + [min(17, i // 2) for i in range(2, 50)][:46]
EXTRA = EXTRA[:50]
BASE = [0]
for _i in range(1, 51):
    BASE.append(BASE[-1] + (1 << EXTRA[_i - 1]))
SLOTS = {15: 30, 16: 32, 17: 34, 18: 36, 19: 38, 20: 42, 21: 50}


def table(lengths, bits=16):
    """Canonical Huffman decode table: index = next `bits` bits -> (symbol, length)."""
    t = [None] * (1 << bits)
    code = 0
    for length in range(1, bits + 1):
        for sym, l in enumerate(lengths):
            if l != length:
                continue
            span = 1 << (bits - length)
            start = code << (bits - length)
            if start + span > len(t):
                raise LzxError("Huffman table overflow")
            entry = (sym, length)
            for k in range(start, start + span):
                t[k] = entry
            code += 1
        code <<= 1
    return t


class Bits:
    def __init__(self, data, pos, end):
        self.d, self.pos, self.end = data, pos, end
        self.buf, self.left = 0, 0

    def _fill(self):
        while self.left <= 16:
            if self.pos + 1 < self.end:
                w = self.d[self.pos] | self.d[self.pos + 1] << 8
            else:
                w = 0  # past the frame: zeros (the decoder never uses them)
            self.pos += 2
            self.buf |= w << (16 - self.left)
            self.left += 16

    def peek16(self):
        if self.left < 16:
            self._fill()
        return (self.buf >> 16) & 0xFFFF

    def drop(self, n):
        self.buf = (self.buf << n) & 0xFFFFFFFF
        self.left -= n

    def read(self, n):
        if n == 0:
            return 0
        if n > 16:
            return self.read(16) << (n - 16) | self.read(n - 16)
        if self.left < n:
            self._fill()
        v = self.buf >> (32 - n)
        self.drop(n)
        return v

    def decode(self, t):
        e = t[self.peek16()]
        if e is None:
            raise LzxError("bad Huffman code")
        self.drop(e[1])
        return e[0]

    def align(self):
        """Before an uncompressed block: skip to the next 16-bit word (1..16 bits of padding) and
        give back the words read ahead."""
        skip = self.left % 16 or 16
        if self.left < skip:  # nothing buffered: the padding is the next word
            self.pos += 2
        else:
            self.left -= skip
        self.pos -= 2 * (self.left // 16)
        self.buf, self.left = 0, 0


class Lzx:
    def __init__(self, window_bits=17):
        self.wsize = 1 << window_bits
        self.window = bytearray(self.wsize)
        self.wpos = 0
        self.slots = SLOTS[window_bits]
        self.main_lengths = [0] * (256 + self.slots * 8)
        self.len_lengths = [0] * 249
        self.r = [1, 1, 1]
        self.header = False
        self.e8 = 0
        self.block_type = 0
        self.block_left = 0
        self.target = 0
        self.raw_odd = 0

    def _lengths(self, bits, lengths, first, last):
        pre = [bits.read(4) for _ in range(20)]
        pt = table(pre)
        x = first
        while x < last:
            z = bits.decode(pt)
            if z == 17:
                y = bits.read(4) + 4
                lengths[x:x + y] = [0] * y
                x += y
            elif z == 18:
                y = bits.read(5) + 20
                lengths[x:x + y] = [0] * y
                x += y
            elif z == 19:
                y = bits.read(1) + 4
                z = bits.decode(pt)
                z = (lengths[x] + 17 - z) % 17
                lengths[x:x + y] = [z] * y
                x += y
            else:
                lengths[x] = (lengths[x] + 17 - z) % 17
                x += 1

    def frame(self, data, pos, end, frame_size):
        """Decodes one frame from data[pos:end] up to the frame's end in the output (`frame_size`
        after the last frame's end); returns the new bytes (a match may run a little past it: those
        bytes belong to the next frame and come now)."""
        bits = Bits(data, pos, end)
        if not self.header:
            self.header = True
            if bits.read(1):
                self.e8 = bits.read(16) << 16 | bits.read(16)
        start = self.wpos
        self.target = getattr(self, "target", 0) + frame_size
        w, wmask = self.window, self.wsize - 1
        while self.wpos < self.target:
            if self.block_left <= 0:
                carry = -self.block_left
                self.block_type = bits.read(3)
                self.block_left = (bits.read(16) << 8 | bits.read(8)) - carry
                if self.block_type == 2:
                    self.aligned = table([bits.read(3) for _ in range(8)])
                if self.block_type in (1, 2):
                    self._lengths(bits, self.main_lengths, 0, 256)
                    self._lengths(bits, self.main_lengths, 256, len(self.main_lengths))
                    self.main = table(self.main_lengths)
                    self._lengths(bits, self.len_lengths, 0, 249)
                    self.lens = table(self.len_lengths) if any(self.len_lengths) else None
                elif self.block_type == 3:
                    bits.align()
                    p = bits.pos
                    self.r = list(struct.unpack_from("<3I", data, p))
                    bits.pos = p + 12
                    self.raw_odd = (self.block_left + carry) & 1
                else:
                    raise LzxError("block type %d" % self.block_type)
            run = min(self.block_left, self.target - self.wpos)
            if self.block_type == 3:
                p = bits.pos
                for k in range(run):
                    w[(self.wpos + k) & wmask] = data[p + k]
                bits.pos = p + run
                self.wpos += run
                self.block_left -= run
                if self.block_left == 0 and self.raw_odd:
                    bits.pos += 1
                continue
            r0, r1, r2 = self.r
            aligned = self.block_type == 2
            main, lens = self.main, self.lens
            stop = self.wpos + run
            while self.wpos < stop:
                sym = bits.decode(main)
                if sym < 256:
                    w[self.wpos & wmask] = sym
                    self.wpos += 1
                    continue
                sym -= 256
                length = sym & 7
                if length == 7:
                    if lens is None:
                        raise LzxError("length tree missing")
                    length += bits.decode(lens)
                length += 2
                slot = sym >> 3
                if slot > 2:
                    extra = EXTRA[slot]
                    off = BASE[slot] - 2
                    if aligned and extra >= 3:
                        if extra > 3:
                            off += bits.read(extra - 3) << 3
                        off += bits.decode(self.aligned)
                    elif extra:
                        off += bits.read(extra)
                    r2, r1, r0 = r1, r0, off
                elif slot == 0:
                    off = r0
                elif slot == 1:
                    off = r1
                    r1, r0 = r0, off
                else:
                    off = r2
                    r2, r0 = r0, off
                src = self.wpos - off
                if src < 0 and self.wpos < self.wsize:
                    raise LzxError("match before the start")
                for k in range(length):
                    w[(self.wpos + k) & wmask] = w[(src + k) & wmask]
                self.wpos += length
            self.r = [r0, r1, r2]
            self.block_left -= self.wpos - (stop - run)
        n = self.wpos - start
        if n > self.wsize:
            raise LzxError("frame larger than the window")
        out = bytearray(w[(start + k) & wmask] for k in range(n))
        if self.e8 and n > 10:
            i = 0
            while i < n - 10:
                if out[i] == 0xE8:
                    cur = start + i
                    absv = struct.unpack_from("<i", out, i + 1)[0]
                    if -cur <= absv < self.e8:
                        rel = absv - cur if absv >= 0 else absv + self.e8
                        struct.pack_into("<i", out, i + 1, rel)
                    i += 5
                else:
                    i += 1
        return bytes(out)


def xmem_decompress(data, pos, end, out_size, window_bits=17):
    """One XMemCompress stream (a chunk) of `out_size` bytes."""
    lzx = Lzx(window_bits)
    out = bytearray()
    while len(out) < out_size and pos < end:
        hi = data[pos]
        if hi == 0xFF:
            frame_size = data[pos + 1] << 8 | data[pos + 2]
            block = data[pos + 3] << 8 | data[pos + 4]
            pos += 5
        else:
            frame_size = 0x8000
            block = hi << 8 | data[pos + 1]
            pos += 2
        if block == 0 or frame_size == 0:
            break
        out += lzx.frame(data, pos, pos + block, frame_size)
        pos += block
    if len(out) < out_size:
        raise LzxError("chunk ended after %d of %d bytes" % (len(out), out_size))
    return bytes(out[:out_size])


def unpack(data):
    if data[:4] != MAGIC:
        raise LzxError("not an .lfs file")
    _, total, packed, count = struct.unpack_from("<4I", data, 4)
    out = bytearray()
    for i in range(count):
        sizes, off = struct.unpack_from("<II", data, 20 + i * 8)
        csize, usize = sizes & 0xFFFF, sizes >> 16 or 0x10000
        start = off + 0xF + 4  # each chunk: a word (0), then the packed data
        if csize == usize:
            out += data[start:start + usize]
            continue
        out += xmem_decompress(data, start, start + csize, usize)
    if len(out) != total:
        raise LzxError("unpacked %d bytes, the header says %d" % (len(out), total))
    return bytes(out)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else (src[:-4] if src.lower().endswith(".lfs") else src + ".out")
    data = unpack(open(src, "rb").read())
    with open(dst, "wb") as f:
        f.write(data)
    print("%s: %d bytes" % (dst, len(data)))


if __name__ == "__main__":
    main()
