// CubeEvil4 Studio's converter of UHD skins (studio_convert.h): port/tools/uhd_skin_convert.py and re4lfs.py in C++.
// The names follow the Python tool's, so the two can be read side by side.
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include "third_party/bcdec.h"  // BC1..BC7 blocks (Sergii Kudlai, MIT or Unlicense): the RE4 Remake's textures
#include "third_party/meshoptimizer/meshoptimizer.h"  // the simplifier (Arseny Kapoulkine, MIT): too dense skins
#include <cctype>
#include "studio_convert.h"
#include "texture.h"  // DecodeTpl: the disc's pictures (the PC HUD: only the changed ones)

#include <algorithm>
#include <array>
#include <cfenv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <cctype>
#include <stdexcept>
#include <tuple>

namespace re4::studio::convert {
namespace {

// ---- bytes --------------------------------------------------------------------------------------------------------

[[noreturn]] void fail(const std::string &what) { throw std::runtime_error(what); }

std::uint32_t le32(const Bytes &b, std::size_t p) {
  if (p + 4 > b.size()) fail("truncated (offset " + std::to_string(p) + ")");
  return std::uint32_t(b[p]) | std::uint32_t(b[p + 1]) << 8 | std::uint32_t(b[p + 2]) << 16 |
         std::uint32_t(b[p + 3]) << 24;
}
std::uint16_t le16(const Bytes &b, std::size_t p) {
  if (p + 2 > b.size()) fail("truncated (offset " + std::to_string(p) + ")");
  return std::uint16_t(b[p] | b[p + 1] << 8);
}
std::int16_t les16(const Bytes &b, std::size_t p) { return std::int16_t(le16(b, p)); }
float lef32(const Bytes &b, std::size_t p) {
  const std::uint32_t u = le32(b, p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
std::uint32_t be32(const Bytes &b, std::size_t p) {
  if (p + 4 > b.size()) fail("truncated (offset " + std::to_string(p) + ")");
  return std::uint32_t(b[p]) << 24 | std::uint32_t(b[p + 1]) << 16 | std::uint32_t(b[p + 2]) << 8 | b[p + 3];
}
std::uint16_t be16(const Bytes &b, std::size_t p) {
  if (p + 2 > b.size()) fail("truncated (offset " + std::to_string(p) + ")");
  return std::uint16_t(b[p] << 8 | b[p + 1]);
}
float bef32(const Bytes &b, std::size_t p) {
  const std::uint32_t u = be32(b, p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
std::uint8_t at8(const Bytes &b, std::size_t p) {
  if (p >= b.size()) fail("truncated (offset " + std::to_string(p) + ")");
  return b[p];
}

void put8(Bytes &o, unsigned v) { o.push_back(std::uint8_t(v)); }
void put16(Bytes &o, unsigned v) { o.push_back(std::uint8_t(v >> 8)), o.push_back(std::uint8_t(v)); }
void put32(Bytes &o, std::uint32_t v) { put16(o, v >> 16), put16(o, v & 0xFFFF); }
void putf(Bytes &o, float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  put32(o, u);
}
void set32(Bytes &o, std::size_t p, std::uint32_t v) {
  o[p] = std::uint8_t(v >> 24), o[p + 1] = std::uint8_t(v >> 16), o[p + 2] = std::uint8_t(v >> 8),
  o[p + 3] = std::uint8_t(v);
}
std::size_t align(std::size_t n, std::size_t a = 32) { return (n + a - 1) / a * a; }
void pad(Bytes &o, std::size_t a = 32) { o.resize(align(o.size(), a), 0); }

// Python's int(round(x)): to the nearest, halves to even
long long pyround(double x) {
  const int mode = std::fegetround();
  std::fesetround(FE_TONEAREST);
  const double r = std::nearbyint(x);
  std::fesetround(mode);
  return (long long)r;
}
int quant(double v, double scale, int lo = -32768, int hi = 32767) {
  const long long q = pyround(v * scale);
  return int(std::max<long long>(lo, std::min<long long>(hi, q)));
}

std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char *f, ...) {
  char buf[512];
  va_list a;
  va_start(a, f);
  std::vsnprintf(buf, sizeof(buf), f, a);
  va_end(a);
  return buf;
}

// ---- .lfs: XMemCompress LZX (re4lfs.py) -------------------------------------------------------------------------

struct Huff {
  std::vector<std::int32_t> t;  // (symbol << 8 | length), -1 none; indexed by the next 16 bits
};

Huff table(const std::vector<int> &lengths) {
  Huff h;
  h.t.assign(1 << 16, -1);
  unsigned code = 0;
  for (int length = 1; length <= 16; ++length) {
    for (std::size_t sym = 0; sym < lengths.size(); ++sym) {
      if (lengths[sym] != length) continue;
      const unsigned span = 1u << (16 - length), start = code << (16 - length);
      if (start + span > h.t.size()) fail("LZX: Huffman table overflow");
      for (unsigned k = start; k < start + span; ++k) h.t[k] = std::int32_t(sym << 8 | unsigned(length));
      ++code;
    }
    code <<= 1;
  }
  return h;
}

struct Bits {
  const Bytes &d;
  std::size_t pos, end;
  std::uint32_t buf = 0;
  int left = 0;
  Bits(const Bytes &data, std::size_t p, std::size_t e) : d(data), pos(p), end(e) {}
  void fill() {
    while (left <= 16) {
      const unsigned w = pos + 1 < end ? unsigned(d[pos] | d[pos + 1] << 8) : 0u;
      pos += 2;
      buf |= std::uint32_t(w) << (16 - left);
      left += 16;
    }
  }
  unsigned peek16() {
    if (left < 16) fill();
    return (buf >> 16) & 0xFFFF;
  }
  void drop(int n) {
    buf = n >= 32 ? 0 : buf << n;
    left -= n;
  }
  unsigned read(int n) {
    if (n == 0) return 0;
    if (n > 16) {
      const unsigned hi = read(16);
      return hi << (n - 16) | read(n - 16);
    }
    if (left < n) fill();
    const unsigned v = buf >> (32 - n);
    drop(n);
    return v;
  }
  unsigned decode(const Huff &t) {
    const std::int32_t e = t.t[peek16()];
    if (e < 0) fail("LZX: bad Huffman code");
    drop(e & 0xFF);
    return unsigned(e >> 8);
  }
  void alignWord() {
    const int skip = left % 16 ? left % 16 : 16;
    if (left < skip) {
      pos += 2;
    } else {
      left -= skip;
    }
    pos -= 2 * std::size_t(left / 16);
    buf = 0;
    left = 0;
  }
};

const std::array<int, 51> &Extra() {
  static const std::array<int, 51> e = [] {
    std::array<int, 51> a{};
    // EXTRA = [0, 0, 0, 0] + [min(17, i // 2) for i in range(2, 50)][:46]
    for (int i = 4; i < 50; ++i) a[std::size_t(i)] = std::min(17, (i - 2) / 2);
    return a;
  }();
  return e;
}

class Lzx {
public:
  explicit Lzx(int windowBits = 17)
      : wsize_(1u << windowBits), window_(wsize_), slots_(windowBits == 17 ? 34 : 32),
        mainLengths_(256 + std::size_t(slots_) * 8, 0), lenLengths_(249, 0) {
    for (int i = 0; i <= 50; ++i) base_[std::size_t(i)] = i == 0 ? 0 : base_[std::size_t(i - 1)] + (1u << Extra()[std::size_t(i - 1)]);
  }

  Bytes frame(const Bytes &data, std::size_t pos, std::size_t end, unsigned frameSize) {
    Bits bits(data, pos, end);
    if (!header_) {
      header_ = true;
      if (bits.read(1)) e8_ = bits.read(16) << 16 | bits.read(16);
    }
    const std::uint64_t start = wpos_;
    target_ += frameSize;
    const std::uint64_t wmask = wsize_ - 1;
    while (wpos_ < target_) {
      if (blockLeft_ <= 0) {
        const long long carry = -blockLeft_;
        blockType_ = int(bits.read(3));
        blockLeft_ = (long long)(bits.read(16) << 8 | bits.read(8)) - carry;
        if (blockType_ == 2) {
          std::vector<int> al(8);
          for (auto &x : al) x = int(bits.read(3));
          aligned_ = table(al);
        }
        if (blockType_ == 1 || blockType_ == 2) {
          lengths(bits, mainLengths_, 0, 256);
          lengths(bits, mainLengths_, 256, mainLengths_.size());
          main_ = table(mainLengths_);
          lengths(bits, lenLengths_, 0, 249);
          hasLens_ = std::any_of(lenLengths_.begin(), lenLengths_.end(), [](int l) { return l != 0; });
          if (hasLens_) lens_ = table(lenLengths_);
        } else if (blockType_ == 3) {
          bits.alignWord();
          const std::size_t p = bits.pos;
          r_ = {le32(data, p), le32(data, p + 4), le32(data, p + 8)};
          bits.pos = p + 12;
          rawOdd_ = (blockLeft_ + carry) & 1;
        } else {
          fail("LZX: block type " + std::to_string(blockType_));
        }
      }
      const long long run = std::min<long long>(blockLeft_, (long long)(target_ - wpos_));
      if (blockType_ == 3) {
        const std::size_t p = bits.pos;
        for (long long k = 0; k < run; ++k) window_[(wpos_ + std::uint64_t(k)) & wmask] = at8(data, p + std::size_t(k));
        bits.pos = p + std::size_t(run);
        wpos_ += std::uint64_t(run);
        blockLeft_ -= run;
        if (blockLeft_ == 0 && rawOdd_) bits.pos += 1;
        continue;
      }
      std::uint32_t r0 = r_[0], r1 = r_[1], r2 = r_[2];
      const bool aligned = blockType_ == 2;
      const std::uint64_t stop = wpos_ + std::uint64_t(run);
      const std::uint64_t before = wpos_;
      while (wpos_ < stop) {
        unsigned sym = bits.decode(main_);
        if (sym < 256) {
          window_[wpos_ & wmask] = std::uint8_t(sym);
          ++wpos_;
          continue;
        }
        sym -= 256;
        unsigned length = sym & 7;
        if (length == 7) {
          if (!hasLens_) fail("LZX: length tree missing");
          length += bits.decode(lens_);
        }
        length += 2;
        const unsigned slot = sym >> 3;
        std::uint32_t off;
        if (slot > 2) {
          const int extra = Extra()[slot];
          off = base_[slot] - 2;
          if (aligned && extra >= 3) {
            if (extra > 3) off += bits.read(extra - 3) << 3;
            off += bits.decode(aligned_);
          } else if (extra) {
            off += bits.read(extra);
          }
          r2 = r1, r1 = r0, r0 = off;
        } else if (slot == 0) {
          off = r0;
        } else if (slot == 1) {
          off = r1;
          r1 = r0, r0 = off;
        } else {
          off = r2;
          r2 = r0, r0 = off;
        }
        const long long src = (long long)wpos_ - (long long)off;
        if (src < 0 && wpos_ < wsize_) fail("LZX: match before the start");
        for (unsigned k = 0; k < length; ++k)
          window_[(wpos_ + k) & wmask] = window_[std::uint64_t(src + (long long)k) & wmask];
        wpos_ += length;
      }
      r_ = {r0, r1, r2};
      blockLeft_ -= (long long)(wpos_ - before);
    }
    const std::uint64_t n = wpos_ - start;
    if (n > wsize_) fail("LZX: frame larger than the window");
    Bytes out(n);
    for (std::uint64_t k = 0; k < n; ++k) out[k] = window_[(start + k) & wmask];
    if (e8_ && n > 10) {
      for (std::uint64_t i = 0; i < n - 10;) {
        if (out[i] == 0xE8) {
          const long long cur = (long long)(start + i);
          std::int32_t absv;
          std::memcpy(&absv, &out[i + 1], 4);
          if (-cur <= absv && absv < (long long)e8_) {
            const std::int32_t rel = std::int32_t(absv >= 0 ? absv - cur : absv + (long long)e8_);
            std::memcpy(&out[i + 1], &rel, 4);
          }
          i += 5;
        } else {
          ++i;
        }
      }
    }
    return out;
  }

private:
  void lengths(Bits &bits, std::vector<int> &lens, std::size_t first, std::size_t last) {
    std::vector<int> pre(20);
    for (auto &x : pre) x = int(bits.read(4));
    const Huff pt = table(pre);
    std::size_t x = first;
    while (x < last) {
      const unsigned z = bits.decode(pt);
      if (z == 17) {
        const unsigned y = bits.read(4) + 4;
        for (unsigned k = 0; k < y && x + k < lens.size(); ++k) lens[x + k] = 0;
        x += y;
      } else if (z == 18) {
        const unsigned y = bits.read(5) + 20;
        for (unsigned k = 0; k < y && x + k < lens.size(); ++k) lens[x + k] = 0;
        x += y;
      } else if (z == 19) {
        const unsigned y = bits.read(1) + 4;
        const unsigned z2 = bits.decode(pt);
        if (x >= lens.size()) fail("LZX: lengths past the end");
        const int v = (lens[x] + 17 - int(z2)) % 17;
        for (unsigned k = 0; k < y && x + k < lens.size(); ++k) lens[x + k] = v;
        x += y;
      } else {
        lens[x] = (lens[x] + 17 - int(z)) % 17;
        ++x;
      }
    }
  }

  std::uint64_t wsize_;
  std::vector<std::uint8_t> window_;
  std::uint64_t wpos_ = 0;
  int slots_;
  std::vector<int> mainLengths_, lenLengths_;
  std::array<std::uint32_t, 3> r_{1, 1, 1};
  bool header_ = false;
  std::uint32_t e8_ = 0;
  int blockType_ = 0;
  long long blockLeft_ = 0;
  std::uint64_t target_ = 0;
  long long rawOdd_ = 0;
  Huff main_, lens_, aligned_;
  bool hasLens_ = false;
  std::array<std::uint32_t, 51> base_{};
};

Bytes xmemDecompress(const Bytes &data, std::size_t pos, std::size_t end, std::size_t outSize) {
  Lzx lzx(17);
  Bytes out;
  while (out.size() < outSize && pos < end) {
    const unsigned hi = at8(data, pos);
    unsigned frameSize, block;
    if (hi == 0xFF) {
      frameSize = unsigned(at8(data, pos + 1) << 8 | at8(data, pos + 2));
      block = unsigned(at8(data, pos + 3) << 8 | at8(data, pos + 4));
      pos += 5;
    } else {
      frameSize = 0x8000;
      block = hi << 8 | at8(data, pos + 1);
      pos += 2;
    }
    if (block == 0 || frameSize == 0) break;
    const Bytes f = lzx.frame(data, pos, pos + block, frameSize);
    out.insert(out.end(), f.begin(), f.end());
    pos += block;
  }
  if (out.size() < outSize) fail("LZX: chunk ended after " + std::to_string(out.size()) + " bytes");
  out.resize(outSize);
  return out;
}

}  // namespace


namespace {

// ---- archives (.das/.drs/.udas): part table at 0x20, part 0 a DAT archive ----------------------------------------

struct Archive {
  Bytes data;
  bool big = true;
  struct Part {
    std::uint32_t type, size, x, off;
    std::array<std::uint8_t, 16> tail;
  };
  std::vector<Part> parts;
  Bytes body;
  std::vector<std::uint32_t> offsets;
  std::size_t rel = 0;
  std::vector<std::array<std::uint8_t, 4>> types;

  std::uint32_t u(const Bytes &b, std::size_t p) const { return big ? be32(b, p) : le32(b, p); }

  Archive(Bytes d, bool bigEndian) : data(std::move(d)), big(bigEndian) {
    for (int i = 0; i < 31; ++i) {
      const std::size_t r = 0x20 + std::size_t(i) * 0x20;
      Part p{u(data, r), u(data, r + 4), u(data, r + 8), u(data, r + 12), {}};
      if (p.type == 0xFFFFFFFFu) break;
      std::memcpy(p.tail.data(), &data.at(r + 0x10), 16);
      parts.push_back(p);
    }
    if (parts.empty() || parts[0].type != 0) fail("part 0 is not the data archive");
    if (std::size_t(parts[0].off) + parts[0].size > data.size()) fail("the data archive runs past the file");
    body.assign(data.begin() + parts[0].off, data.begin() + parts[0].off + parts[0].size);
    const std::uint32_t n = u(body, 0);
    if (n > 4096) fail("not an archive");
    for (std::uint32_t i = 0; i < n; ++i) offsets.push_back(u(body, 16 + i * 4));
    const std::uint32_t r = u(body, 4);
    rel = 16 + std::size_t(n) * 8 <= r && r < body.size() && r % 32 == 0 ? r : 0;
    for (std::uint32_t i = 0; i < n; ++i) {
      std::array<std::uint8_t, 4> t{};
      std::memcpy(t.data(), &body.at(16 + std::size_t(n) * 4 + i * 4), 4);
      types.push_back(t);
    }
  }
  std::size_t count() const { return offsets.size(); }
  Bytes entry(std::size_t i) const {
    const std::size_t start = offsets[i];
    const std::size_t stop = i + 1 < offsets.size() ? offsets[i + 1] : (rel ? rel : body.size());
    if (start == 0 || start >= stop || stop > body.size()) return {};
    return Bytes(body.begin() + long(start), body.begin() + long(stop));
  }
  std::string kind(std::size_t i) const {
    std::string k(types[i].begin(), types[i].end());
    while (!k.empty() && k.back() == 0) k.pop_back();
    for (auto &c : k) c = char(std::toupper(static_cast<unsigned char>(c)));
    return k;
  }
};

// The PC archive seen with the disc's entries (Aligned): a mod's archive may hold entries the disc's lacks
struct PcView {
  const Archive *pc = nullptr;
  std::vector<long> map;  // per disc entry: the PC entry, -1 none
  std::vector<std::size_t> dropped;
  std::string kind(std::size_t i) const { return map[i] >= 0 ? pc->kind(std::size_t(map[i])) : ""; }
  Bytes entry(std::size_t i) const { return map[i] >= 0 ? pc->entry(std::size_t(map[i])) : Bytes{}; }
};

// difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks(), for the kinds
void matching(const std::vector<std::string> &a, const std::vector<std::string> &b, std::size_t alo, std::size_t ahi,
              std::size_t blo, std::size_t bhi, std::vector<std::array<std::size_t, 3>> &out) {
  // find_longest_match: the longest equal run, earliest in a, then in b
  std::size_t besti = alo, bestj = blo, bestsize = 0;
  std::map<std::size_t, std::size_t> j2len;
  for (std::size_t i = alo; i < ahi; ++i) {
    std::map<std::size_t, std::size_t> next;
    for (std::size_t j = blo; j < bhi; ++j) {
      if (a[i] != b[j]) continue;
      const auto it = j > 0 ? j2len.find(j - 1) : j2len.end();
      const std::size_t k = (it != j2len.end() ? it->second : 0) + 1;
      next[j] = k;
      if (k > bestsize) besti = i - k + 1, bestj = j - k + 1, bestsize = k;
    }
    j2len.swap(next);
  }
  if (!bestsize) return;
  if (alo < besti && blo < bestj) matching(a, b, alo, besti, blo, bestj, out);
  out.push_back({besti, bestj, bestsize});
  if (besti + bestsize < ahi && bestj + bestsize < bhi) matching(a, b, besti + bestsize, ahi, bestj + bestsize, bhi, out);
}

PcView align(const Archive &gc, const Archive &pc) {
  PcView v;
  v.pc = &pc;
  const std::size_t n = gc.count();
  std::vector<std::string> gk, pk;
  for (std::size_t i = 0; i < n; ++i) gk.push_back(gc.kind(i));
  for (std::size_t i = 0; i < pc.count(); ++i) pk.push_back(pc.kind(i));
  v.map.assign(n, -1);
  if (pc.count() == n) {
    for (std::size_t i = 0; i < n; ++i) v.map[i] = long(i);
    return v;
  }
  bool sameLayout = pk.size() >= gk.size();
  for (std::size_t i = 0; sameLayout && i < gk.size(); ++i)
    sameLayout = gk[i] == pk[i] || gk[i].empty() || pk[i].empty();
  std::vector<bool> used(pk.size(), false);
  if (sameLayout) {
    for (std::size_t i = 0; i < n; ++i)
      if (!gk[i].empty() && !pk[i].empty()) v.map[i] = long(i), used[i] = true;
  } else {
    std::vector<std::array<std::size_t, 3>> blocks;
    matching(gk, pk, 0, gk.size(), 0, pk.size(), blocks);
    blocks.push_back({gk.size(), pk.size(), 0});
    // the opcodes: "equal" blocks map entry by entry, so does a "replace" of equal lengths
    std::size_t i = 0, j = 0;
    for (const auto &blk : blocks) {
      const std::size_t ai = blk[0], bj = blk[1], size = blk[2];
      if (i < ai && j < bj && ai - i == bj - j)
        for (std::size_t k = 0; k < ai - i; ++k) v.map[i + k] = long(j + k);
      for (std::size_t k = 0; k < size; ++k) v.map[ai + k] = long(bj + k);
      i = ai + size, j = bj + size;
    }
    for (long m : v.map)
      if (m >= 0) used[std::size_t(m)] = true;
  }
  for (std::size_t j = 0; j < pk.size(); ++j)
    if (!used[j] && (!sameLayout || !pk[j].empty())) v.dropped.push_back(j);
  return v;
}

Bytes buildBody(const std::vector<Bytes> &entries, const std::vector<std::array<std::uint8_t, 4>> &types) {
  const std::size_t n = entries.size();
  Bytes out(align(16 + n * 8), 0);
  set32(out, 0, std::uint32_t(n));
  for (std::size_t i = 0; i < n; ++i) {
    std::memcpy(&out[16 + n * 4 + i * 4], types[i].data(), 4);
    set32(out, 16 + i * 4, std::uint32_t(out.size()));
    if (entries[i].empty()) continue;
    out.insert(out.end(), entries[i].begin(), entries[i].end());
    pad(out);
  }
  return out;
}

Bytes buildArchive(const Archive &gc, Bytes body) {
  if (gc.rel && be32(body, 4) == 0) {
    pad(body);
    set32(body, 4, std::uint32_t(body.size()));
    body.insert(body.end(), gc.body.begin() + long(gc.rel), gc.body.end());
  }
  Bytes out(gc.data.begin(), gc.data.begin() + 0x20);
  Bytes table(0x3E0, 0);
  std::size_t pos = 0x400 + align(body.size());
  std::vector<Bytes> rest;
  for (std::size_t i = 0; i < gc.parts.size(); ++i) {
    const auto &p = gc.parts[i];
    std::uint32_t recOff, recSize;
    if (i == 0) {
      recOff = 0x400, recSize = std::uint32_t(body.size());
    } else {
      recOff = std::uint32_t(pos), recSize = p.size;
      rest.emplace_back(gc.data.begin() + p.off, gc.data.begin() + p.off + p.size);
      pos = align(pos + p.size);
    }
    set32(table, i * 0x20, p.type), set32(table, i * 0x20 + 4, recSize), set32(table, i * 0x20 + 8, p.x),
        set32(table, i * 0x20 + 12, recOff);
    std::memcpy(&table[i * 0x20 + 0x10], p.tail.data(), 16);
  }
  set32(table, gc.parts.size() * 0x20, 0xFFFFFFFFu);
  out.insert(out.end(), table.begin(), table.end());
  out.insert(out.end(), body.begin(), body.end());
  pad(out);
  for (const auto &r : rest) {
    out.insert(out.end(), r.begin(), r.end());
    pad(out);
  }
  return out;
}

// ---- models --------------------------------------------------------------------------------------------------------

int prim(unsigned type) {
  switch (type) {
  case 6: return 0x98;  // strip
  case 8: return 0x80;  // quads
  case 5: return 0x90;  // triangles
  default: return -1;
  }
}

using Weight = std::vector<std::pair<int, int>>;  // (bone, rate)

struct Run {
  Bytes raw;  // the part header's bytes 0..0x17
  std::uint32_t npoly;
  std::vector<std::pair<unsigned, unsigned>> runs;  // (type, count)
};

struct Model {  // a PC model (little-endian, the fields of cModelData)
  const Bytes &b;
  std::uint32_t head, x04, x08, clr, tex, wgt, parts, flags, ntex, shape, vtx, nrm, version, blend = 0, flip = 0,
      vidx = 0, nidx = 0;
  unsigned wpal, nparts, nlist, shift, x29, wext, nvtx, nnrm;
  std::vector<std::map<unsigned, std::array<int, 3>>> shapes;

  explicit Model(const Bytes &blob) : b(blob) {
    if (blob.size() < 0x50) fail("not a model (too short)");
    head = le32(b, 0), x04 = le32(b, 4), x08 = le32(b, 8), clr = le32(b, 0xC), tex = le32(b, 0x10), wgt = le32(b, 0x14);
    wpal = b[0x18], nparts = b[0x19], nlist = le16(b, 0x1A);
    parts = le32(b, 0x1C), flags = le32(b, 0x20), ntex = le32(b, 0x24);
    shift = b[0x28], x29 = b[0x29], wext = le16(b, 0x2A);
    shape = le32(b, 0x2C), vtx = le32(b, 0x30), nrm = le32(b, 0x34);
    nvtx = le16(b, 0x38), nnrm = le16(b, 0x3A), version = le32(b, 0x3C);
    if (version == 0x20030818) blend = le32(b, 0x40), flip = le32(b, 0x44), vidx = le32(b, 0x48), nidx = le32(b, 0x4C);
    if (version != 0x20010801 && version != 0x20030817 && version != 0x20030818)
      fail(fmt("not a model (version %#x)", version));
    if (shape) {
      const std::uint32_t n = le32(b, shape);
      for (std::uint32_t k = 0; k < n; ++k) {
        const std::uint32_t ofs = le32(b, shape + 4 + k * 8);
        const std::int32_t cnt = std::int32_t(le32(b, shape + 8 + k * 8));
        std::map<unsigned, std::array<int, 3>> d;
        for (std::int32_t j = 0; j < cnt; ++j) {
          const std::size_t q = shape + 4 + ofs + std::size_t(j) * 8;
          d[le16(b, q)] = {les16(b, q + 2), les16(b, q + 4), les16(b, q + 6)};
        }
        shapes.push_back(std::move(d));
      }
    }
    if (nvtx != nnrm) fail(fmt("vertices %u, normals %u: the PC layout has one of each", nvtx, nnrm));
  }
  std::array<double, 3> f3(std::uint32_t off, unsigned i) const {
    return {lef32(b, off + i * 12), lef32(b, off + i * 12 + 4), lef32(b, off + i * 12 + 8)};
  }
  std::vector<Run> runs() const {
    std::vector<Run> out;
    std::size_t p = parts;
    for (unsigned k = 0; k < nlist; ++k) {
      const std::uint32_t size = le32(b, p + 0x18), npoly = le32(b, p + 0x1C);
      const std::uint32_t cnt = size >= 4 ? le32(b, p + 0x20) : 0;
      Run r;
      r.raw.assign(b.begin() + long(p), b.begin() + long(p + 0x18));
      r.npoly = npoly;
      for (std::uint32_t i = 0; i < cnt; ++i) r.runs.push_back({le16(b, p + 0x24 + i * 4), le16(b, p + 0x26 + i * 4)});
      out.push_back(std::move(r));
      p += 0x20 + size;
    }
    return out;
  }
  std::vector<Weight> weights() const {
    const unsigned n = wext > 0xFF ? wext : wpal;
    std::vector<Weight> out;
    for (unsigned i = 0; i < n; ++i) {
      const std::size_t w = wgt + std::size_t(i) * 8;
      const unsigned k = std::min<unsigned>(at8(b, w + 3), 3);
      Weight e;
      for (unsigned j = 0; j < k; ++j) e.push_back({at8(b, w + j), at8(b, w + 4 + j)});
      out.push_back(e);
    }
    return out;
  }
  // (raw 4 bytes, position) per part
  std::vector<std::pair<std::array<std::uint8_t, 4>, std::array<float, 3>>> bones() const {
    std::vector<std::pair<std::array<std::uint8_t, 4>, std::array<float, 3>>> out;
    for (unsigned i = 0; i < nparts; ++i) {
      const std::size_t r = head + std::size_t(i) * 16;
      std::array<std::uint8_t, 4> raw{at8(b, r), at8(b, r + 1), at8(b, r + 2), at8(b, r + 3)};
      out.push_back({raw, {lef32(b, r + 4), lef32(b, r + 8), lef32(b, r + 12)}});
    }
    return out;
  }
};

struct Merged {
  std::vector<Weight> entries;
  std::vector<int> remap;
  int step = 1;
};

Merged mergeWeights(const std::vector<Weight> &entries, std::size_t cap) {
  for (int step : {1, 2, 5, 10, 15, 20, 25, 34, 50, 100}) {
    std::map<Weight, int> index;
    Merged m;
    m.step = step;
    for (const auto &e : entries) {
      Weight key;
      if (e.empty()) {
        key = {{0, 100}};
      } else {
        std::vector<std::pair<int, int>> infl;
        for (const auto &[bone, w] : e) {
          const long long r = std::max<long long>(0, pyround(double(w) / double(step)) * step);
          if (r > 0) infl.push_back({bone, int(r)});
        }
        if (infl.empty()) {
          // max(e, key=rate): the first of the largest
          auto best = e.begin();
          for (auto it = e.begin(); it != e.end(); ++it)
            if (it->second > best->second) best = it;
          infl = {{best->first, 100}};
        }
        long long total = 0;
        for (const auto &x : infl) total += x.second;
        std::vector<std::pair<int, double>> scaled;
        for (const auto &x : infl) scaled.push_back({x.first, double(x.second) * 100.0 / double(total)});
        std::stable_sort(scaled.begin(), scaled.end(), [](const auto &a, const auto &b) { return -a.second < -b.second; });
        std::vector<int> rates;
        long long sum = 0;
        for (const auto &x : scaled) rates.push_back(int(pyround(x.second))), sum += rates.back();
        rates[0] += int(100 - sum);
        for (std::size_t k = 0; k < scaled.size(); ++k) key.push_back({scaled[k].first, rates[k]});
        std::sort(key.begin(), key.end());
      }
      auto it = index.find(key);
      if (it == index.end()) {
        it = index.emplace(key, int(m.entries.size())).first;
        m.entries.push_back(key);
      }
      m.remap.push_back(it->second);
    }
    if (m.entries.size() <= cap) return m;
  }
  fail("weights do not fit " + std::to_string(cap) + " entries");
}

// the disc model's parts records (16 bytes each, big-endian as they are)
std::vector<Bytes> gcBoneRecords(const Bytes &gcModel) {
  std::vector<Bytes> out;
  const unsigned n = at8(gcModel, 0x19);
  for (unsigned i = 0; i < n; ++i)
    out.emplace_back(gcModel.begin() + long(0x60 + i * 16), gcModel.begin() + long(0x70 + i * 16));
  return out;
}

// Per PC part: the disc's part it is drawn with (rebind_map); `moved` the PC parts that are not alike on the disc
std::map<int, int> rebindMap(const Bytes &gcModel, const Bytes &pcModel, std::vector<int> *moved) {
  std::map<int, std::pair<int, std::array<float, 3>>> gb;
  for (const auto &r : gcBoneRecords(gcModel)) gb[r[0]] = {r[1], {bef32(r, 4), bef32(r, 8), bef32(r, 12)}};
  std::map<int, std::pair<int, std::array<float, 3>>> pc;
  for (const auto &[raw, xyz] : Model(pcModel).bones()) pc[raw[0]] = {raw[1], xyz};
  std::set<int> same;
  for (const auto &[bone, v] : pc) {
    auto it = gb.find(bone);
    if (it == gb.end() || it->second.first != v.first) continue;
    float d = 0;
    for (int k = 0; k < 3; ++k) d = std::max(d, std::fabs(it->second.second[std::size_t(k)] - v.second[std::size_t(k)]));
    if (d <= 0.5f) same.insert(bone);
  }
  if (same.empty()) fail("the skeletons have no part in common");
  std::map<int, int> out;
  for (const auto &[bone, v] : pc) {
    int a = bone, hops = 0;
    while (!same.count(a) && pc.count(a) && hops < 256) a = pc[a].first, ++hops;
    out[bone] = same.count(a) ? a : *same.begin();
    if (!same.count(bone) && moved) moved->push_back(bone);
  }
  return out;
}

std::string checkSkeleton(const Bytes &gcModel, const Bytes &pcModel, const Options &o, std::string &log) {
  std::vector<std::tuple<int, int, std::array<float, 3>>> gb, pb;
  for (const auto &r : gcBoneRecords(gcModel)) gb.push_back({r[0], r[1], {bef32(r, 4), bef32(r, 8), bef32(r, 12)}});
  for (const auto &[raw, xyz] : Model(pcModel).bones()) pb.push_back({raw[0], raw[1], xyz});
  if (gb.size() != pb.size()) return fmt("parts %zu (disc) and %zu (PC)", gb.size(), pb.size());
  std::vector<int> moved;
  for (std::size_t i = 0; i < gb.size(); ++i) {
    const auto &[ga, gp, gx] = gb[i];
    const auto &[pa, pp, px] = pb[i];
    float d = 0;
    for (int k = 0; k < 3; ++k) d = std::max(d, std::fabs(gx[std::size_t(k)] - px[std::size_t(k)]));
    if (ga != pa || gp != pp || d > 0.5f) {
      if ((o.allFree || o.bonesFree.count(int(i))) && ga == pa && gp == pp) {
        moved.push_back(int(i));
        continue;
      }
      return fmt("part %zu differs (disc %d/%d, PC %d/%d)", i, ga, gp, pa, pp);
    }
  }
  if (!moved.empty()) {
    std::string list;
    for (int m : moved) list += (list.empty() ? "" : ", ") + std::to_string(m);
    log += "the PC skeleton's parts " + list + " lie elsewhere: the model keeps them, the disc's motions drive them\n";
  }
  return "";
}

struct Converted {
  Bytes blob;
  std::string stats;
};

Converted convertModel(const Bytes &blob, std::size_t cap, const Bytes *rebind) {
  const Model m(blob);
  const unsigned nv = m.nvtx;
  std::vector<std::array<double, 3>> pos(nv), nrm(nv);
  std::vector<std::array<double, 2>> uv(nv);
  for (unsigned i = 0; i < nv; ++i) {
    pos[i] = m.f3(m.vtx, i);
    auto n = m.f3(m.nrm, i);
    double l = std::hypot(n[0], n[1], n[2]);
    if (l == 0) l = 1.0;
    nrm[i] = {n[0] / l, n[1] / l, n[2] / l};
    uv[i] = {lef32(blob, m.tex + i * 8), lef32(blob, m.tex + i * 8 + 4)};
  }
  const bool hasClr = (m.flags & 0x80000000u) != 0;
  std::vector<std::array<std::uint8_t, 4>> clr;
  double uvScale;
  int uvLo, uvHi;
  if (hasClr) {
    for (unsigned i = 0; i < nv; ++i)
      clr.push_back({at8(blob, m.clr + i * 4), at8(blob, m.clr + i * 4 + 1), at8(blob, m.clr + i * 4 + 2),
                     at8(blob, m.clr + i * 4 + 3)});
    uvScale = 256.0, uvLo = -32768, uvHi = 32767;
  } else {
    uvScale = 32768.0, uvLo = 0, uvHi = 65535;
  }
  std::vector<unsigned> vw(nv, 0);
  if (m.vidx)
    for (unsigned i = 0; i < nv; ++i) vw[i] = le16(blob, m.vidx + i * 2);
  std::vector<Weight> weights = m.weights();
  if (rebind) {
    const auto bmap = rebindMap(*rebind, blob, nullptr);
    for (auto &e : weights) {
      std::vector<std::pair<int, int>> acc;  // insertion order, as Python's dict
      for (const auto &[bone, w] : e) {
        auto f = bmap.find(bone);
        const int to = f != bmap.end() ? f->second : bone;
        auto it = std::find_if(acc.begin(), acc.end(), [to](const auto &x) { return x.first == to; });
        if (it == acc.end()) acc.push_back({to, w});
        else it->second += w;
      }
      std::stable_sort(acc.begin(), acc.end(), [](const auto &a, const auto &b) { return -a.second < -b.second; });
      e = acc;
    }
  }
  Merged merged;
  if (!weights.empty()) {
    merged = mergeWeights(weights, cap);
  } else {
    merged.entries = {{{0, 100}}};
    merged.remap = {0};
  }
  for (auto &w : vw) w = w < merged.remap.size() ? unsigned(merged.remap[w]) : 0u;
  const bool s8 = merged.entries.size() <= 0xFF;
  double big = 1.0;
  for (const auto &p : pos)
    for (double c : p) big = std::max(big, std::fabs(c));
  int shift = std::max(0, std::min(15, int(std::floor(std::log2(32767.0 / big)))));
  if (m.shift && big * double(1 << m.shift) <= 32767) shift = int(m.shift);
  const double ps = double(1 << shift);

  // shapes: the deltas in the new shift; a vertex keeps its own position when its deltas differ (the lips)
  const double dscale = double(1 << shift) / double(1 << m.shift);
  std::vector<std::map<unsigned, std::array<int, 3>>> deltas;
  for (const auto &sd : m.shapes) {
    std::map<unsigned, std::array<int, 3>> d;
    for (const auto &[v, c] : sd)
      d[v] = {int(pyround(c[0] * dscale)), int(pyround(c[1] * dscale)), int(pyround(c[2] * dscale))};
    deltas.push_back(std::move(d));
  }
  // positions: (x, y, z, weight) unique, with the shape signature in the key
  std::map<std::vector<int>, unsigned> pIndex;
  std::vector<std::array<int, 4>> P;
  std::vector<unsigned> pref(nv);
  for (unsigned v = 0; v < nv; ++v) {
    std::vector<int> key = {quant(pos[v][0], ps), quant(pos[v][1], ps), quant(pos[v][2], ps), int(vw[v])};
    for (const auto &sd : deltas) {
      auto it = sd.find(v);
      if (it == sd.end()) key.insert(key.end(), {0, 0, 0, 0});
      else key.insert(key.end(), {1, it->second[0], it->second[1], it->second[2]});
    }
    auto it = pIndex.find(key);
    if (it == pIndex.end()) {
      it = pIndex.emplace(key, unsigned(P.size())).first;
      P.push_back({key[0], key[1], key[2], key[3]});
    }
    pref[v] = it->second;
  }
  std::map<std::array<int, 4>, unsigned> nIndex;
  std::vector<std::array<int, 4>> N;
  std::vector<unsigned> nref(nv);
  for (unsigned v = 0; v < nv; ++v) {
    std::array<int, 4> key = s8 ? std::array<int, 4>{quant(nrm[v][0], 64, -128, 127), quant(nrm[v][1], 64, -128, 127),
                                                    quant(nrm[v][2], 64, -128, 127), int(vw[v])}
                                : std::array<int, 4>{quant(nrm[v][0], 16384), quant(nrm[v][1], 16384),
                                                    quant(nrm[v][2], 16384), int(vw[v])};
    auto it = nIndex.find(key);
    if (it == nIndex.end()) {
      it = nIndex.emplace(key, unsigned(N.size())).first;
      N.push_back(key);
    }
    nref[v] = it->second;
  }
  std::map<std::array<int, 2>, unsigned> tIndex;
  std::vector<std::array<int, 2>> T;
  std::vector<unsigned> tref(nv);
  for (unsigned v = 0; v < nv; ++v) {
    std::array<int, 2> key{quant(uv[v][0], uvScale, uvLo, uvHi), quant(uv[v][1], uvScale, uvLo, uvHi)};
    auto it = tIndex.find(key);
    if (it == tIndex.end()) {
      it = tIndex.emplace(key, unsigned(T.size())).first;
      T.push_back(key);
    }
    tref[v] = it->second;
  }
  std::map<std::array<std::uint8_t, 4>, unsigned> cIndex;
  std::vector<std::array<std::uint8_t, 4>> C;
  std::vector<unsigned> cref(nv);
  if (hasClr) {
    for (unsigned v = 0; v < nv; ++v) {
      auto it = cIndex.find(clr[v]);
      if (it == cIndex.end()) {
        it = cIndex.emplace(clr[v], unsigned(C.size())).first;
        C.push_back(clr[v]);
      }
      cref[v] = it->second;
    }
  }
  if (std::max(P.size(), N.size()) > 0xFFFF) fail(fmt("more than 65535 vertices after merging (%zu)", P.size()));

  const std::uint32_t flags = (m.flags & ~0x20000000u) | (s8 ? 0x20000000u : 0u);
  Bytes out(0x60, 0);
  std::map<std::string, std::uint32_t> sec;
  auto put = [&](const std::string &name, const Bytes &data, std::size_t a) {
    pad(out, a);
    sec[name] = std::uint32_t(out.size());
    out.insert(out.end(), data.begin(), data.end());
  };
  Bytes head;
  if (rebind) {
    for (const auto &r : gcBoneRecords(*rebind)) head.insert(head.end(), r.begin(), r.end());
  } else {
    for (const auto &[raw, xyz] : m.bones()) {
      head.insert(head.end(), raw.begin(), raw.end());
      for (float f : xyz) putf(head, f);
    }
  }
  put("head", head, 16);
  Bytes vb;
  for (const auto &p : P) put16(vb, unsigned(p[0] & 0xFFFF)), put16(vb, unsigned(p[1] & 0xFFFF)),
      put16(vb, unsigned(p[2] & 0xFFFF)), put16(vb, unsigned(p[3]));
  put("vtx", vb, 32);
  Bytes nb;
  for (const auto &n : N) {
    if (s8) put8(nb, unsigned(n[0] & 0xFF)), put8(nb, unsigned(n[1] & 0xFF)), put8(nb, unsigned(n[2] & 0xFF)),
        put8(nb, unsigned(n[3]));
    else put16(nb, unsigned(n[0] & 0xFFFF)), put16(nb, unsigned(n[1] & 0xFFFF)), put16(nb, unsigned(n[2] & 0xFFFF)),
        put16(nb, unsigned(n[3]));
  }
  put("nrm", nb, 32);
  if (!C.empty()) {
    Bytes cb;
    for (const auto &c : C) cb.insert(cb.end(), c.begin(), c.end());
    put("clr", cb, 32);
  }
  Bytes tb;
  for (const auto &t : T) put16(tb, unsigned(t[0] & 0xFFFF)), put16(tb, unsigned(t[1] & 0xFFFF));
  put("tex", tb, 32);
  Bytes wb;
  for (const auto &e : merged.entries) {
    std::array<int, 3> ids{0, 0, 0}, rates{0, 0, 0};
    for (std::size_t k = 0; k < e.size() && k < 3; ++k) ids[k] = e[k].first, rates[k] = e[k].second;
    if (merged.entries.size() <= 0xFF) {
      put8(wb, unsigned(ids[0])), put8(wb, unsigned(ids[1])), put8(wb, unsigned(ids[2])), put8(wb, unsigned(e.size()));
    } else {
      put16(wb, unsigned(ids[0])), put16(wb, unsigned(ids[1])), put16(wb, unsigned(ids[2])), put16(wb, unsigned(e.size()));
    }
    put8(wb, unsigned(rates[0])), put8(wb, unsigned(rates[1])), put8(wb, unsigned(rates[2])), put8(wb, 0);
  }
  put("wgt", wb, 32);
  Bytes pb;
  unsigned first = 0;
  std::uint64_t totalPolys = 0;
  for (const auto &r : m.runs()) {
    Bytes stream;
    for (const auto &[type, cnt] : r.runs) {
      const int op = prim(type);
      if (op < 0) fail("unknown primitive type " + std::to_string(type));
      put8(stream, unsigned(op)), put16(stream, cnt);
      for (unsigned v = first; v < first + cnt; ++v) {
        if (v >= nv) fail("a run past the vertices");
        put16(stream, pref[v]), put16(stream, nref[v]);
        if (!C.empty()) put16(stream, cref[v]);
        put16(stream, tref[v]);
      }
      first += cnt;
    }
    pad(stream);
    pb.insert(pb.end(), r.raw.begin(), r.raw.end());
    put32(pb, std::uint32_t(stream.size())), put32(pb, r.npoly);
    pb.insert(pb.end(), stream.begin(), stream.end());
    totalPolys += r.npoly;
  }
  if (first != nv) fail(fmt("the runs use %u of %u vertices", first, nv));
  put("parts", pb, 32);
  if (rebind && be32(*rebind, 0x3C) == 0x20030818 && be32(*rebind, 0x40)) {
    const std::uint32_t at = be32(*rebind, 0x40), n = be32(*rebind, at);
    put("blend", Bytes(rebind->begin() + at, rebind->begin() + at + 4 + n * 8), 4);
  } else if (!rebind && m.blend) {
    const std::uint32_t n = le32(blob, m.blend);
    Bytes bl;
    put32(bl, n);
    for (std::uint32_t k = 0; k < n * 4; ++k) put16(bl, le16(blob, m.blend + 4 + k * 2));
    put("blend", bl, 4);
  }
  std::string shapeNote;
  if (!m.shapes.empty()) {
    std::vector<std::vector<std::pair<unsigned, std::array<int, 3>>>> lists;
    for (const auto &sd : deltas) {
      std::map<unsigned, std::array<int, 3>> seen;
      for (const auto &[v, d] : sd)
        if (v < nv && (d[0] || d[1] || d[2])) seen.emplace(pref[v], d);
      lists.emplace_back(seen.begin(), seen.end());
    }
    Bytes table;
    put32(table, std::uint32_t(lists.size()));
    Bytes data;
    const std::uint32_t ofs = std::uint32_t(8 * lists.size());
    for (const auto &lst : lists) {
      put32(table, ofs + std::uint32_t(data.size())), put32(table, std::uint32_t(lst.size()));
      for (const auto &[v, d] : lst) {
        put16(data, v);
        for (int c : d) put16(data, unsigned(std::max(-32768, std::min(32767, c)) & 0xFFFF));
      }
    }
    table.insert(table.end(), data.begin(), data.end());
    put("shape", table, 4);
    std::size_t bad = 0;
    std::string counts;
    for (std::size_t k = 0; k < deltas.size(); ++k) {
      std::map<unsigned, std::array<int, 3>> got(lists[k].begin(), lists[k].end());
      for (unsigned v = 0; v < nv; ++v) {
        auto a = deltas[k].find(v);
        auto g = got.find(pref[v]);
        const std::array<int, 3> d = a != deltas[k].end() ? a->second : std::array<int, 3>{0, 0, 0};
        const std::array<int, 3> gv = g != got.end() ? g->second : std::array<int, 3>{0, 0, 0};
        bad += d != gv;
      }
      counts += (counts.empty() ? "" : "/") + std::to_string(lists[k].size());
    }
    shapeNote = fmt(", %zu shapes (%s deltas), %zu vertices off", lists.size(), counts.c_str(), bad);
    if (bad) fail(fmt("shape deltas do not survive the merge (%zu vertices)", bad));
  }
  if (rebind) {
    if (be32(*rebind, 0x3C) == 0x20030818 && be32(*rebind, 0x44)) {
      const std::uint32_t at = be32(*rebind, 0x44), n = be32(*rebind, at);
      put("flip", Bytes(rebind->begin() + at, rebind->begin() + at + 4 + n * 2), 4);
    }
  } else if (m.flip) {
    const std::uint32_t n = be32(blob, m.flip);  // the PC keeps it big-endian, as on the disc
    put("flip", Bytes(blob.begin() + m.flip, blob.begin() + m.flip + 4 + n * 2), 4);
  }
  pad(out);
  auto get = [&](const char *k) { auto it = sec.find(k); return it != sec.end() ? it->second : 0u; };
  Bytes hdr;
  put32(hdr, sec["head"]), put32(hdr, m.x04), put32(hdr, m.version == 0x20030818 ? 0x50u : m.x08);
  put32(hdr, get("clr")), put32(hdr, sec["tex"]), put32(hdr, sec["wgt"]);
  put8(hdr, unsigned(merged.entries.size() & 0xFF)), put8(hdr, rebind ? at8(*rebind, 0x19) : m.nparts),
      put16(hdr, m.nlist);
  put32(hdr, sec["parts"]), put32(hdr, flags), put32(hdr, m.ntex);
  put8(hdr, unsigned(shift)), put8(hdr, m.x29), put16(hdr, unsigned(merged.entries.size() & 0xFFFF));
  put32(hdr, get("shape")), put32(hdr, sec["vtx"]), put32(hdr, sec["nrm"]);
  put16(hdr, unsigned(P.size())), put16(hdr, unsigned(N.size())), put32(hdr, m.version);
  if (m.version == 0x20030818) put32(hdr, get("blend")), put32(hdr, get("flip"));
  std::copy(hdr.begin(), hdr.end(), out.begin());
  Converted c;
  c.stats = fmt("%zu vertices (PC %u), %zu normals, %zu texture coordinates, %llu polygons, weights %zu -> %zu "
                "(step %d%%), shift %d, %s normals%s, %zu bytes (PC %zu)",
                P.size(), nv, N.size(), T.size(), (unsigned long long)totalPolys, weights.size(),
                merged.entries.size(), merged.step, shift, s8 ? "s8" : "s16", shapeNote.c_str(), out.size(),
                blob.size());
  c.blob = std::move(out);
  return c;
}

}  // namespace


namespace {

// ---- textures ------------------------------------------------------------------------------------------------------

struct Rgba {
  int w = 0, h = 0;
  std::vector<std::uint8_t> px;  // w * h * 4
};

void rgb565(unsigned c, std::uint8_t *o) {
  o[0] = std::uint8_t(((c >> 11) & 31) * 255 / 31), o[1] = std::uint8_t(((c >> 5) & 63) * 255 / 63),
  o[2] = std::uint8_t((c & 31) * 255 / 31);
}

// Pillow's 565 expansion (bcn decoder: the top bits repeated)
void pil565(unsigned v, std::uint8_t *c) {
  const unsigned r = (v & 0xF800) >> 8, g = (v & 0x07E0) >> 3, b = (v & 0x001F) << 3;
  c[0] = std::uint8_t(r | r >> 5), c[1] = std::uint8_t(g | g >> 6), c[2] = std::uint8_t(b | b >> 5);
}

// A DDS file (DXT1, DXT3, DXT5 or uncompressed RGB(A) with masks) as RGBA, as Pillow reads it
bool decodeDds(const Bytes &d, Rgba &img, std::string *why) {
  if (d.size() < 128 || std::memcmp(d.data(), "DDS ", 4) != 0) {
    if (why) *why = "not a DDS file";
    return false;
  }
  const std::uint32_t h = le32(d, 12), w = le32(d, 16), pfFlags = le32(d, 80), four = le32(d, 84),
                      bits = le32(d, 88);
  if (!w || !h || w > 8192 || h > 8192) {
    if (why) *why = "bad size";
    return false;
  }
  img.w = int(w), img.h = int(h);
  img.px.assign(std::size_t(w) * h * 4, 0);
  const std::size_t base = 128;
  if (pfFlags & 4) {
    const bool dxt1 = four == 0x31545844u, dxt3 = four == 0x33545844u, dxt5 = four == 0x35545844u;
    if (!dxt1 && !dxt3 && !dxt5) {
      if (why) *why = fmt("DDS kind %c%c%c%c", char(four), char(four >> 8), char(four >> 16), char(four >> 24));
      return false;
    }
    const std::size_t blockBytes = dxt1 ? 8 : 16;
    const std::uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    if (base + std::size_t(bw) * bh * blockBytes > d.size()) {
      if (why) *why = "truncated DDS";
      return false;
    }
    for (std::uint32_t by = 0; by < bh; ++by)
      for (std::uint32_t bx = 0; bx < bw; ++bx) {
        const std::size_t b = base + (std::size_t(by) * bw + bx) * blockBytes;
        std::uint8_t alpha[16];
        std::fill(alpha, alpha + 16, 255);
        std::size_t cb = b;
        if (dxt3) {
          for (int k = 0; k < 16; ++k) alpha[k] = std::uint8_t(((d[b + std::size_t(k / 2)] >> (4 * (k % 2))) & 15) * 17);
          cb = b + 8;
        } else if (dxt5) {
          const unsigned a0 = d[b], a1 = d[b + 1];
          std::uint64_t ab = 0;
          for (int k = 0; k < 6; ++k) ab |= std::uint64_t(d[b + 2 + std::size_t(k)]) << (8 * k);
          unsigned pal[8];
          pal[0] = a0, pal[1] = a1;
          if (a0 > a1)
            for (int k = 1; k < 7; ++k) pal[k + 1] = ((7 - unsigned(k)) * a0 + unsigned(k) * a1) / 7;
          else {
            for (int k = 1; k < 5; ++k) pal[k + 1] = ((5 - unsigned(k)) * a0 + unsigned(k) * a1) / 5;
            pal[6] = 0, pal[7] = 255;
          }
          for (int k = 0; k < 16; ++k) alpha[k] = std::uint8_t(pal[(ab >> (3 * k)) & 7]);
          cb = b + 8;
        }
        const unsigned c0 = unsigned(d[cb] | d[cb + 1] << 8), c1 = unsigned(d[cb + 2] | d[cb + 3] << 8);
        const std::uint32_t idx = le32(d, cb + 4);
        std::uint8_t pal[4][4];
        pil565(c0, pal[0]), pil565(c1, pal[1]);
        pal[0][3] = pal[1][3] = 255;
        if (c0 > c1 || !dxt1) {
          for (int k = 0; k < 3; ++k) {
            pal[2][k] = std::uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = std::uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
          }
          pal[2][3] = pal[3][3] = 255;
        } else {
          for (int k = 0; k < 3; ++k) pal[2][k] = std::uint8_t((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
          pal[2][3] = 255, pal[3][3] = 0;
        }
        for (int k = 0; k < 16; ++k) {
          const std::uint32_t x = bx * 4 + std::uint32_t(k % 4), y = by * 4 + std::uint32_t(k / 4);
          if (x >= w || y >= h) continue;
          const auto *c = pal[(idx >> (2 * k)) & 3];
          auto *o = &img.px[(std::size_t(y) * w + x) * 4];
          o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = dxt1 ? c[3] : alpha[k];
        }
      }
    return true;
  }
  if (pfFlags & 0x40) {  // RGB with masks
    const std::uint32_t rm = le32(d, 92), gm = le32(d, 96), bm = le32(d, 100), am = (pfFlags & 1) ? le32(d, 104) : 0;
    if (bits != 32 && bits != 24) {
      if (why) *why = fmt("DDS %u-bit colour", bits);
      return false;
    }
    const std::size_t step = bits / 8;
    if (base + std::size_t(w) * h * step > d.size()) {
      if (why) *why = "truncated DDS";
      return false;
    }
    auto chan = [](std::uint32_t v, std::uint32_t mask) -> std::uint8_t {
      if (!mask) return 255;
      int sh = 0;
      while (!((mask >> sh) & 1)) ++sh;
      const std::uint32_t max = mask >> sh;
      return std::uint8_t(((v & mask) >> sh) * 255 / max);
    };
    for (std::uint32_t i = 0; i < w * h; ++i) {
      std::uint32_t v = 0;
      for (std::size_t k = 0; k < step; ++k) v |= std::uint32_t(d[base + i * step + k]) << (8 * k);
      auto *o = &img.px[std::size_t(i) * 4];
      o[0] = chan(v, rm), o[1] = chan(v, gm), o[2] = chan(v, bm), o[3] = am ? chan(v, am) : 255;
    }
    return true;
  }
  if (why) *why = "DDS kind not read here";
  return false;
}

// A TGA image (true colour, 24 or 32 bits, plain or run-length; the PC packs hold some among the DDS) as Pillow reads it
bool decodeTga(const Bytes &d, Rgba &img, std::string *why) {
  if (d.size() < 18) return false;
  const unsigned idLen = d[0], cmap = d[1], type = d[2], w = le16(d, 12), h = le16(d, 14), bpp = d[16], desc = d[17];
  if (cmap || (type != 2 && type != 10) || (bpp != 24 && bpp != 32) || !w || !h || w > 8192 || h > 8192) {
    if (why) *why = "not a DDS or TGA image this reads";
    return false;
  }
  const std::size_t step = bpp / 8, count = std::size_t(w) * h;
  std::size_t p = 18 + idLen;
  std::vector<std::uint8_t> raw(count * 4);
  auto pixel = [&](std::size_t i, std::size_t at) {
    raw[i * 4] = d[at + 2], raw[i * 4 + 1] = d[at + 1], raw[i * 4 + 2] = d[at], raw[i * 4 + 3] = step == 4 ? d[at + 3] : 255;
  };
  if (type == 2) {
    if (p + count * step > d.size()) {
      if (why) *why = "truncated TGA";
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) pixel(i, p + i * step);
  } else {
    std::size_t i = 0;
    while (i < count) {
      if (p >= d.size()) {
        if (why) *why = "truncated TGA";
        return false;
      }
      const unsigned head = d[p++], n = (head & 0x7F) + 1;
      if (head & 0x80) {
        if (p + step > d.size()) return false;
        for (unsigned k = 0; k < n && i < count; ++k) pixel(i++, p);
        p += step;
      } else {
        if (p + n * step > d.size()) return false;
        for (unsigned k = 0; k < n && i < count; ++k) pixel(i++, p + k * step);
        p += n * step;
      }
    }
  }
  img.w = int(w), img.h = int(h);
  img.px.resize(count * 4);
  const bool top = desc & 0x20, right = desc & 0x10;
  for (unsigned y = 0; y < h; ++y)
    for (unsigned x = 0; x < w; ++x) {
      const unsigned sy = top ? y : h - 1 - y, sx = right ? w - 1 - x : x;
      std::memcpy(&img.px[(std::size_t(y) * w + x) * 4], &raw[(std::size_t(sy) * w + sx) * 4], 4);
    }
  return true;
}

// Pillow's resize of an RGBA image (bicubic, a = -0.5, its support widened when shrinking; premultiplied alpha)
double cubic(double x) {
  const double a = -0.5;
  x = std::fabs(x);
  if (x < 1) return ((a + 2) * x - (a + 3)) * x * x + 1;
  if (x < 2) return (((x - 5) * x + 8) * x - 4) * a;
  return 0;
}

std::vector<double> resample1d(const std::vector<double> &src, int inSize, int outSize, int stride, int lines,
                               bool horizontal) {
  // src: horizontal: lines rows of inSize * 4 values; vertical: inSize rows of stride values (stride = lines * 4)
  std::vector<double> out(horizontal ? std::size_t(lines) * outSize * 4 : std::size_t(outSize) * stride);
  const double scale = double(inSize) / outSize, filterscale = std::max(1.0, scale), support = 2.0 * filterscale;
  for (int o = 0; o < outSize; ++o) {
    const double center = (o + 0.5) * scale;
    const int xmin = std::max(0, int(std::floor(center - support + 0.5)));
    const int xmax = std::min(inSize, int(std::floor(center + support + 0.5)));
    std::vector<double> k;
    double ww = 0;
    for (int x = xmin; x < xmax; ++x) {
      const double v = cubic((x - center + 0.5) / filterscale);
      k.push_back(v);
      ww += v;
    }
    if (ww != 0)
      for (auto &v : k) v /= ww;
    if (horizontal) {
      for (int l = 0; l < lines; ++l)
        for (int c = 0; c < 4; ++c) {
          double acc = 0;
          for (int x = xmin; x < xmax; ++x) acc += src[(std::size_t(l) * inSize + std::size_t(x)) * 4 + std::size_t(c)] * k[std::size_t(x - xmin)];
          out[(std::size_t(l) * outSize + std::size_t(o)) * 4 + std::size_t(c)] = acc;
        }
    } else {
      for (int i = 0; i < stride; ++i) {
        double acc = 0;
        for (int x = xmin; x < xmax; ++x) acc += src[std::size_t(x) * std::size_t(stride) + std::size_t(i)] * k[std::size_t(x - xmin)];
        out[std::size_t(o) * std::size_t(stride) + std::size_t(i)] = acc;
      }
    }
  }
  return out;
}

Rgba resize(const Rgba &in, int tw, int th) {
  if (in.w == tw && in.h == th) return in;
  std::vector<double> src(in.px.size());
  for (std::size_t i = 0; i < in.px.size(); i += 4) {
    const double a = in.px[i + 3] / 255.0;
    src[i] = in.px[i] * a, src[i + 1] = in.px[i + 1] * a, src[i + 2] = in.px[i + 2] * a, src[i + 3] = in.px[i + 3];
  }
  const auto hor = resample1d(src, in.w, tw, 0, in.h, true);
  const auto ver = resample1d(hor, in.h, th, tw * 4, tw, false);
  Rgba out;
  out.w = tw, out.h = th;
  out.px.resize(std::size_t(tw) * th * 4);
  auto clip = [](double v) { return std::uint8_t(std::max(0.0, std::min(255.0, std::floor(v + 0.5)))); };
  for (std::size_t i = 0; i < out.px.size(); i += 4) {
    const double a = std::max(0.0, std::min(255.0, ver[i + 3]));
    const std::uint8_t ab = clip(a);
    out.px[i + 3] = ab;
    for (int c = 0; c < 3; ++c) out.px[i + std::size_t(c)] = ab ? clip(ver[i + std::size_t(c)] * 255.0 / std::max(1.0, a)) : 0;
  }
  return out;
}

unsigned c565(const std::uint8_t *c) { return unsigned((c[0] >> 3) << 11 | (c[1] >> 2) << 5 | c[2] >> 3); }

Bytes encCmpr(const Rgba &img) {
  Bytes out;
  const int w = img.w, h = img.h;
  for (int ty = 0; ty < h; ty += 8)
    for (int tx = 0; tx < w; tx += 8)
      for (auto [sx, sy] : {std::pair{0, 0}, std::pair{4, 0}, std::pair{0, 4}, std::pair{4, 4}}) {
        std::array<const std::uint8_t *, 16> cols;
        for (int y = 0; y < 4; ++y)
          for (int x = 0; x < 4; ++x) cols[std::size_t(y * 4 + x)] = &img.px[(std::size_t(ty + sy + y) * w + std::size_t(tx + sx + x)) * 4];
        bool transparent = false;
        std::vector<const std::uint8_t *> opaque;
        for (auto *c : cols) {
          if (c[3] < 128) transparent = true;
          else opaque.push_back(c);
        }
        if (opaque.empty()) opaque.assign(cols.begin(), cols.end());
        auto lum = [](const std::uint8_t *c) { return c[0] * 3 + c[1] * 6 + c[2]; };
        const std::uint8_t *a = opaque[0], *b = opaque[0];
        for (auto *c : opaque) {  // Python's max / min: the first of the largest / smallest
          if (lum(c) > lum(a)) a = c;
          if (lum(c) < lum(b)) b = c;
        }
        unsigned c0 = c565(a), c1 = c565(b);
        if (transparent) {
          if (c0 > c1) std::swap(c0, c1);
        } else {
          if (c0 < c1) std::swap(c0, c1);
          if (c0 == c1) {
            c1 = c0 ? c0 - 1 : 0;
            if (c0 == 0) c0 = 1;
          }
        }
        std::uint8_t p0[3], p1[3];
        rgb565(c0, p0), rgb565(c1, p1);
        int pal[4][3];
        int n;
        for (int k = 0; k < 3; ++k) pal[0][k] = p0[k], pal[1][k] = p1[k];
        if (c0 > c1) {
          for (int k = 0; k < 3; ++k) pal[2][k] = (2 * p0[k] + p1[k]) / 3, pal[3][k] = (p0[k] + 2 * p1[k]) / 3;
          n = 4;
        } else {
          for (int k = 0; k < 3; ++k) pal[2][k] = (p0[k] + p1[k]) / 2;
          n = 3;
        }
        std::uint32_t bits = 0;
        for (auto *c : cols) {
          int k;
          if (transparent && c[3] < 128) {
            k = 3;
          } else {
            k = 0;
            long best = -1;
            for (int i = 0; i < n; ++i) {
              long dd = 0;
              for (int j = 0; j < 3; ++j) dd += long(c[j] - pal[i][j]) * long(c[j] - pal[i][j]);
              if (best < 0 || dd < best) best = dd, k = i;
            }
          }
          bits = bits << 2 | unsigned(k);
        }
        put16(out, c0), put16(out, c1), put32(out, bits);
      }
  return out;
}

Bytes encI8(const Rgba &img) {
  bool useAlpha = false;
  for (std::size_t i = 3; i < img.px.size(); i += 4) useAlpha = useAlpha || img.px[i] < 255;
  Bytes out;
  for (int by = 0; by < img.h; by += 4)
    for (int bx = 0; bx < img.w; bx += 8)
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) {
          const auto *c = &img.px[(std::size_t(by + y) * img.w + std::size_t(bx + x)) * 4];
          out.push_back(useAlpha ? c[3] : std::uint8_t((c[0] * 3 + c[1] * 6 + c[2]) / 10));
        }
  return out;
}

Bytes encIa8(const Rgba &img) {
  Bytes out;
  for (int by = 0; by < img.h; by += 4)
    for (int bx = 0; bx < img.w; bx += 4)
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const auto *c = &img.px[(std::size_t(by + y) * img.w + std::size_t(bx + x)) * 4];
          out.push_back(c[3]), out.push_back(std::uint8_t((c[0] * 3 + c[1] * 6 + c[2]) / 10));
        }
  return out;
}

// ENCODERS: the PC format -> the GX format and its encoder
int gxFormat(std::uint32_t pcFormat) {
  switch (pcFormat) {
  case 0x0:
  case 0x1: return 0x1;  // I8
  case 0x3: return 0x3;  // IA8
  default: return 0xE;   // CMPR
  }
}
Bytes encode(int gx, const Rgba &img) { return gx == 0x1 ? encI8(img) : gx == 0x3 ? encIa8(img) : encCmpr(img); }

const std::map<int, int> &GxBits() {
  static const std::map<int, int> m = {{0x0, 4}, {0x1, 8}, {0x2, 8}, {0x3, 16}, {0x4, 16}, {0x5, 16}, {0x6, 32},
                                       {0xE, 4}};
  return m;
}

struct DiscImage {
  unsigned h, w, f;
  Bytes data;
};

std::vector<DiscImage> discTextures(const Bytes &tpl) {
  std::vector<DiscImage> out;
  if (tpl.size() < 12 || be32(tpl, 0) != 0x0020AF30) return out;
  const std::uint32_t n = be32(tpl, 4);
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t ho = be32(tpl, 12 + i * 8);
    const unsigned h = be16(tpl, ho), w = be16(tpl, ho + 2), f = be32(tpl, ho + 4), data = be32(tpl, ho + 8);
    auto it = GxBits().find(int(f));
    if (it == GxBits().end()) continue;
    const std::size_t len = std::size_t(w) * h * std::size_t(it->second) / 8;
    if (data + len > tpl.size()) continue;
    out.push_back({h, w, f, Bytes(tpl.begin() + data, tpl.begin() + long(data + len))});
  }
  return out;
}

std::size_t tplFound(const Bytes &blob, const Packs &packs) {
  const std::uint32_t n = le32(blob, 4);
  std::size_t found = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t ho = le32(blob, 12 + i * 8), data = le32(blob, ho + 8);
    if (data + 8 <= blob.size() && packs.has(le32(blob, data))) ++found;
  }
  return found;
}

Bytes convertTpl(const Bytes &blob, const Packs &packs, const Options &o, std::vector<std::string> &stats,
                 std::vector<std::string> &notes, const Bytes &gcTpl, const std::set<int> &colourOnly) {
  const std::uint32_t n = le32(blob, 4);
  const auto disc = discTextures(gcTpl);
  const DiscImage *discColour = nullptr;
  for (const auto &t : disc)
    if (t.f == 0xE && (!discColour || t.h * t.w > discColour->h * discColour->w)) discColour = &t;
  struct Out {
    unsigned h, w, gx;
    Bytes data;
    std::uint32_t ws, wt;
    float bias;
  };
  std::vector<Out> images;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t ho = le32(blob, 12 + i * 8);
    const unsigned h = le16(blob, ho), w = le16(blob, ho + 2);
    const std::uint32_t pcFmt = le32(blob, ho + 4), data = le32(blob, ho + 8), ws = le32(blob, ho + 12),
                        wt = le32(blob, ho + 16);
    const float bias = lef32(blob, ho + 28);
    std::uint32_t ref = 0, idx = 0;
    if (data + 8 <= blob.size()) ref = le32(blob, data), idx = le32(blob, data + 4);
    Packs::Image pi;
    std::string why;
    bool got = packs.size() && packs.image(ref, idx, pi, &why);
    if (!got && !why.empty() && packs.has(ref)) notes.push_back(fmt("  texture %#x/%u: %s", ref, idx, why.c_str()));
    int gx = gxFormat(pcFmt);
    if (gx == 0x1 && colourOnly.count(int(i))) gx = 0xE;
    if (!got && gx == 0xE && discColour) {
      images.push_back({discColour->h, discColour->w, discColour->f, discColour->data, ws, wt, bias});
      stats.push_back(fmt("  texture %u: missing (%#x/%u), the disc's %ux%u", i, ref, idx, discColour->w,
                          discColour->h));
      continue;
    }
    Rgba img;
    bool missing = false;
    if (!got) {
      img.w = img.h = 8;
      img.px.assign(8 * 8 * 4, 0);
      for (std::size_t k = 0; k < img.px.size(); k += 4) {
        const std::uint8_t g = gx == 0xE ? 128 : 255;
        img.px[k] = img.px[k + 1] = img.px[k + 2] = g, img.px[k + 3] = 255;
      }
      missing = true;
    } else {
      img.w = pi.width, img.h = pi.height, img.px = std::move(pi.rgba);
    }
    const int limitSet = gx == 0xE ? o.texColour : o.texOther;
    const int limit = limitSet ? limitSet : o.tex;
    const double scale = std::min(1.0, double(limit) / double(std::max(img.w, img.h)));
    const int tw = std::max(8, 1 << int(pyround(std::log2(std::max(1.0, img.w * scale)))));
    const int th = std::max(8, 1 << int(pyround(std::log2(std::max(1.0, img.h * scale)))));
    const Rgba scaled = resize(img, tw, th);
    images.push_back({unsigned(th), unsigned(tw), unsigned(gx), encode(gx, scaled), ws, wt, bias});
    if (missing)
      stats.push_back(fmt("  texture %u: missing (%#x/%u), %s %ux%u -> %dx%d format %#x", i, ref, idx,
                          gx == 0xE ? "grey" : "white", w, h, tw, th, unsigned(gx)));
    else
      stats.push_back(fmt("  texture %u: %#x/%u %ux%u -> %dx%d format %#x", i, ref, idx, w, h, tw, th, unsigned(gx)));
  }
  const std::size_t head = 12 + std::size_t(n) * 8;
  Bytes out(align(head + std::size_t(n) * 0x24), 0);
  set32(out, 0, 0x0020AF30), set32(out, 4, n), set32(out, 8, 12);
  for (std::size_t i = 0; i < images.size(); ++i) {
    const auto &im = images[i];
    pad(out);
    set32(out, 12 + i * 8, std::uint32_t(head + i * 0x24)), set32(out, 16 + i * 8, 0);
    Bytes hd;
    put16(hd, im.h), put16(hd, im.w), put32(hd, im.gx), put32(hd, std::uint32_t(out.size())), put32(hd, im.ws),
        put32(hd, im.wt), put32(hd, 1), put32(hd, 1), putf(hd, im.bias), put32(hd, 0);
    std::copy(hd.begin(), hd.end(), out.begin() + long(head + i * 0x24));
    out.insert(out.end(), im.data.begin(), im.data.end());
  }
  return out;
}

// ---- the archive ---------------------------------------------------------------------------------------------------

using TplUsers = std::map<int, std::vector<int>>;
const TplUsers &PlayerTplUsers() {
  static const TplUsers m = {{1, {0, 6, 9, 12}}, {3, {2, 4, 5}}};
  return m;
}
const std::map<std::string, TplUsers> &CharacterTplUsers() {
  static const std::map<std::string, TplUsers> m = {
      {"pl01", {{1, {0, 4, 6, 13, 16, 17}}, {5, {3}}, {7, {2}}}},
      {"pl05", {{1, {0, 4, 6, 13, 16, 17}}, {5, {3}}, {7, {2}}}},
      {"pl11", {{1, {0, 5, 6, 8, 9, 10, 11, 12}}, {4, {2}}, {7, {3}}}},
      {"pl15", {{1, {0, 5, 6, 8, 9, 10, 11, 12}}, {4, {2}}, {7, {3}}}},
      {"pl0b", {{1, {0, 13, 14, 15, 16, 17}}, {3, {2, 4, 7}}, {6, {5}}, {9, {8}}, {103, {102}}, {119, {118}}, {122, {121}}}},
      {"pl0c", {{1, {0, 13, 14, 15, 16, 17}}, {3, {2, 4, 7}}, {6, {5}}, {9, {8}}, {103, {102}}, {119, {118}}, {122, {121}}}},
  };
  return m;
}

std::set<int> colourSlots(const PcView &pc, std::size_t count, int t, const TplUsers &users, bool player) {
  std::vector<int> list;
  if (count >= 100 || !player) {
    auto it = users.find(t);
    if (it != users.end()) list = it->second;
  } else {
    list = {t - 1};
  }
  std::set<int> colour, other;
  for (int k : list) {
    if (k < 0 || std::size_t(k) >= count || pc.kind(std::size_t(k)) != "BIN") continue;
    const Bytes e = pc.entry(std::size_t(k));
    if (e.empty()) continue;
    try {
      const Model m(e);
      for (const auto &r : m.runs()) {
        colour.insert(r.raw[0xC]);
        for (int x : {int(r.raw[0xD]), int(r.raw[0xE])})
          if (x != 0xFF) other.insert(x);
        if (r.raw[0xB] & 0x10) other.insert(r.raw[0x17]);
      }
    } catch (const std::exception &) {
    }
  }
  std::set<int> out;
  for (int c : colour)
    if (!other.count(c)) out.insert(c);
  return out;
}

Bytes readFile(const std::string &path, std::uint64_t at, std::uint64_t len) {
  std::ifstream in(path, std::ios::binary);
  if (!in) fail("cannot read " + path);
  in.seekg(std::streamoff(at));
  Bytes b(len);
  in.read(reinterpret_cast<char *>(b.data()), std::streamsize(len));
  b.resize(std::size_t(in.gcount()));
  return b;
}

#include "studio_dff.inc"
#include "studio_remesh.inc"

// A model takes at most 65535 corners (the header's count is 16-bit): 21845 triangles. A skin with more (the RE4
// Remake's Leons have 40 000 to 80 000; DevJunior, 20.11.2026: "the Remake's models do not always work") is reduced
// to `target` triangles by edge collapses (meshoptimizer's simplifier, attribute-aware: the UVs and normals keep
// their seams). First each vertex is made one material's (a vertex two materials share is split): every collapse
// stays within a material, so a triangle's material is its first corner's. Unused vertices are dropped after.
void reduceSkin(DffMesh &m, std::size_t target, std::string &log) {
  const std::size_t before = m.tris.size();
  if (before <= target) return;
  // one material per vertex
  std::map<std::pair<unsigned, unsigned>, unsigned> split;  // (vertex, material) -> vertex
  std::vector<unsigned> vmat(m.pos.size(), ~0u);
  auto copyVertex = [&](unsigned v) {
    m.pos.push_back(m.pos[v]);
    if (v < m.nrm.size()) m.nrm.push_back(m.nrm[v]);
    if (v < m.uv.size()) m.uv.push_back(m.uv[v]);
    if (v < m.bone.size()) m.bone.push_back(m.bone[v]);
    if (v < m.weight.size()) m.weight.push_back(m.weight[v]);
    return unsigned(m.pos.size() - 1);
  };
  std::vector<std::array<unsigned, 4>> tris;
  for (auto t : m.tris) {
    if (t[0] >= m.pos.size() || t[1] >= m.pos.size() || t[2] >= m.pos.size()) continue;
    for (int c = 0; c < 3; ++c) {
      const unsigned v = t[c];
      if (vmat[v] == ~0u) vmat[v] = t[3];
      if (vmat[v] == t[3]) continue;
      auto it = split.find({v, t[3]});
      if (it == split.end()) {
        it = split.emplace(std::make_pair(v, t[3]), copyVertex(v)).first;
        vmat.push_back(t[3]);
      }
      t[c] = it->second;
    }
    tris.push_back(t);
  }
  const std::size_t nv = m.pos.size();
  m.nrm.resize(nv, V3{0, 1, 0}), m.uv.resize(nv, {0.0, 0.0});
  std::vector<float> P(nv * 3), A(nv * 5);
  for (std::size_t v = 0; v < nv; ++v) {
    const double p[3] = {m.pos[v].x, m.pos[v].y, m.pos[v].z};
    for (int k = 0; k < 3; ++k) P[v * 3 + k] = float(p[k]);
    A[v * 5] = float(m.uv[v][0]), A[v * 5 + 1] = float(m.uv[v][1]);
    A[v * 5 + 2] = float(m.nrm[v].x), A[v * 5 + 3] = float(m.nrm[v].y), A[v * 5 + 4] = float(m.nrm[v].z);
  }
  std::vector<unsigned> in, out(tris.size() * 3);
  in.reserve(tris.size() * 3);
  for (const auto &t : tris) in.insert(in.end(), {t[0], t[1], t[2]});
  const float weights[5] = {1.0f, 1.0f, 0.5f, 0.5f, 0.5f};
  float err = 0;
  // the borders of the mesh (a sleeve's open end, a cut at the neck) keep their place; seams between materials and
  // UV islands collapse on both sides together
  std::size_t n = meshopt_simplifyWithAttributes(out.data(), in.data(), in.size(), P.data(), nv, sizeof(float) * 3,
                                                 A.data(), sizeof(float) * 5, weights, 5, nullptr, target * 3, 1.0f,
                                                 meshopt_SimplifyLockBorder, &err);
  if (n > target * 3) {  // the locked borders hold too many: let them move too
    n = meshopt_simplifyWithAttributes(out.data(), in.data(), in.size(), P.data(), nv, sizeof(float) * 3, A.data(),
                                       sizeof(float) * 5, weights, 5, nullptr, target * 3, 1.0f, 0, &err);
  }
  if (n > target * 3) {  // still too many (many small separate pieces): the plain simplifier, then dropping
    n = meshopt_simplify(out.data(), in.data(), in.size(), P.data(), nv, sizeof(float) * 3, target * 3, 1.0f, 0, &err);
    if (n > target * 3) n = target * 3;
  }
  // the vertices still used, renumbered
  std::vector<unsigned> remap(nv, ~0u);
  DffMesh r = m;
  r.pos.clear(), r.nrm.clear(), r.uv.clear(), r.bone.clear(), r.weight.clear(), r.tris.clear();
  for (std::size_t i = 0; i < n; i += 3) {
    std::array<unsigned, 4> t{out[i], out[i + 1], out[i + 2], vmat[out[i]]};
    for (int c = 0; c < 3; ++c) {
      unsigned &w = remap[t[c]];
      if (w == ~0u) {
        w = unsigned(r.pos.size());
        r.pos.push_back(m.pos[t[c]]), r.nrm.push_back(m.nrm[t[c]]), r.uv.push_back(m.uv[t[c]]);
        if (t[c] < m.bone.size()) r.bone.push_back(m.bone[t[c]]);
        if (t[c] < m.weight.size()) r.weight.push_back(m.weight[t[c]]);
      }
      t[c] = w;
    }
    r.tris.push_back(t);
  }
  log += fmt("the skin reduced from %zu to %zu triangles, %zu vertices (deviation %.2f%% of its size; one model holds "
             "21845 triangles, half of them with the inner layer)\n", before, r.tris.size(), r.pos.size(),
             double(err) * 100.0);
  m = std::move(r);
}

}  // namespace

bool Packs::add(const std::string &path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const auto size = std::uint64_t(in.tellg());
  if (size < 8) return false;
  in.seekg(0);
  std::uint8_t h[8];
  in.read(reinterpret_cast<char *>(h), 8);
  const std::uint32_t id = std::uint32_t(h[0] | h[1] << 8 | h[2] << 16 | std::uint32_t(h[3]) << 24);
  const std::uint32_t n = std::uint32_t(h[4] | h[5] << 8 | h[6] << 16 | std::uint32_t(h[7]) << 24);
  if (n > 65536 || 8 + std::uint64_t(n) * 4 > size) return false;
  std::vector<std::uint8_t> raw(std::size_t(n) * 4);
  in.read(reinterpret_cast<char *>(raw.data()), std::streamsize(raw.size()));
  Pack p{id, path, {}, size};
  for (std::uint32_t i = 0; i < n; ++i)
    p.offsets.push_back(std::uint32_t(raw[i * 4] | raw[i * 4 + 1] << 8 | raw[i * 4 + 2] << 16 |
                                      std::uint32_t(raw[i * 4 + 3]) << 24));
  // a later pack of the same id replaces the earlier (Python's dict)
  for (auto &q : packs_)
    if (q.id == id) {
      q = std::move(p);
      return true;
    }
  packs_.push_back(std::move(p));
  return true;
}

const Packs::Pack *Packs::find(std::uint32_t ref) const {
  for (const auto &p : packs_)
    if (p.id == ref) return &p;
  const Pack *only = nullptr;
  int same = 0;
  for (const auto &p : packs_)
    if (p.id >> 24 == ref >> 24) only = &p, ++same;
  return same == 1 ? only : nullptr;
}

bool Packs::image(std::uint32_t ref, std::uint32_t index, Image &out, std::string *why) const {
  const Pack *p = find(ref);
  if (!p || index >= p->offsets.size()) return false;
  const std::uint64_t start = std::uint64_t(p->offsets[index]) + 16;
  std::uint64_t stop = p->fileSize;
  for (std::uint32_t o : p->offsets)
    if (o > p->offsets[index] && o < stop) stop = o;
  if (start >= stop) return false;
  try {
    const Bytes dds = readFile(p->path, start, stop - start);
    Rgba img;
    const bool dds_ = dds.size() >= 4 && std::memcmp(dds.data(), "DDS ", 4) == 0;
    if (!(dds_ ? decodeDds(dds, img, why) : decodeTga(dds, img, why))) return false;
    out.width = img.w, out.height = img.h, out.rgba = std::move(img.px);
    return true;
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    return false;
  }
}

bool ConvertSkin(const Bytes &gcData, const std::string &gcName, const Bytes &pcData, const Packs &packs,
                 const Options &o, Bytes &result, std::string &log, std::string *why) {
  try {
    const Archive gc(gcData, true);
    const Bytes pcBytes = IsLfs(pcData) ? UnpackLfs(pcData) : pcData;
    const Archive pc(pcBytes, false);
    std::string stem = gcName;
    if (auto slash = stem.find_last_of("/\\"); slash != std::string::npos) stem = stem.substr(slash + 1);
    stem = stem.substr(0, stem.find('.'));
    for (auto &c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
    const auto ch = CharacterTplUsers().find(stem);
    const bool player = ch == CharacterTplUsers().end();
    const TplUsers &users = player ? PlayerTplUsers() : ch->second;
    const PcView view = align(gc, pc);
    if (pc.count() != gc.count()) {
      std::string dropped;
      for (auto j : view.dropped) dropped += (dropped.empty() ? "" : ", ") + std::to_string(j);
      log += fmt("the archives differ: %zu entries on the disc, %zu in the PC file; matched by kind, PC entries left "
                 "out: [%s]\n", gc.count(), pc.count(), dropped.c_str());
    }
    std::optional<Bytes> rebind0;
    if (gc.kind(0) == "BIN" && view.kind(0) == "BIN") {
      const std::string differs = checkSkeleton(gc.entry(0), view.entry(0), o, log);
      if (!differs.empty() && o.rebind) {
        std::vector<int> moved;
        rebindMap(gc.entry(0), view.entry(0), &moved);
        rebind0 = gc.entry(0);
        log += fmt("the skeletons differ (%s): drawn with the disc's %u parts; %zu PC parts move with their nearest "
                   "common part\n", differs.c_str(), unsigned(gc.entry(0)[0x19]), moved.size());
      } else if (!differs.empty()) {
        fail("the skeletons differ: " + differs + "; the disc's motions would not fit this model");
      }
    }
    std::vector<Bytes> entries;
    for (std::size_t i = 0; i < gc.count(); ++i) {
      const std::string kindGc = gc.kind(i), kindPc = view.kind(i);
      const Bytes blobPc = view.entry(i);
      if (view.map[i] < 0) {
        entries.push_back(gc.entry(i));
        continue;
      }
      if (kindGc != kindPc) fail(fmt("entry %zu: %s on the disc, %s in the PC file", i, kindGc.c_str(), kindPc.c_str()));
      if (blobPc.empty()) {
        entries.push_back(gc.entry(i));
        continue;
      }
      if (kindPc == "BIN") {
        const Converted c = convertModel(blobPc, std::size_t(o.palette), i == 0 && rebind0 ? &*rebind0 : nullptr);
        log += fmt("entry %3zu BIN: %s\n", i, c.stats.c_str());
        entries.push_back(c.blob);
      } else if (kindPc == "TPL" && tplFound(blobPc, packs) == 0 && !gc.entry(i).empty()) {
        entries.push_back(gc.entry(i));
        log += fmt("entry %3zu TPL: none of its textures in the mod's packs, the disc's\n", i);
      } else if (kindPc == "TPL") {
        std::vector<std::string> stats, notes;
        const Bytes t = convertTpl(blobPc, packs, o, stats, notes, gc.entry(i),
                                   colourSlots(view, gc.count(), int(i), users, player));
        log += fmt("entry %3zu TPL: %zu textures, %zu bytes\n", i, stats.size(), t.size());
        for (const auto &st : stats) log += st + "\n";
        for (const auto &st : notes) log += st + "\n";
        entries.push_back(t);
      } else {
        entries.push_back(gc.entry(i));
      }
    }
    const Bytes body = buildBody(entries, gc.types);
    result = buildArchive(gc, body);
    log += fmt("%s: %zu bytes, data part %zu bytes (disc %zu; the game's place for Leon holds %d)\n", gcName.c_str(),
               result.size(), body.size(), gc.body.size(), 0x118000);
    return true;
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    return false;
  }
}

namespace {

// A skinned mesh (a GTA skin's, an RE4 Remake mod's: its bones named as a GTA ped's, kLeon) with its textures, onto
// Leon's archive `gcData`: the work of ConvertDff below the reading.
// A new model in the PC release's layout on a disc model's skeleton and settings (`body`: its header, bone records
// and first part record), then the disc's (convertModel): the triangles by material (`byMat`, corners into P, N, uv,
// W), each material's texture index (`matSlot`), the palette's size (`texCount`, header 0x24). GTA skins, RE4 Remake
// outfits and weapons.
Converted buildModel(const Bytes &body, const std::map<unsigned, std::vector<std::array<unsigned, 3>>> &byMat,
                     const std::vector<V3> &P, const std::vector<V3> &N, const std::vector<std::array<double, 2>> &uv,
                     const std::vector<Weight> &W, const std::map<unsigned, unsigned> &matSlot, std::size_t texCount,
                     std::size_t palette) {
    const Bytes &posed = body;
    std::size_t corners = 0;
    for (const auto &[mat, list] : byMat) corners += list.size() * 3;
    std::map<Weight, unsigned> wIndex;
    std::vector<Weight> wList;
    Bytes vtx, nrm, tex, clr, vidx;
    for (const auto &[mat, list] : byMat)
      for (const auto &t : list)
        for (unsigned v : t) {
          lefw(vtx, float(P[v].x)), lefw(vtx, float(P[v].y)), lefw(vtx, float(P[v].z));
          lefw(nrm, float(N[v].x)), lefw(nrm, float(N[v].y)), lefw(nrm, float(N[v].z));
          lefw(tex, float(uv[v][0])), lefw(tex, float(uv[v][1]));
          le32w(clr, 0xFFFFFFFFu);
          auto wi = wIndex.find(W[v]);
          if (wi == wIndex.end()) wi = wIndex.emplace(W[v], unsigned(wList.size())).first, wList.push_back(W[v]);
          le16w(vidx, wi->second);
        }
    Bytes wgt;
    for (const auto &e : wList) {
      for (std::size_t k = 0; k < 3; ++k) wgt.push_back(std::uint8_t(k < e.size() ? e[k].first : 0));
      wgt.push_back(std::uint8_t(e.size()));
      for (std::size_t k = 0; k < 3; ++k) wgt.push_back(std::uint8_t(k < e.size() ? e[k].second : 0));
      wgt.push_back(0);
    }
    Bytes parts;
    const std::uint32_t partsAt0 = be32(body, 0x1C);
    Bytes raw(body.begin() + long(partsAt0), body.begin() + long(partsAt0 + 0x18));
    raw[0xB] = 0, raw[0xD] = raw[0xE] = raw[0xF] = 0xFF, raw[0x17] = 0xFF;
    for (std::size_t k = 0x10; k < 0x17; ++k) raw[k] = 0;
    for (const auto &[mat, list] : byMat) {
      Bytes r = raw;
      r[0xC] = std::uint8_t(matSlot.at(mat));
      parts.insert(parts.end(), r.begin(), r.end());
      std::vector<unsigned> runs;
      for (std::size_t left = list.size() * 3; left;) {
        const std::size_t c = std::min<std::size_t>(left, 65535 - 65535 % 3);
        runs.push_back(unsigned(c));
        left -= c;
      }
      le32w(parts, std::uint32_t(4 + runs.size() * 4)), le32w(parts, std::uint32_t(list.size()));
      le32w(parts, std::uint32_t(runs.size()));
      for (unsigned c : runs) le16w(parts, 5), le16w(parts, c);
    }
    Bytes blob(0x50, 0);
    auto section = [&](const Bytes &d) {
      while (blob.size() % 32) blob.push_back(0);
      const std::uint32_t at2 = std::uint32_t(blob.size());
      blob.insert(blob.end(), d.begin(), d.end());
      return at2;
    };
    Bytes head;
    for (const auto &rec : gcBoneRecords(posed)) {
      head.insert(head.end(), rec.begin(), rec.begin() + 4);
      for (int k = 0; k < 3; ++k) lefw(head, bef32(rec, std::size_t(4 + k * 4)));
    }
    const std::uint32_t oHead = section(head), oVtx = section(vtx), oNrm = section(nrm), oTex = section(tex),
                        oClr = section(clr), oWgt = section(wgt), oVidx = section(vidx), oParts = section(parts);
    le32at(blob, 0x00, oHead), le32at(blob, 0x04, be32(body, 4)), le32at(blob, 0x08, be32(body, 8));
    le32at(blob, 0x0C, oClr), le32at(blob, 0x10, oTex), le32at(blob, 0x14, oWgt);
    blob[0x18] = std::uint8_t(wList.size() <= 0xFF ? wList.size() : 0), blob[0x19] = body[0x19];
    le16at(blob, 0x1A, unsigned(byMat.size())), le32at(blob, 0x1C, oParts);
    le32at(blob, 0x20, be32(body, 0x20) | 0x80000000u), le32at(blob, 0x24, std::uint32_t(texCount));
    blob[0x28] = 0, blob[0x29] = body[0x29], le16at(blob, 0x2A, unsigned(wList.size() > 0xFF ? wList.size() : 0));
    le32at(blob, 0x2C, 0), le32at(blob, 0x30, oVtx), le32at(blob, 0x34, oNrm);
    le16at(blob, 0x38, unsigned(corners)), le16at(blob, 0x3A, unsigned(corners)), le32at(blob, 0x3C, 0x20030818u);
    le32at(blob, 0x40, 0), le32at(blob, 0x44, 0), le32at(blob, 0x48, oVidx), le32at(blob, 0x4C, 0);
    return convertModel(blob, palette, &posed);
}

// The character's own skin in its archive (entry = PL_ARC - 4 of setModel): body, head, face, hands; with
// `outfit` its outfit too (Leon's jacket in pl00)
std::vector<std::size_t> ownSkin(const std::string &stem, bool outfit) {
  if (stem == "pl01" || stem == "pl05") return outfit ? std::vector<std::size_t>{0, 2, 3, 4, 13, 16} : std::vector<std::size_t>{0, 2, 3, 13, 16};
  if (stem == "pl11") return outfit ? std::vector<std::size_t>{0, 2, 3, 5, 8, 11} : std::vector<std::size_t>{0, 2, 3, 8, 11};
  if (stem == "pl0b" || stem == "pl0c") return outfit ? std::vector<std::size_t>{0, 2, 4, 5, 13, 14} : std::vector<std::size_t>{0, 2, 4, 13, 14};
  return outfit ? std::vector<std::size_t>{0, 2, 4, 6, 14, 17} : std::vector<std::size_t>{0, 2, 4, 14, 17};
}

void skinOnLeon(const Bytes &gcData, const std::string &gcName, DffMesh m, const std::map<std::string, Rgba> &images,
                std::vector<std::string> notes, const Options &o, Bytes &result, std::string &log, const char *what,
                bool shoulderFit = false) {
    const Archive gc(gcData, true);
    std::string stem = gcName;
    if (auto slash = stem.find_last_of("/\\"); slash != std::string::npos) stem = stem.substr(slash + 1);
    stem = lower(stem.substr(0, stem.find('.')));
    static const std::set<std::string> kOn = {"pl00", "pl08", "pl09", "pl10", "pl01", "pl05", "pl11", "pl0b", "pl0c"};
    if (!kOn.count(stem))
      fail(std::string(what) + "s go on Leon (pl00, pl08..pl10), Ashley (pl01, pl05, pl11) or Ada (pl0b, pl0c), not on " +
           stem);
    if (gc.count() < 22 || gc.kind(0) != "BIN" || gc.kind(1) != "TPL") fail("not Leon's archive");
    const Bytes body = gc.entry(0);
    const auto skel = discSkeleton(body);
    for (int need : {0, 2, 3, 4, 5, 7, 8, 10, 11, 13, 14, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 27, 28, 40, 55})
      if (!skel.count(need)) fail(fmt("the disc's skeleton has no bone %d", need));

    log += fmt("%s: %zu vertices, %zu triangles, %zu materials, %zu bones, %zu textures\n", what, m.pos.size(),
               m.tris.size(), m.matTex.size(), m.boneAt.size(), images.size());
    // the inner layer doubles the triangles: half the model's room for the skin then. Leon and Ashley lend the skin a
    // second model, their outfit's (entry 6, drawn with the body's TPL; DevJunior, 25.11.2026: "the Studio does not
    // convert the skins well": a 62798-triangle skin cut to 10900 lost 8.4 % of its shape, spiky hands, holes in the
    // hair; in two models 21800, 3.0 %)
    const std::size_t second = (stem == "pl0b" || stem == "pl0c") ? 0 : 6;
    const std::size_t room = (o.innerLayer ? 10900 : 21800) * (second ? 2 : 1);
    reduceSkin(m, o.maxSkinTris ? std::min(o.maxSkinTris, room) : room, log);
    const std::size_t nb = m.boneAt.size(), nv = m.pos.size();

    // Some rigs name the back "Spine 1" / "Spine 2" for GTA's "Spine" / "Spine1" (DevJunior, 25.11.2026: Sergio Van
    // Dyk's Ashley: the back then moved with the hips, stiff)
    {
      bool spine = false, spine1 = false;
      for (const auto &n : m.boneName) spine |= gtaKey(n) == "spine", spine1 |= gtaKey(n) == "spine 1";
      if (!spine && spine1)
        for (auto &n : m.boneName)
          if (gtaKey(n) == "spine 1") n = "spine";
          else if (gtaKey(n) == "spine 2") n = "spine1";
    }
    // the bones by name, each to its disc bone
    std::map<std::string, int> byName;
    for (std::size_t k = 0; k < nb; ++k)
      if (!m.boneName[k].empty()) byName[gtaKey(m.boneName[k])] = int(k);
    for (const char *need : {"pelvis", "head", "l foot", "r foot", "l upperarm", "r upperarm"})
      if (!byName.count(need)) fail(std::string("the skin has no bone \"") + need + "\": not a GTA SA ped skeleton");
    auto at = [&](const char *n) { return m.boneAt[std::size_t(byName.at(n))]; };

    // The whole skin into Leon's space (up, forward, its left) at the size of his trunk: hips to head as long as his.
    // Leon's skeleton stays as it is (his motions, his grips on the weapons and the knife are made for it): the trunk
    // (hips, back, neck, head) keeps the skin's own shape and sits on his hips; the limbs are turned at his joints to
    // point as his bones do and stretched to his bones' lengths, so the hands are where his are.
    const V3 feet = (at("l foot") + at("r foot")) * 0.5;
    const bool toes = byName.count("l toe0") && byName.count("r toe0");
    const V3 toeMid = toes ? (at("l toe0") + at("r toe0")) * 0.5 : feet;
    const V3 up = unit(at("head") - feet);
    V3 fwd = toes ? toeMid - feet : V3{};
    if (len(fwd - up * dot(fwd, up)) < 1e-6) fwd = cross(up, at("l upperarm") - at("r upperarm"));
    fwd = unit(fwd - up * dot(fwd, up));
    V3 left = cross(up, fwd);
    bool mirror = false;
    if (dot(at("l upperarm") - at("r upperarm"), left) < 0) left = left * -1.0, mirror = true;
    // GTA skins: hips to head as long as his, on his hips. RE4 Remake mods (Leon himself, his bones not placed as on
    // the disc: the Remake's hip joint is at the thighs, the disc's pelvis bone at the waist): the thighs to the
    // shoulders as his, the thighs' middle on his (their head and feet then within a few cm of his: limbs ~1.0..1.1).
    double gtaTrunk, leonTrunk;
    V3 hips, h;
    if (shoulderFit) {
      const V3 th = (at("l thigh") + at("r thigh")) * 0.5, sh = (at("l upperarm") + at("r upperarm")) * 0.5;
      gtaTrunk = dot(sh - th, up), leonTrunk = (skel.at(13).at.y + skel.at(7).at.y - skel.at(22).at.y - skel.at(18).at.y) * 0.5;
      hips = th, h = (skel.at(22).at + skel.at(18).at) * 0.5;
      if (o.trunkToHead) {  // the thighs to the head joint as his: a smaller trunk and head, as the GTA skins come out
        gtaTrunk = dot(at("head") - th, up), leonTrunk = skel.at(4).at.y - h.y;
      }
    } else {
      gtaTrunk = dot(at("head") - at("pelvis"), up), leonTrunk = skel.at(4).at.y - skel.at(17).at.y;
      hips = at("pelvis"), h = skel.at(17).at;
    }
    if (gtaTrunk <= 0) fail("the skin's head (or shoulders) is not above its hips");
    double scale = leonTrunk / gtaTrunk;
    // A GTA skin whose limbs come out much shorter than the character's at the trunk's size (DevJunior, 25.11.2026:
    // Sergio Van Dyk's Ashley on Ashley: every limb stretched by 1.22..1.53, thin and spiky): the whole skin a little
    // larger, halfway (the square root of the limbs' middle stretch), so trunk and limbs share the difference. Only
    // on Ashley and Ada: on Leon the GTA skins stay as DevJunior took them (20.11.2026: "theirs looked best").
    const bool onLeon = stem == "pl00" || stem == "pl08" || stem == "pl09" || stem == "pl10";
    if (!shoulderFit && !onLeon) {
      std::vector<double> ks;
      const std::pair<const char *, const char *> limbs[] = {{"l upperarm", "l forearm"}, {"l forearm", "l hand"},
          {"r upperarm", "r forearm"}, {"r forearm", "r hand"}, {"l thigh", "l calf"}, {"l calf", "l foot"},
          {"r thigh", "r calf"}, {"r calf", "r foot"}};
      for (const auto &lb : limbs) {
        int da = -1, db = -1;
        for (const auto &e : kLeon)
          if (std::string(e.gta) == lb.first) da = e.disc, db = e.discTo;
        if (da < 0 || db < 0 || !byName.count(lb.first) || !byName.count(lb.second)) continue;
        const double sk = len(at(lb.second) - at(lb.first)) * scale, dk = len(skel.at(db).at - skel.at(da).at);
        if (sk > 1e-6) ks.push_back(dk / sk);
      }
      if (ks.size() >= 4) {
        std::sort(ks.begin(), ks.end());
        const double mid = ks[ks.size() / 2];
        if (mid > 1.12 || mid < 0.88) {
          scale *= std::sqrt(mid);
          log += fmt("its limbs %.2f times the character's at the trunk's size: the whole skin %.2f times larger\n", mid,
                     std::sqrt(mid));
        }
      }
    }
    auto G = [&](V3 v) {  // the skin's hips on Leon's
      const V3 d = v - hips;
      return V3{h.x + scale * dot(d, left), h.y + scale * dot(d, up), h.z + scale * dot(d, fwd)};
    };
    auto Gdir = [&](V3 n) { return unit(V3{dot(n, left), dot(n, up), dot(n, fwd)}); };
    log += fmt("into Leon's space: scale %.1f (his trunk)%s\n", scale, mirror ? ", mirrored (its left side was the other)" : "");

    struct Piece {
      bool set = false;
      int disc = 0;
      M3 r;
      V3 from, to, axis{0, 1, 0};
      double k = 1, c = 1;  // along the bone, across it (half the stretch: a longer limb also a little thicker)
      V3 apply(V3 p) const {
        const V3 q = p - from;
        const double a = dot(q, axis);
        return to + r * (axis * (a * k) + (q - axis * a) * c);
      }
    };
    std::vector<Piece> piece(nb);
    std::vector<V3> gj(nb);
    for (std::size_t b = 0; b < nb; ++b) gj[b] = G(m.boneAt[b]);
    auto isLimb = [](const std::string &k) {
      for (const char *x : {"upperarm", "forearm", "hand", "thigh", "calf", "foot"})
        if (k.find(x) != std::string::npos && k.find("finger") == std::string::npos) return true;
      return false;
    };
    std::vector<std::string> unknown;
    double kMin = 9, kMax = 0;
    std::function<const Piece &(std::size_t, int)> make = [&](std::size_t b, int depth) -> const Piece & {
      Piece &pc = piece[b];
      if (pc.set || depth > 64) return pc;
      const std::string key = gtaKey(m.boneName[b]);
      const BoneMap *bm = nullptr;
      for (const auto &e : kLeon)
        if (key == e.gta) bm = &e;
      if (!bm) unknown.push_back(m.boneName[b]);
      const int parent = m.boneParent[b];
      const bool clavicle = key.find("clavicle") != std::string::npos;
      if (bm && bm->discTo >= 0 && byName.count(bm->gtaTo) && (isLimb(key) || clavicle)) {
        const V3 g = gj[std::size_t(byName.at(bm->gtaTo))] - gj[b];
        // a limb from Leon's joint along his bone; a clavicle from where the trunk puts it to his shoulder joint
        V3 start = skel.at(bm->disc).at;
        if (clavicle && parent >= 0) start = make(std::size_t(parent), depth + 1).apply(gj[b]);
        const V3 l = skel.at(bm->discTo).at - start;
        pc.axis = unit(g);
        pc.r = turn(pc.axis, unit(l));
        pc.k = std::max(0.6, std::min(1.6, len(l) / std::max(1e-6, len(g))));
        pc.c = 1 + (pc.k - 1) * 0.5;
        kMin = std::min(kMin, pc.k), kMax = std::max(kMax, pc.k);
        pc.from = gj[b], pc.to = start;
        pc.disc = bm->disc;
      } else if (parent >= 0) {
        // the trunk follows its root; leaves (head, fingers, toes) and unknown bones move as their parent does
        pc = make(std::size_t(parent), depth + 1);
        pc.set = false;
        if (bm) pc.disc = bm->disc;
      } else {
        // the root of the skeleton: the trunk as it is, on Leon's hips
        pc.from = pc.to = V3{};
        pc.disc = bm ? bm->disc : 0;
      }
      pc.set = true;
      return pc;
    };
    for (std::size_t b = 0; b < nb; ++b) make(b, 0);
    if (!unknown.empty()) {
      std::string list;
      for (const auto &u : unknown) list += (list.empty() ? "" : ", ") + u;
      log += "bones not known here, moved with their parent: " + list + "\n";
    }
    log += fmt("limbs stretched to Leon's bones by %.2f..%.2f\n", kMin, kMax);
    const Bytes &posed = body;

    // the vertices on Leon's skeleton, their weights by his bones
    std::vector<V3> P(nv), N(nv);
    std::vector<Weight> W(nv);
    for (std::size_t v = 0; v < nv; ++v) {
      const V3 g = G(m.pos[v]), gn = Gdir(m.nrm[v]);
      V3 p{}, n{};
      double total = 0;
      std::map<int, double> by;
      for (int j = 0; j < 4; ++j) {
        const double w = m.weight[v][std::size_t(j)];
        const unsigned b = m.bone[v][std::size_t(j)];
        if (w <= 0 || b >= nb) continue;
        p = p + piece[b].apply(g) * w;
        n = n + piece[b].r * gn * w;
        by[piece[b].disc] += w;
        total += w;
      }
      if (total <= 0) {
        P[v] = g, N[v] = gn, W[v] = {{17, 100}};
        continue;
      }
      P[v] = p * (1.0 / total), N[v] = unit(n);
      std::vector<std::pair<int, double>> list(by.begin(), by.end());
      std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
      if (list.size() > 3) list.resize(3);
      double sum = 0;
      for (const auto &x : list) sum += x.second;
      Weight e;
      int given = 0;
      for (std::size_t k = 0; k < list.size(); ++k) {
        const int r = k + 1 == list.size() ? 100 - given : int(std::lround(list[k].second * 100.0 / sum));
        if (r <= 0) continue;
        e.push_back({list[k].first, r});
        given += r;
      }
      W[v] = e;
    }
    // the skin's own head: vertices its own skeleton puts on the head (before Leon's or the character's weights)
    std::size_t ownHead = 0;
    for (std::size_t v = 0; v < nv; ++v) {
      int best = -1, bone = -1;
      for (const auto &[b, r] : W[v])
        if (r > best) best = r, bone = b;
      if (bone == 4 || bone == 27 || bone == 28) ++ownHead;
    }

    // Leon's own weights for the skin (DevJunior, 19.11.2026: "the models stretch too much ... the tool has to fix
    // this"): the skin's few bones blend coarsely (a GTA ped has no twist bones, its hips and shoulders bend hard);
    // each vertex takes the weights of the nearest vertices of Leon's own body, head, face and hands (by the inverse
    // square of their distance, only those of the body parts its own bones belong to: an arm keeps arm weights), so
    // it bends as his skin does, with his twist bones. Far from him (a shoulder pad, the helmet) it keeps more of its
    // own.
    if (o.transfer > 0) {
      std::vector<SrcVert> src;
      for (std::size_t e : ownSkin(stem, true))
        if (e < gc.count() && gc.kind(e) == "BIN" && !gc.entry(e).empty()) collectSkin(gc.entry(e), skel, src);
      std::size_t moved = 0;
      double far = 0;
      for (std::size_t v = 0; v < nv; ++v) {
        // the head keeps its own weights (DevJunior, 19.11.2026: the helmet "deforms" in the cutscenes: near his face
        // it took Leon's jaw, which talks); a vertex mostly on the skin's head, jaw or brows
        {
          double head = 0, all = 0;
          for (int j = 0; j < 4; ++j) {
            const unsigned b = m.bone[v][std::size_t(j)];
            const double w = m.weight[v][std::size_t(j)];
            if (w <= 0 || b >= nb) continue;
            const std::string key = gtaKey(m.boneName[b]);
            all += w;
            if (key == "head" || key == "jaw" || key == "l brow" || key == "r brow") head += w;
          }
          if (all > 0 && head / all > 0.5) continue;
        }
        std::set<int> chains;
        for (const auto &[bone, r] : W[v]) chains.insert(chainOf(bone));
        // the 6 nearest of his vertices on those chains
        std::array<std::pair<double, const SrcVert *>, 6> near;
        near.fill({1e30, nullptr});
        for (const auto &sv : src) {
          if (!chains.count(sv.chain)) continue;
          const V3 d = sv.p - P[v];
          const double dd = dot(d, d);
          if (dd >= near.back().first) continue;
          std::size_t at2 = near.size() - 1;
          while (at2 > 0 && near[at2 - 1].first > dd) near[at2] = near[at2 - 1], --at2;
          near[at2] = {dd, &sv};
        }
        if (!near[0].second) continue;
        std::map<int, double> acc;
        double total = 0;
        for (const auto &[dd, sv] : near) {
          if (!sv) break;
          const double w = 1.0 / (dd + 25.0);
          for (const auto &[bone, r] : sv->w) acc[bone] += w * r;
          total += w * 100.0;
        }
        // near him all his; 250 mm away and more half his, half its own
        const double dist = std::sqrt(near[0].first);
        far = std::max(far, dist);
        const double mine = o.transfer * (dist < 80 ? 1.0 : std::max(0.5, 1.0 - (dist - 80) / 340.0));
        std::map<int, double> mix;
        for (const auto &[bone, a] : acc) mix[bone] += mine * a / total;
        for (const auto &[bone, r] : W[v]) mix[bone] += (1 - mine) * r / 100.0;
        std::vector<std::pair<int, double>> list(mix.begin(), mix.end());
        std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        if (list.size() > 3) list.resize(3);
        double sum = 0;
        for (const auto &x : list) sum += x.second;
        Weight e;
        int given = 0;
        for (std::size_t k = 0; k < list.size(); ++k) {
          const int r = k + 1 == list.size() ? 100 - given : int(std::lround(list[k].second * 100.0 / sum));
          if (r <= 0) continue;
          e.push_back({list[k].first, r});
          given += r;
        }
        if (!e.empty()) W[v] = e, ++moved;
      }
      log += fmt("weights from Leon's own skin for %zu of %zu vertices (%zu of his; the farthest %.0f mm off)\n", moved,
                 nv, src.size(), far);
    }

    const std::vector<Weight> Wsoft = W;  // before the plates: the inner layer bends smoothly
    std::vector<char> plated(nv, 0);
    // The armour round the hips (DevJunior, 19.11.2026): a separate small piece of the mesh there (its triangles joined
    // by shared vertices, at most kPlate across) is armour, not cloth. First all its vertices took the piece's average
    // weights, so it moved as one ("the crotch plate looks very bad when running": it stretched between the thighs);
    // made rigid, the hip guards stood off the leg when it lifted, a dark gap under them (second report: "it still
    // breaks"). Now half way: each vertex kStiff of the piece's average and the rest its own smooth weights (half the
    // stretch, no gap; checked in the Studio's view with the knee raised, walking and crouching). A piece hanging
    // between both thighs (each leg at least kBothLegs of its weights) stays one rigid plate, mostly on the pelvis
    // (kPelvis).
    {
      const double kPlate = 540.0;  // the hip guards and thigh plates of a suit of armour are this large
      std::vector<std::size_t> root(nv);
      for (std::size_t v = 0; v < nv; ++v) root[v] = v;
      std::function<std::size_t(std::size_t)> find = [&](std::size_t x) {
        while (root[x] != x) x = root[x] = root[root[x]];
        return x;
      };
      for (const auto &t : m.tris) {
        if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
        root[find(t[1])] = find(t[0]), root[find(t[2])] = find(t[0]);
      }
      std::map<std::size_t, std::vector<std::size_t>> parts2;
      for (std::size_t v = 0; v < nv; ++v) parts2[find(v)].push_back(v);
      std::size_t plates = 0, stiffened = 0, plateVerts = 0;
      for (const auto &[r, vs] : parts2) {
        if (vs.size() < 3) continue;
        V3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
        for (auto v : vs)
          lo = {std::min(lo.x, P[v].x), std::min(lo.y, P[v].y), std::min(lo.z, P[v].z)},
          hi = {std::max(hi.x, P[v].x), std::max(hi.y, P[v].y), std::max(hi.z, P[v].z)};
        if (len(hi - lo) > kPlate) continue;
        // only round the hips (the crotch plate, the hip guards): plates on the shoulders and limbs opened gaps at
        // the joints (DevJunior, 19.11.2026: "the model now shows breaks")
        const V3 mid = (lo + hi) * 0.5, h = skel.at(17).at;
        if (mid.y < h.y - 320 || mid.y > h.y + 120 || std::fabs(mid.x - h.x) > 230) continue;
        double legL = 0, legR = 0, all = 0;
        for (auto v : vs)
          for (const auto &[bone, rate] : W[v]) {
            all += rate;
            if (chainOf(bone) == 3) legL += rate;
            if (chainOf(bone) == 4) legR += rate;
          }
        if (all <= 0) continue;
        const double kBothLegs = 0.15, kPelvis = 0.65, kStiff = 0.5;
        const bool crotch = legL / all >= kBothLegs && legR / all >= kBothLegs;
        const double stiff = crotch ? 1.0 : kStiff;
        std::map<int, double> avg;
        for (auto v : vs)
          for (const auto &[bone, rate] : W[v]) avg[bone] += (crotch ? 1.0 - kPelvis : 1.0) * rate / all;
        if (crotch) avg[17] += kPelvis;
        for (auto v : vs) {
          std::map<int, double> acc;
          for (const auto &[bone, a] : avg) acc[bone] += stiff * a;
          for (const auto &[bone, rate] : W[v]) acc[bone] += (1.0 - stiff) * rate / 100.0;
          std::vector<std::pair<int, double>> list(acc.begin(), acc.end());
          std::stable_sort(list.begin(), list.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
          if (list.size() > 3) list.resize(3);
          double sum = 0;
          for (const auto &x : list) sum += x.second;
          Weight e;
          int given = 0;
          for (std::size_t k = 0; k < list.size(); ++k) {
            const int rr = k + 1 == list.size() ? 100 - given : int(std::lround(list[k].second * 100.0 / sum));
            if (rr <= 0) continue;
            e.push_back({list[k].first, rr});
            given += rr;
          }
          W[v] = e;
          plated[v] = crotch;
        }
        ++(crotch ? plates : stiffened), plateVerts += vs.size();
      }
      log += fmt("%zu pieces of the mesh; at the hips %zu crotch plates move rigidly, %zu pieces half stiff (%zu vertices)\n",
                 parts2.size(), plates, stiffened, plateVerts);
    }

    // Leon's build (DevJunior, 20.11.2026, the Remake's Master Chief: "make it Leon's size and proportions, it looks too
    // fat and big"): around each of Leon's bones the skin is made no thicker than his own body there (his body, head and
    // hands of the disc, with his jacket in pl00): for each bone the distance of its outer vertices from the bone (the
    // segment to its child; for an end bone the middle of his vertices on it) is measured on both; the skin's distances are
    // scaled down by one factor for all (0.7 at most), each vertex by its weights, so the joints stay smooth. Only
    // thinner, never thicker.
    if (o.fitBody > 0) {
      std::map<int, int> childOf;
      for (const auto &[id, b] : skel) {
        if (b.parent < 0 || !transferBone(id) || !transferBone(b.parent)) continue;
        const V3 d = b.at - skel.at(b.parent).at;
        auto it = childOf.find(b.parent);
        // several children (the spine: neck and clavicles; the hips: spine and thighs): the most upright one
        if (it == childOf.end() || std::fabs(unit(d).y) > std::fabs(unit(skel.at(it->second).at - skel.at(b.parent).at).y))
          childOf[b.parent] = id;
      }
      // an end bone (the head, a hand): round the middle of the skin's own vertices on it (round Leon's pulled the
      // helmet forward and down onto the chest: "it looks like a dinosaur", DevJunior, 20.11.2026)
      std::map<int, V3> hisMiddle, refMiddle;  // the skin's, the reference skin's
      auto radialAt = [&](int bone, const V3 &p, V3 &foot, const std::map<int, V3> &middle) {
        const V3 j = skel.at(bone).at;
        auto c = childOf.find(bone);
        if (c == childOf.end()) {
          auto m = middle.find(bone);
          foot = m != middle.end() ? m->second : j;
          return p - foot;
        }
        const V3 seg = skel.at(c->second).at - j;
        const double t = std::max(0.0, std::min(1.0, dot(p - j, seg) / std::max(1e-9, dot(seg, seg))));
        foot = j + seg * t;
        return p - foot;
      };
      auto radial = [&](int bone, const V3 &p, V3 &foot) { return radialAt(bone, p, foot, hisMiddle); };
      // measured only along the middle of a bone (a knee or a hip of the other bone's vertices would count as girth)
      auto inMiddle = [&](int bone, const V3 &p) {
        auto c = childOf.find(bone);
        if (c == childOf.end()) return true;
        const V3 j = skel.at(bone).at, seg = skel.at(c->second).at - j;
        const double t = dot(p - j, seg) / std::max(1e-9, dot(seg, seg));
        return t >= 0.15 && t <= 0.85;
      };
      auto heaviest = [](const std::vector<std::pair<int, double>> &w) {
        int b = -1;
        double best = -1;
        for (const auto &[bone, r] : w)
          if (r > best) best = r, b = bone;
        return b;
      };
      // his body without the jacket (pl00's entry 6) and without the outfits' extras: an armour is measured against
      // his build, not his coat
      std::vector<SrcVert> leon;
      const bool byRef = !o.fitRef.empty();
      if (byRef) {
        // another skin's proportions (DevJunior, 20.11.2026: "the Remake's with the same proportions as the GTA one"):
        // its converted archive's body (entry 0, on Leon's skeleton as this one), each bone measured on it
        const Archive ref(o.fitRef, true);
        if (ref.count() == 0 || ref.kind(0) != "BIN") fail("the reference skin is not a converted player archive");
        collectSkin(ref.entry(0), skel, leon);
        std::map<int, std::pair<V3, int>> acc;
        for (const auto &sv : leon) {
          const int b = heaviest(sv.w);
          if (b >= 0) acc[b].first = acc[b].first + sv.p, ++acc[b].second;
        }
        for (const auto &[b, a] : acc)
          if (!childOf.count(b) && a.second > 0) refMiddle[b] = a.first * (1.0 / a.second);
      } else {
        for (std::size_t e : ownSkin(stem, false))
          if (e < gc.count() && gc.kind(e) == "BIN" && !gc.entry(e).empty()) collectSkin(gc.entry(e), skel, leon);
      }
      std::map<int, std::vector<double>> his, its;  // bone -> the distances of its vertices
      {
        std::map<int, std::pair<V3, int>> acc;
        for (std::size_t v = 0; v < nv; ++v) {
          int b = -1, best = -1;
          for (const auto &[bone, r] : W[v])
            if (r > best) best = r, b = bone;
          if (b >= 0) acc[b].first = acc[b].first + P[v], ++acc[b].second;
        }
        for (const auto &[b, a] : acc)
          if (!childOf.count(b) && a.second > 0) hisMiddle[b] = a.first * (1.0 / a.second);
      }
      for (const auto &sv : leon) {
        const int b = heaviest(sv.w);
        if (b < 0 || !skel.count(b)) continue;
        V3 f;
        if (inMiddle(b, sv.p)) his[b].push_back(len(radialAt(b, sv.p, f, refMiddle)));
      }
      for (std::size_t v = 0; v < nv; ++v) {
        int b = -1, best = -1;
        for (const auto &[bone, r] : W[v])
          if (r > best) best = r, b = bone;
        if (b < 0 || !skel.count(b)) continue;
        V3 f;
        if (inMiddle(b, P[v])) its[b].push_back(len(radial(b, P[v], f)));
      }
      // the outer surface: the distance 85 % of the vertices stay within (armour plates and pouches count, the inside
      // of a sleeve less)
      auto outer = [](std::vector<double> d) {
        std::sort(d.begin(), d.end());
        return d[std::min(d.size() - 1, d.size() * 85 / 100)];
      };
      // one factor for the whole skin (DevJunior, 20.11.2026, with a factor per bone: "the torso and the body look
      // very out of proportion": a small head and thin limbs on the broad chest): the middle of the bones' own (each
      // by its vertices), so the skin keeps its proportions, only slimmer
      std::vector<std::pair<double, std::size_t>> wants;
      std::size_t all = 0;
      for (const auto &[b, sc] : its) {
        auto h = his.find(b);
        // measured on the bones with a segment (an end bone's middle is the skin's own, not his)
        if (h == his.end() || h->second.size() < 8 || sc.size() < 8 || !childOf.count(b)) continue;
        wants.push_back({outer(h->second) / std::max(1e-6, outer(sc)), sc.size()});
        all += sc.size();
      }
      std::sort(wants.begin(), wants.end());
      double want = 1.0;
      std::size_t seen = 0;
      for (const auto &[wq, n] : wants)
        if ((seen += n) * 2 >= all) {
          want = wq;
          break;
        }
      want = std::max(0.7, std::min(1.0, want));
      const double kAll = 1.0 - o.fitBody * (1.0 - want);
      std::map<int, double> k;
      std::string ks = fmt("%.2f", kAll);
      if (byRef) {
        // like the reference by body part (the trunk, the head, the arms, the legs: a bone alone depends on how each
        // skin's weights share the vertices), each part the middle of its bones' ratios by their vertices
        ks.clear();
        auto part = [](int b) { return b == 4 ? 5 : chainOf(b) == 0 ? 0 : chainOf(b) <= 2 ? 1 : 3; };
        static const char *kPart[] = {"trunk", "arms", "", "legs", "", "head"};
        std::map<int, std::vector<std::pair<double, std::size_t>>> byPart;
        for (const auto &[b, sc] : its) {
          auto h = his.find(b);
          if (h == his.end() || h->second.size() < 8 || sc.size() < 8) continue;
          byPart[part(b)].push_back({outer(h->second) / std::max(1e-6, outer(sc)), sc.size()});
        }
        std::map<int, double> partK;
        for (auto &[pt, list] : byPart) {
          std::sort(list.begin(), list.end());
          std::size_t total = 0, seen2 = 0;
          for (const auto &x : list) total += x.second;
          double w = 1.0;
          for (const auto &x : list)
            if ((seen2 += x.second) * 2 >= total) {
              w = x.first;
              break;
            }
          w = std::max(0.6, std::min(1.25, w));
          partK[pt] = 1.0 - o.fitBody * (1.0 - w);
          ks += fmt("%s%s %.2f", ks.empty() ? "" : ", ", kPart[pt], partK[pt]);
        }
        for (const auto &[b, sc] : its)
          if (partK.count(part(b))) k[b] = partK.at(part(b));
      } else {
        // the head half as much (a helmet is part of the look; the hands and feet as the rest)
        for (const auto &[b, sc] : its) k[b] = b == 4 ? (1.0 + kAll) * 0.5 : kAll;
      }
      double lo = 1e30, hi = -1e30, lo2 = 1e30, hi2 = -1e30;
      for (std::size_t v = 0; v < nv; ++v) {
        V3 moved{0, 0, 0};
        double total = 0;
        for (const auto &[bone, r] : W[v]) {
          if (!skel.count(bone)) continue;
          V3 foot;
          const V3 d = radial(bone, P[v], foot);
          const double kb = k.count(bone) ? k.at(bone) : 1.0;
          moved = moved + (foot + d * kb) * double(r);
          total += r;
        }
        lo = std::min(lo, P[v].y), hi = std::max(hi, P[v].y);
        if (total > 0) P[v] = moved * (1.0 / total);
        lo2 = std::min(lo2, P[v].y), hi2 = std::max(hi2, P[v].y);
      }
      log += fmt("Leon's build: thinner round his bones (factor %s); %.0f..%.0f mm high, now %.0f..%.0f\n",
                 ks.empty() ? "none" : ks.c_str(), lo, hi, lo2, hi2);
    }

    // the triangles by material, facing as the disc's do (corners counter-clockwise round their normal)
    long facing = 0;
    for (const auto &t : m.tris) {
      if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
      const V3 c = cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]]);
      facing += dot(c, N[t[0]] + N[t[1]] + N[t[2]]) >= 0 ? 1 : -1;
    }
    std::map<unsigned, std::vector<std::array<unsigned, 3>>> byMat;
    for (const auto &t : m.tris) {
      if (t[0] >= nv || t[1] >= nv || t[2] >= nv || t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
      const unsigned mat = t[3] < m.matTex.size() ? t[3] : 0;
      byMat[mat].push_back(facing >= 0 ? std::array<unsigned, 3>{t[0], t[1], t[2]} : std::array<unsigned, 3>{t[0], t[2], t[1]});
    }
    // An inner layer (DevJunior, 19.11.2026: "where it breaks, fill those gaps with the same tone"): the whole skin
    // once more, kInset under its surface (deeper under the rigid plates), with the same texture and the smooth
    // weights: where plates or pieces part at a bending joint, the inside shows the same colours, not the world.
    if (o.innerLayer) {
      const double kInset = 10.0, kInsetPlate = 22.0;
      P.resize(nv * 2), N.resize(nv * 2), W.resize(nv * 2), m.uv.resize(nv * 2);
      for (std::size_t v = 0; v < nv; ++v) {
        P[nv + v] = P[v] - N[v] * (plated[v] ? kInsetPlate : kInset);
        N[nv + v] = N[v], W[nv + v] = Wsoft[v], m.uv[nv + v] = m.uv[v];
      }
      std::size_t inner = 0;
      for (auto &[mat, list] : byMat) {
        const std::size_t n = list.size();
        for (std::size_t k = 0; k < n; ++k) {
          const auto t = list[k];
          list.push_back({unsigned(t[0] + nv), unsigned(t[1] + nv), unsigned(t[2] + nv)});
          ++inner;
        }
      }
      log += fmt("an inner layer of %zu triangles under the surface fills the gaps at the joints\n", inner);
    }
    std::size_t corners = 0;
    for (const auto &[mat, list] : byMat) corners += list.size() * 3;
    // more than one model holds: the materials shared out over the body and the second model (a material that does
    // not fit cut in two)
    std::map<unsigned, std::vector<std::array<unsigned, 3>>> byMatB;
    if (corners > 0xFFFF && second) {
      const std::size_t kOne = 21845;
      std::size_t inA = 0;
      std::map<unsigned, std::vector<std::array<unsigned, 3>>> a;
      for (auto &[mat, list] : byMat) {
        const std::size_t fit = inA < kOne ? std::min(list.size(), kOne - inA) : 0;
        if (fit) a[mat].assign(list.begin(), list.begin() + long(fit));
        if (fit < list.size()) byMatB[mat].assign(list.begin() + long(fit), list.end());
        inA += fit;
      }
      byMat.swap(a);
      std::size_t inB = 0;
      for (const auto &[mat, list] : byMatB) inB += list.size();
      log += fmt("the skin in two models: %zu triangles in the body, %zu in entry %zu\n", inA, inB, second);
      corners = inA * 3;
      if (inB > kOne) fail(fmt("%zu triangles: at most 21845 fit the second model", inB));
    }
    if (corners > 0xFFFF) fail(fmt("%zu triangles: at most 21845 fit one model", corners / 3));

    // textures behind Leon's own in the body's TPL; a material without one gets its colour
    const Bytes tpl = gc.entry(1);
    const std::uint32_t n0 = be32(tpl, 4);
    std::vector<std::tuple<unsigned, unsigned, unsigned, Bytes>> add;
    std::map<std::string, unsigned> slotOf;
    std::map<unsigned, unsigned> matSlot;
    std::set<unsigned> mats;
    for (const auto &[mat, list] : byMat) mats.insert(mat);
    for (const auto &[mat, list] : byMatB) mats.insert(mat);
    for (const unsigned mat : mats) {
      const std::string name = m.matTex[mat];
      const auto c = m.matColour[mat];
      const std::string key = images.count(name) ? name : fmt("#%02x%02x%02x", c[0], c[1], c[2]);
      auto it = slotOf.find(key);
      if (it == slotOf.end()) {
        Rgba img;
        if (images.count(name)) {
          img = images.at(name);
        } else {
          img.w = img.h = 8;
          img.px.assign(8 * 8 * 4, 255);
          for (std::size_t q = 0; q < img.px.size(); q += 4) img.px[q] = c[0], img.px[q + 1] = c[1], img.px[q + 2] = c[2];
          if (!name.empty()) notes.push_back("  " + name + ": not in the .txd, the material's colour");
        }
        const double sc = std::min(1.0, double(o.tex) / double(std::max(img.w, img.h)));
        const int tw = std::max(8, 1 << int(pyround(std::log2(std::max(1.0, img.w * sc)))));
        const int th = std::max(8, 1 << int(pyround(std::log2(std::max(1.0, img.h * sc)))));
        const Rgba scaled = resize(img, tw, th);
        add.push_back({unsigned(tw), unsigned(th), 0xEu, encCmpr(scaled)});
        it = slotOf.emplace(key, unsigned(n0 + add.size() - 1)).first;
        log += fmt("  texture %u: %s %dx%d -> %dx%d\n", it->second, key.c_str(), img.w, img.h, tw, th);
      }
      matSlot[mat] = it->second;
    }
    for (const auto &nt : notes) log += nt + "\n";
    if (n0 + add.size() > 0xF7) fail("too many textures for one model");

    // the model in the PC release's layout, then the disc's (convertModel, Leon's skeleton kept)
    const Converted conv = buildModel(body, byMat, P, N, m.uv, W, matSlot, n0 + add.size(), std::size_t(o.palette));
    log += "entry   0 BIN: " + conv.stats + "\n";
    Converted convB;
    if (!byMatB.empty()) {
      convB = buildModel(body, byMatB, P, N, m.uv, W, matSlot, n0 + add.size(), std::size_t(o.palette));
      log += fmt("entry %3zu BIN: ", second) + convB.stats + "\n";
    }

    // a skin without a head of its own (a mod that leaves the head to the game) keeps the character's
    bool keepHead = o.keepHead;
    if (!keepHead && ownHead < 30) {
      keepHead = true;
      log += fmt("no head of its own (%zu vertices on the head): the character's head kept\n", ownHead);
    }
    // the archive: the new body and TPL, the character's own head, face, hair, outfit, wound and hands folded away
    // (entry = PL_ARC - 4 of each setModel: studio_model.cpp Characters; DevJunior, 20.11.2026: skins for Ashley and
    // Ada too)
    std::set<std::size_t> fold, hair;  // hair: the head's hair, kept with the head (in pl01/pl11 with her skirt)
    if (stem == "pl01" || stem == "pl05") {  // Ashley: hair 4 (pl01 with the skirt), outfit 6, hands, head 2, face 3
      fold = {6, 13, 14, 16, 17}, hair = {4};
      if (!keepHead) fold.insert({2, 3});
    } else if (stem == "pl11") {  // Ashley at Leon's side: hair and skirt 5, outfit 6, hands 8..12
      fold = {6, 8, 9, 10, 11, 12}, hair = {5};
      if (!keepHead) fold.insert({2, 3});
    } else if (stem == "pl0b" || stem == "pl0c") {  // Ada: hair 5, hands 13..17, head 2, face 4
      fold = {13, 14, 15, 16, 17}, hair = {5};
      if (!keepHead) fold.insert({2, 4});
    } else {  // Leon
      fold = {6, 10};
      if (!keepHead) fold.insert({2, 4, 5});
      if (stem != "pl00") fold.insert(12);
      for (std::size_t i = 14; i <= 21; ++i) fold.insert(i);
    }
    if (!keepHead) fold.insert(hair.begin(), hair.end()), hair.clear();
    // below the neck (her spine's top less 22 cm) a hair model's other part (the skirt) goes
    const double cut = skel.count(3) ? skel.at(3).at.y - 220.0 : 1e9;
    std::vector<Bytes> entries;
    std::string folded;
    for (std::size_t i = 0; i < gc.count(); ++i) {
      if (i == 0) entries.push_back(conv.blob);
      else if (i == 1) entries.push_back(appendTpl(tpl, add));
      else if (i == second && !byMatB.empty()) entries.push_back(convB.blob);
      else if (fold.count(i) && gc.kind(i) == "BIN" && !gc.entry(i).empty()) {
        entries.push_back(foldModel(gc.entry(i), skel));
        folded += (folded.empty() ? "" : ", ") + std::to_string(i);
      } else if (hair.count(i) && gc.kind(i) == "BIN" && !gc.entry(i).empty()) {
        entries.push_back(foldBelow(gc.entry(i), skel, cut));
        folded += (folded.empty() ? "" : ", ") + std::to_string(i) + fmt(" (below %.0f mm)", cut);
      } else {
        entries.push_back(gc.entry(i));
      }
    }
    log += "the character's own models folded away: " + folded + (keepHead ? " (the head kept)" : "") + "\n";
    const Bytes newBody = buildBody(entries, gc.types);
    result = buildArchive(gc, newBody);
    // a larger player file goes to the mods' place (patch 0901: up to 6 MB)
    log += fmt("%s: %zu bytes, data part %zu bytes (disc %zu; the mods' place for the player holds %d)\n", gcName.c_str(),
               result.size(), newBody.size(), gc.body.size(), 0x600000);
    if (newBody.size() > 0x600000) fail(fmt("too large for the mods' place for the player (%zu bytes): pick smaller "
                                            "textures", newBody.size()));
}

}  // namespace

bool ConvertDff(const Bytes &gcData, const std::string &gcName, const Bytes &dffData, const Bytes &txdData,
                const Options &o, Bytes &result, std::string &log, std::string *why) {
  try {
    DffMesh m = readDff(dffData);
    std::vector<std::string> notes;
    std::map<std::string, Rgba> images;
    if (!txdData.empty()) images = readTxd(txdData, notes);
    skinOnLeon(gcData, gcName, std::move(m), images, std::move(notes), o, result, log, "GTA skin");
    return true;
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    return false;
  }
}

bool ConvertReMod(const Bytes &gcData, const std::string &gcName, const std::map<std::string, Bytes> &files,
                  const Options &o, Bytes &result, std::string &log, std::string *why) {
  try {
    std::vector<std::string> notes;
    std::map<std::string, Rgba> images;
    // the outfit of the character this archive is: Leon cha0.., Ashley cha1.., Ada cha2..
    std::string stem = gcName;
    if (auto slash = stem.find_last_of("/\\"); slash != std::string::npos) stem = stem.substr(slash + 1);
    stem = lower(stem.substr(0, stem.find('.')));
    const char *who = (stem == "pl01" || stem == "pl05" || stem == "pl11") ? "cha1"
                      : (stem == "pl0b" || stem == "pl0c")                ? "cha2"
                                                                            : "cha0";
    DffMesh m = readReMod(files, images, notes, log, false, who);
    skinOnLeon(gcData, gcName, std::move(m), images, std::move(notes), o, result, log, "RE4 Remake mod", true);
    return true;
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    return false;
  }
}

// RE4 Remake weapon -> the disc's weapon archive it replaces (wish of DevJunior, 20.11.2026: "replace weapons or make
// weapon packs"; the pistols of the Remake, its ids as its folders name them). "" none known (the user picks).
const char *ReWeaponTarget(const std::string &wp) {
  static const std::map<std::string, const char *> kMap = {
      {"wp4000", "wep04"},  // SG-09 R -> the handgun
      {"wp4001", "wep02"},  // Punisher
      {"wp4002", "wep03"},  // Red9
      {"wp4003", "wep01"},  // Blacktail
      {"wp4004", "wep17"},  // Matilda
  };
  auto it = kMap.find(wp);
  return it == kMap.end() ? "" : it->second;
}

namespace {

// The disc weapon model's vertices (its space: -x the barrel's way, +z up) with each one's heaviest bone
struct WepVert {
  V3 p;
  int bone;
};
std::vector<WepVert> weaponVertices(const Bytes &model) {
  std::vector<WepVert> out;
  if (model.size() < 0x60) return out;
  const unsigned shift = model[0x28], nv = be16(model, 0x38);
  const std::uint32_t vtx = be32(model, 0x30), wgt = be32(model, 0x14);
  const unsigned ext = be16(model, 0x2A), n = ext > 0xFF ? ext : model[0x18];
  const bool big = ext > 0xFF;
  std::vector<int> heaviest(n, 0);
  for (unsigned k = 0; k < n; ++k) {
    const std::size_t e = wgt + std::size_t(k) * (big ? 12 : 8);
    if (e + (big ? 12 : 8) > model.size()) break;
    const unsigned cnt = std::min<unsigned>(big ? be16(model, e + 6) : model.at(e + 3), 3);
    int best = -1;
    for (unsigned j = 0; j < cnt; ++j) {
      const int r = model.at(e + (big ? 8 : 4) + j);
      if (r > best) best = r, heaviest[k] = big ? int(be16(model, e + j * 2)) : int(model.at(e + j));
    }
  }
  const double s = 1.0 / double(1 << shift);
  for (unsigned i = 0; i < nv; ++i) {
    const std::size_t q = vtx + std::size_t(i) * 8;
    if (q + 8 > model.size()) break;
    const unsigned wi = be16(model, q + 6);
    out.push_back({V3{double(std::int16_t(be16(model, q))) * s, double(std::int16_t(be16(model, q + 2))) * s,
                      double(std::int16_t(be16(model, q + 4))) * s},
                   wi < heaviest.size() ? heaviest[wi] : 0});
  }
  return out;
}

}  // namespace

bool ConvertReWeapon(const Bytes &gcData, const std::string &gcName, const std::map<std::string, Bytes> &files,
                     const std::string &wp, const Options &o, Bytes &result, std::string &log, std::string *why) {
  try {
    const Archive gc(gcData, true);
    std::vector<std::string> notes;
    std::map<std::string, Rgba> images;
    std::string picked;
    DffMesh m = readReMod(files, images, notes, log, true, wp, &picked);

    // the weapon's own pieces: not the loose round its reload shows (material "Bullet")
    {
      std::vector<std::array<unsigned, 4>> keep;
      std::size_t dropped = 0;
      for (const auto &t : m.tris) {
        const std::string name = t[3] < m.matName.size() ? lower(m.matName[t[3]]) : "";
        if (name.find("bullet") != std::string::npos) ++dropped;
        else keep.push_back(t);
      }
      if (keep.empty()) fail("the weapon's meshes hold only rounds");
      if (dropped) log += fmt("the loose round left out (%zu triangles)\n", dropped);
      m.tris = std::move(keep);
    }

    // the disc's models of this weapon: the first BIN entry on a skeleton of a few bones at the weapon's place (not the
    // shell, the magazine dropped in the case's space) is the plain weapon; the later ones on the same skeleton vary it
    // (a silencer, a stock); others (another weapon's copy in the Matilda's archive) stay
    struct Target {
      std::size_t entry;
      std::map<int, DiscBone> skel;
      double lo = 1e30, hi = -1e30;
    };
    std::vector<Target> targets;
    for (std::size_t e = 0; e < gc.count(); ++e) {
      if (gc.kind(e) != "BIN" || gc.entry(e).size() < 0x60) continue;
      const Bytes model = gc.entry(e);
      Target t{e, discSkeleton(model)};
      bool sane = t.skel.size() >= 2 && t.skel.size() <= 64;
      for (const auto &[id, b] : t.skel) sane = sane && id < 64 && std::fabs(b.at.x) < 2000 && std::fabs(b.at.z) < 2000;
      if (!sane) continue;
      for (const auto &v : weaponVertices(model)) t.lo = std::min(t.lo, v.p.x), t.hi = std::max(t.hi, v.p.x);
      if (t.hi - t.lo < 60) continue;
      if (!targets.empty()) {
        bool same = t.skel.size() == targets[0].skel.size();
        for (const auto &[id, b] : t.skel) {
          auto it = targets[0].skel.find(id);
          same = same && it != targets[0].skel.end() && len(it->second.at - b.at) < 0.5;
        }
        if (!same) continue;
      }
      targets.push_back(std::move(t));
    }
    if (targets.empty()) fail(gcName + " has no weapon model to replace");
    const Target &ref = targets[0];
    // its muzzle: the bone farthest along the barrel (the flash comes out there)
    V3 gcMuzzle{0, 0, 0};
    double front = 1e30;
    for (const auto &[id, b] : ref.skel)
      if (id != 0 && b.at.x < front) front = b.at.x, gcMuzzle = b.at;

    // the Remake's weapon: +z the barrel's way, +y up, metres; its muzzle bone vfx_muzzle1 (else the front)
    double reLo = 1e30, reHi = -1e30;
    std::vector<char> used(m.pos.size(), 0);
    for (const auto &t : m.tris)
      for (int c = 0; c < 3; ++c) used[t[std::size_t(c)]] = 1;
    for (std::size_t v = 0; v < m.pos.size(); ++v)
      if (used[v]) reLo = std::min(reLo, m.pos[v].z), reHi = std::max(reHi, m.pos[v].z);
    V3 reMuzzle{0, 0, reHi};
    bool muzzle = false;
    for (std::size_t b = 0; b < m.boneName.size(); ++b)
      if (lower(m.boneName[b]) == "vfx_muzzle1") reMuzzle = m.boneAt[b], muzzle = true;
    if (!muzzle) {  // the middle of the front
      double ylo = 1e30, yhi = -1e30;
      for (std::size_t v = 0; v < m.pos.size(); ++v)
        if (used[v] && m.pos[v].z > reHi - 0.01) ylo = std::min(ylo, m.pos[v].y), yhi = std::max(yhi, m.pos[v].y);
      reMuzzle = V3{0, (ylo + yhi) * 0.5, reHi};
    }
    // Placed on the disc's plain weapon (the hand's grip, the motions and the flash are the disc's): first as high as it
    // with the muzzles on each other, then moved and scaled (the axes kept) until the two shapes lie best on each other
    // (both ways nearest points, 30 rounds): the grip goes into the hand even where the barrel or the back differ
    // (DevJunior's Halo magnum: a long back; the muzzles alone left the grip 8 cm off the hand)
    auto R = [](V3 d) { return V3{-d.z, -d.x, d.y}; };  // the Remake's axes -> the disc's
    // the first size: as high as the disc's (the grip fits the hand; the barrels differ more: a Red9's is twice a
    // magnum's)
    double gzLo = 1e30, gzHi = -1e30, ryLo = 1e30, ryHi = -1e30;
    for (const auto &g : weaponVertices(gc.entry(ref.entry))) gzLo = std::min(gzLo, g.p.z), gzHi = std::max(gzHi, g.p.z);
    for (std::size_t v = 0; v < m.pos.size(); ++v)
      if (used[v]) ryLo = std::min(ryLo, m.pos[v].y), ryHi = std::max(ryHi, m.pos[v].y);
    double scale = (gzHi - gzLo) / std::max(1e-6, ryHi - ryLo);
    V3 shift = gcMuzzle - R(reMuzzle) * scale;
    {
      std::vector<V3> a, b;
      for (std::size_t v = 0; v < m.pos.size(); v += 1 + m.pos.size() / 3000)
        if (used[v]) a.push_back(R(m.pos[v]));
      for (const auto &g : weaponVertices(gc.entry(ref.entry))) b.push_back(g.p);
      const double s0 = scale;
      for (int it = 0; it < 30 && !a.empty() && !b.empty(); ++it) {
        std::vector<std::pair<V3, V3>> pairs;  // (rotated Remake point, disc point)
        for (const auto &p : a) {
          const V3 q = p * scale + shift;
          double best = 1e30;
          V3 hit{};
          for (const auto &g : b) {
            const V3 d = g - q;
            const double dd = dot(d, d);
            if (dd < best) best = dd, hit = g;
          }
          pairs.push_back({p, hit});
        }
        for (const auto &g : b) {
          double best = 1e30;
          V3 hit{};
          for (const auto &p : a) {
            const V3 d = p * scale + shift - g;
            const double dd = dot(d, d);
            if (dd < best) best = dd, hit = p;
          }
          // as many as the Remake's side: both shapes count the same
          for (std::size_t k = 0; k < std::max<std::size_t>(1, a.size() / b.size()); ++k) pairs.push_back({hit, g});
        }
        V3 ma{}, mb{};
        for (const auto &[p, g] : pairs) ma = ma + p, mb = mb + g;
        ma = ma * (1.0 / double(pairs.size())), mb = mb * (1.0 / double(pairs.size()));
        double num = 0, den = 0;
        for (const auto &[p, g] : pairs) num += dot(p - ma, g - mb), den += dot(p - ma, p - ma);
        if (den > 0) scale = std::min(s0 * 1.25, std::max(s0 * 0.8, num / den));
        shift = mb - ma * scale;
      }
      // then the grips on each other along the barrel (the hand holds the disc's): the middle of the lowest 40 % of each
      auto gripX = [](const std::vector<V3> &pts) {
        double lo = 1e30, hi = -1e30, sum = 0;
        std::size_t n = 0;
        for (const auto &p : pts) lo = std::min(lo, p.z), hi = std::max(hi, p.z);
        for (const auto &p : pts)
          if (p.z < lo + 0.4 * (hi - lo)) sum += p.x, ++n;
        return n ? sum / double(n) : 0.0;
      };
      std::vector<V3> placed;
      for (const auto &p : a) placed.push_back(p * scale + shift);
      if (!a.empty() && !b.empty()) {
        const double dx = gripX(b) - gripX(placed);
        shift.x += dx;
        log += fmt("the grip moved %.0f mm along the barrel onto the disc's\n", dx);
      }
    }
    auto G = [&](V3 p) { return R(p) * scale + shift; };
    auto Gdir = [&](V3 n) { return unit(R(n)); };
    {
      const V3 mz = G(reMuzzle);
      log += fmt("into %s (entry %zu, %.0f mm long): scale %.1f, its muzzle %.0f mm from the disc's%s\n", gcName.c_str(),
                 ref.entry, ref.hi - ref.lo, scale, len(mz - gcMuzzle), muzzle ? "" : " (no muzzle bone: the front's middle)");
    }

    // the texture palette the weapon's models draw with: the TPL entry nearest the plain model (the one before on a tie)
    std::size_t tplEntry = gc.count();
    for (std::size_t e = 0; e < gc.count(); ++e) {
      if (gc.kind(e) != "TPL" || gc.entry(e).empty()) continue;
      auto dist = [&](std::size_t a) { return a > ref.entry ? 2 * (a - ref.entry) : 2 * (ref.entry - a) - 1; };
      if (tplEntry == gc.count() || dist(e) < dist(tplEntry)) tplEntry = e;
    }
    if (tplEntry == gc.count()) fail(gcName + " has no texture palette");
    const Bytes tpl = gc.entry(tplEntry);
    const std::uint32_t n0 = be32(tpl, 4);

    // sizes down until the data part fits the weapon's place (ReadWepData, read.cpp: WEAPON_DATA_MAX 0x70000)
    const std::size_t kBudget = 0x70000 - 0x1000;
    std::size_t tris = std::min<std::size_t>(m.tris.size(), 4000);
    int tex = std::max(32, o.tex);
    for (int round = 0;; ++round) {
      std::string rlog;
      DffMesh r = m;
      reduceSkin(r, tris, rlog);
      const std::size_t nv = r.pos.size();
      // rigid on the disc's bones: each of the Remake's bones (a slide, a trigger, a magazine) on the disc bone most of
      // its vertices lie nearest to (in the plain model), so a piece moves whole
      std::vector<V3> P(nv), N(nv);
      for (std::size_t v = 0; v < nv; ++v) P[v] = G(r.pos[v]), N[v] = Gdir(r.nrm[v]);
      const auto gv = weaponVertices(gc.entry(ref.entry));
      std::map<unsigned, std::map<int, int>> votes;
      std::vector<unsigned> own(nv, 0);
      for (std::size_t v = 0; v < nv; ++v) {
        unsigned b = r.bone[v][0];
        float w = r.weight[v][0];
        for (int j = 1; j < 4; ++j)
          if (r.weight[v][std::size_t(j)] > w) w = r.weight[v][std::size_t(j)], b = r.bone[v][std::size_t(j)];
        own[v] = b;
        double best = 1e30;
        int bone = 0;
        for (const auto &g : gv) {
          const V3 d = g.p - P[v];
          const double dd = dot(d, d);
          if (dd < best) best = dd, bone = g.bone;
        }
        ++votes[b][bone];
      }
      std::map<unsigned, int> boneOf;
      for (const auto &[b, tally] : votes)
        boneOf[b] = std::max_element(tally.begin(), tally.end(), [](const auto &a, const auto &c) { return a.second < c.second; })->first;
      std::vector<Weight> W(nv);
      for (std::size_t v = 0; v < nv; ++v) W[v] = Weight{{boneOf[own[v]], 100}};

      // the triangles by material, facing as the disc's do
      long facing = 0;
      for (const auto &t : r.tris) {
        const V3 c = cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]]);
        facing += dot(c, N[t[0]] + N[t[1]] + N[t[2]]) >= 0 ? 1 : -1;
      }
      std::map<unsigned, std::vector<std::array<unsigned, 3>>> byMat;
      for (const auto &t : r.tris) {
        if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2]) continue;
        byMat[t[3]].push_back(facing >= 0 ? std::array<unsigned, 3>{t[0], t[1], t[2]} : std::array<unsigned, 3>{t[0], t[2], t[1]});
      }
      // the textures behind the palette's own
      std::vector<std::tuple<unsigned, unsigned, unsigned, Bytes>> add;
      std::map<std::string, unsigned> slotOf;
      std::map<unsigned, unsigned> matSlot;
      std::string texLog;
      for (const auto &[mat, list] : byMat) {
        const std::string name = r.matTex[mat];
        auto it = slotOf.find(name);
        if (it == slotOf.end()) {
          Rgba img;
          if (images.count(name)) {
            img = images.at(name);
          } else {
            img.w = img.h = 8;
            img.px.assign(8 * 8 * 4, 160);
            for (std::size_t q = 3; q < img.px.size(); q += 4) img.px[q] = 255;
          }
          const double sc = std::min(1.0, double(tex) / double(std::max(img.w, img.h)));
          const int tw = std::max(8, 1 << int(pyround(std::log2(std::max(1.0, img.w * sc)))));
          const int th = std::max(8, 1 << int(pyround(std::log2(std::max(1.0, img.h * sc)))));
          add.push_back({unsigned(tw), unsigned(th), 0xEu, encCmpr(resize(img, tw, th))});
          it = slotOf.emplace(name, unsigned(n0 + add.size() - 1)).first;
          texLog += fmt("  texture %u: %s %dx%d -> %dx%d\n", it->second, name.c_str(), img.w, img.h, tw, th);
        }
        matSlot[mat] = it->second;
      }
      if (n0 + add.size() > 0xF7) fail("too many textures for one model");
      // every model of the weapon anew (the variants too: the Remake's weapon has no silencer or stock of the disc's)
      std::vector<Bytes> entries;
      std::string modelLog;
      std::set<std::size_t> replaced;
      for (const auto &t : targets) replaced.insert(t.entry);
      for (std::size_t e = 0; e < gc.count(); ++e) {
        if (e == tplEntry) {
          entries.push_back(appendTpl(tpl, add));
        } else if (replaced.count(e)) {
          const Converted conv = buildModel(gc.entry(e), byMat, P, N, r.uv, W, matSlot, n0 + add.size(),
                                            std::size_t(o.palette));
          modelLog += fmt("entry %3zu BIN: %s\n", e, conv.stats.c_str());
          entries.push_back(conv.blob);
        } else {
          entries.push_back(gc.entry(e));
        }
      }
      const Bytes body = buildBody(entries, gc.types);
      if (body.size() <= kBudget || (tris <= 300 && tex <= 32)) {
        log += rlog;
        std::string bones;
        for (const auto &[b, d] : boneOf)
          bones += fmt("%s%s -> %d", bones.empty() ? "" : ", ", b < r.boneName.size() ? r.boneName[b].c_str() : "?", d);
        log += "pieces on the disc's bones: " + bones + "\n" + texLog + modelLog;
        for (const auto &nt : notes) log += nt + "\n";
        if (body.size() > kBudget) fail(fmt("too large for the weapon's place even at its smallest (%zu bytes)", body.size()));
        result = buildArchive(gc, body);
        log += fmt("%s: %zu bytes, data part %zu bytes of %zu (round %d: %zu triangles, textures at most %d)\n",
                   gcName.c_str(), result.size(), body.size(), kBudget, round, r.tris.size(), tex);
        return true;
      }
      // smaller: the triangles first, the textures once the models are under 1500
      if (tris > 1500) tris = std::max<std::size_t>(1500, tris * 4 / 5);
      else if (tex > 32) tex /= 2;
      else tris = std::max<std::size_t>(300, tris * 4 / 5);
    }
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    return false;
  }
}

namespace {

// An image padded to whole tiles (its edge repeated): GX keeps the tiles whole, the header the image's own size
Rgba padTiles(const Rgba &in, int tw = 4, int th = 4) {
  Rgba o;
  o.w = (in.w + tw - 1) / tw * tw, o.h = (in.h + th - 1) / th * th;
  o.px.resize(std::size_t(o.w) * std::size_t(o.h) * 4);
  for (int y = 0; y < o.h; ++y)
    for (int x = 0; x < o.w; ++x) {
      const int sx = std::min(x, in.w - 1), sy = std::min(y, in.h - 1);
      std::memcpy(&o.px[(std::size_t(y) * std::size_t(o.w) + std::size_t(x)) * 4],
                  &in.px[(std::size_t(sy) * std::size_t(in.w) + std::size_t(sx)) * 4], 4);
    }
  return o;
}

// GX RGBA8 (6): per 4x4 tile 32 bytes of alpha and red, then 32 of green and blue
Bytes encRgba8(const Rgba &in) {
  const Rgba img = padTiles(in);
  Bytes out;
  for (int by = 0; by < img.h; by += 4)
    for (int bx = 0; bx < img.w; bx += 4) {
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const auto *c = &img.px[(std::size_t(by + y) * std::size_t(img.w) + std::size_t(bx + x)) * 4];
          out.push_back(c[3]), out.push_back(c[0]);
        }
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const auto *c = &img.px[(std::size_t(by + y) * std::size_t(img.w) + std::size_t(bx + x)) * 4];
          out.push_back(c[1]), out.push_back(c[2]);
        }
    }
  return out;
}

// GX IA8 (3) of any size: alpha, then the grey
Bytes encIa8Any(const Rgba &in) {
  const Rgba img = padTiles(in);
  Bytes out;
  for (int by = 0; by < img.h; by += 4)
    for (int bx = 0; bx < img.w; bx += 4)
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const auto *c = &img.px[(std::size_t(by + y) * std::size_t(img.w) + std::size_t(bx + x)) * 4];
          out.push_back(c[3]), out.push_back(std::uint8_t((c[0] * 3 + c[1] * 6 + c[2]) / 10));
        }
  return out;
}

// GX I8 (1): 8x4 tiles of grey
Bytes encI8Any(const Rgba &in) {
  const Rgba img = padTiles(in, 8, 4);
  Bytes out;
  for (int by = 0; by < img.h; by += 4)
    for (int bx = 0; bx < img.w; bx += 8)
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 8; ++x) {
          const auto *c = &img.px[(std::size_t(by + y) * std::size_t(img.w) + std::size_t(bx + x)) * 4];
          out.push_back(std::uint8_t((c[0] * 3 + c[1] * 6 + c[2]) / 10));
        }
  return out;
}

// GX RGB5A3 (5): opaque 1rrrrrgggggbbbbb, else 0aaarrrrggggbbbb
Bytes encRgb5a3(const Rgba &in) {
  const Rgba img = padTiles(in);
  Bytes out;
  for (int by = 0; by < img.h; by += 4)
    for (int bx = 0; bx < img.w; bx += 4)
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const auto *c = &img.px[(std::size_t(by + y) * std::size_t(img.w) + std::size_t(bx + x)) * 4];
          unsigned v;
          if (c[3] >= 0xF8) v = 0x8000u | unsigned(c[0] >> 3) << 10 | unsigned(c[1] >> 3) << 5 | unsigned(c[2] >> 3);
          else v = unsigned(c[3] >> 5) << 12 | unsigned(c[0] >> 4) << 8 | unsigned(c[1] >> 4) << 4 | unsigned(c[2] >> 4);
          out.push_back(std::uint8_t(v >> 8)), out.push_back(std::uint8_t(v));
        }
  return out;
}

// An effect texture bank (IdTexDataLoad, version 0xB): each TPL's id and its byte range
struct BankTpl {
  unsigned id;
  std::size_t at, end;
};
std::vector<BankTpl> bankTpls(const Bytes &d, bool be) {
  auto r32 = [&](std::size_t p) { return be ? be32(d, p) : le32(d, p); };
  auto r16 = [&](std::size_t p) { return be ? be16(d, p) : le16(d, p); };
  if (d.size() < 0x20 || r32(0) != 0xB) fail("not a texture bank (version 0xB)");
  const std::uint32_t ofsId = r32(4), ofsTpl = r32(0x18), ofsAnm = r32(0x1C), n = r32(ofsId);
  if (r32(ofsTpl) != n) fail("the texture bank's tables differ");
  // each TPL runs to the next one's start (in the bank's order, not the table's), the last to the animations
  std::vector<std::size_t> starts;
  for (std::uint32_t i = 0; i < n; ++i) starts.push_back(ofsTpl + r32(ofsTpl + 4 + i * 4));
  std::vector<std::size_t> sorted = starts;
  sorted.push_back(ofsAnm > starts.back() ? ofsAnm : d.size());
  std::sort(sorted.begin(), sorted.end());
  std::vector<BankTpl> out;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::size_t at = starts[i];
    const std::size_t end = *std::upper_bound(sorted.begin(), sorted.end(), at);
    if (end > d.size()) fail("the texture bank's TPLs run past its end");
    out.push_back({r16(ofsId + 4 + i * 8), at, end});
  }
  return out;
}

}  // namespace

bool ConvertCoreHud(const Bytes &gcData, const Bytes &pcData, const Packs &packs, const Options &o, Bytes &result,
                    std::string &log, std::string *why) {
  try {
    const Archive gc(gcData, true);
    const Bytes pcBytes = IsLfs(pcData) ? UnpackLfs(pcData) : pcData;
    const Archive pc(pcBytes, false);
    // the cockpit's textures: core slot 0x1D (CORE_COCKPIT_TEXTURE), sub-file 0x19
    const std::size_t kBank = 0x19;
    if (gc.count() <= kBank || pc.count() <= kBank || gc.kind(kBank) != "EFF" || pc.kind(kBank) != "EFF")
      fail("not the core archives (etc/core.das, etc/core.udas): no cockpit textures at entry 25");
    const Bytes g = gc.entry(kBank), p = pc.entry(kBank);
    const auto gt = bankTpls(g, true), pt = bankTpls(p, false);
    std::map<unsigned, const BankTpl *> pcById;
    for (const auto &t : pt) pcById[t.id] = &t;
    // which PC texture takes a disc texture's place: the one the PC's HUD layouts (the id files, kind UWF) put on the
    // same unit (mostly the same id; the PC moved a few, and gave some unused ids other pictures: controller prompts).
    // A texture no layout uses (the game's code picks it: sparkles, the scope) stays the disc's.
    std::map<unsigned, std::map<unsigned, int>> votes;
    for (std::size_t e = 0; e < std::min(gc.count(), pc.count()); ++e) {
      if (gc.kind(e) != "UWF" || pc.kind(e) != "UWF") continue;
      const Bytes x = gc.entry(e), y = pc.entry(e);
      if (x.size() < 8 || y.size() < 8) continue;
      std::map<std::pair<int, int>, std::pair<int, int>> pcUnit;  // (id, no) -> (texture, mask)
      for (unsigned k = 0; k < y[5] && 8 + (k + 1) * 0x8C <= y.size(); ++k) {
        const std::size_t q = 8 + k * 0x8C;
        pcUnit[{y[q + 4], y[q + 5]}] = {y[q + 0xB], y[q + 0x6E]};
      }
      for (unsigned k = 0; k < x[5] && 8 + (k + 1) * 0x8C <= x.size(); ++k) {
        const std::size_t q = 8 + k * 0x8C;
        auto it = pcUnit.find({x[q + 4], x[q + 5]});
        if (it == pcUnit.end()) continue;
        ++votes[x[q + 0xB]][unsigned(it->second.first)];
        if (x[q + 0x6E] != 0xFF && it->second.second != 0xFF) ++votes[x[q + 0x6E]][unsigned(it->second.second)];
      }
    }
    std::map<unsigned, unsigned> pcFor;
    for (const auto &[gid, tally] : votes)
      pcFor[gid] = std::max_element(tally.begin(), tally.end(), [](const auto &a, const auto &b) { return a.second < b.second; })->first;

    // what each disc texture becomes: the PC one of the same id with as many pictures, its shape near the disc's
    struct Plan {
      std::vector<Rgba> images;  // the pack's, one per picture; empty: the disc's TPL as it is
    };
    std::vector<Plan> plans(gt.size());
    std::size_t taken = 0;
    for (std::size_t i = 0; i < gt.size(); ++i) {
      const BankTpl &t = gt[i];
      const std::uint32_t n = be32(g, t.at + 4);
      auto mapped = pcFor.find(t.id);
      if (mapped == pcFor.end()) continue;  // no layout uses it
      // the GameCube's own buttons stay (the PC shows its controller's: an "X" where the disc has the green A, the
      // B left out), as does the grenade of the weapon circle (an eye in the PC's picture of that place)
      if (t.id == 40 || t.id == 42 || t.id == 53) {
        log += fmt("  id %3u: the GameCube's own (a button or the grenade): the disc's\n", t.id);
        continue;
      }
      auto it = pcById.find(mapped->second);
      if (it == pcById.end()) continue;
      const BankTpl &q = *it->second;
      const std::uint32_t pn = le32(p, q.at + 4);
      const std::size_t gh0 = t.at + be32(g, t.at + 12), ph0 = q.at + le32(p, q.at + 12);
      const double ga = double(be16(g, gh0 + 2)) / std::max(1, int(be16(g, gh0))),
                   pa = double(le16(p, ph0 + 2)) / std::max(1, int(le16(p, ph0)));
      if (pn != n || std::fabs(std::log(ga / pa)) > 0.35) {
        log += fmt("  id %3u: the PC's differs (%u pictures %ux%u, the disc's %u %ux%u): the disc's\n", t.id, pn,
                   le16(p, ph0 + 2), le16(p, ph0), n, be16(g, gh0 + 2), be16(g, gh0));
        continue;
      }
      bool ok = true;
      for (std::uint32_t k = 0; k < n && ok; ++k) {
        const std::size_t ho = q.at + le32(p, q.at + 12 + k * 8), data = q.at + le32(p, ho + 8);
        Packs::Image im;
        std::string w;
        if (data + 8 > p.size() || !packs.image(le32(p, data), le32(p, data + 4), im, &w)) {
          log += fmt("  id %3u: picture %u not in the pack (%s): the disc's\n", t.id, k, w.c_str());
          ok = false;
          break;
        }
        Rgba r;
        r.w = im.width, r.h = im.height, r.px = std::move(im.rgba);
        plans[i].images.push_back(std::move(r));
      }
      // only what the mod changed: a picture as the disc's (its art in the PC's size) stays the disc's, smaller in
      // its own format (the pack holds the PC's whole core: effects, fonts, film grain)
      if (ok) {
        const Bytes tplBytes(g.begin() + long(t.at), g.begin() + long(t.end));
        double worst = 0;
        for (std::uint32_t k = 0; k < n; ++k) {
          re4::Texture d;
          try {
            d = re4::DecodeTpl(std::vector<std::uint8_t>(tplBytes.begin(), tplBytes.end()), k);
          } catch (...) {
            worst = 1e9;
            break;
          }
          if (d.width == 0 || d.rgba.size() < std::size_t(d.width) * d.height * 4) {
            worst = 1e9;
            break;
          }
          const Rgba small = resize(plans[i].images[k], int(d.width), int(d.height));
          double sum = 0;
          for (std::size_t q = 0; q < d.rgba.size(); q += 4) {
            const double ad = d.rgba[q + 3] / 255.0, as = small.px[q + 3] / 255.0;
            double e = std::fabs(ad - as) * 255.0;
            for (int ch = 0; ch < 3; ++ch) e += std::fabs(d.rgba[q + ch] * ad - small.px[q + ch] * as) / 3.0;
            sum += e;
          }
          worst = std::max(worst, sum / double(d.width * d.height));
        }
        if (worst < o.hudSame) {
          log += fmt("  id %3u: as the disc's (difference %.1f): the disc's\n", t.id, worst);
          plans[i].images.clear();
          continue;
        }
        log += fmt("  id %3u: changed (difference %.1f)\n", t.id, worst);
      }
      if (!ok) plans[i].images.clear();
      else ++taken;
    }
    if (!taken) fail("none of the HUD's textures is in the picked pack (07000000.pack)");

    // the sizes: the disc's times `scale`, from 4 down until the core fits the place of a larger core (3 MB)
    const std::size_t kCoreMax = 0x300000;
    for (double scale : {4.0, 3.0, 2.0, 1.5, 1.0}) {
      for (int fallback = 0; fallback < 2; ++fallback) {  // 1: RGB5A3 (2 bytes) for the coloured ones
        Bytes bank(g.begin(), g.begin() + long(be32(g, 0x18)));  // header, id and other tables up to the TPL table
        const std::uint32_t ofsTpl = be32(g, 0x18);
        Bytes table(4 + gt.size() * 4, 0);
        set32(table, 0, std::uint32_t(gt.size()));
        bank.insert(bank.end(), table.begin(), table.end());
        std::size_t bytesNew = 0;
        std::string detail;
        for (std::size_t i = 0; i < gt.size(); ++i) {
          pad(bank);
          set32(bank, ofsTpl + 4 + i * 4, std::uint32_t(bank.size() - ofsTpl));
          const BankTpl &t = gt[i];
          if (plans[i].images.empty()) {
            bank.insert(bank.end(), g.begin() + long(t.at), g.begin() + long(t.end));
            continue;
          }
          // a new TPL: the disc's wrap and filter settings, each picture the disc's size times the scale
          const std::uint32_t n = be32(g, t.at + 4);
          const std::size_t base = bank.size();
          Bytes tpl(12 + n * 8, 0);
          set32(tpl, 0, 0x0020AF30), set32(tpl, 4, n), set32(tpl, 8, 12);
          for (std::uint32_t k = 0; k < n; ++k) {
            const std::size_t gho = t.at + be32(g, t.at + 12 + k * 8);
            const unsigned gw = be16(g, gho + 2), gh = be16(g, gho), gf = be32(g, gho + 4);
            const Rgba &src = plans[i].images[k];
            const int w = std::max(1, std::min(src.w, int(std::lround(gw * scale)))),
                      h = std::max(1, std::min(src.h, int(std::lround(gh * scale))));
            const Rgba img = resize(src, w, h);
            // the format by what the picture needs: grey without alpha I8 (1 byte), grey IA8 (2), colour with no or hard
            // alpha CMPR (half a byte), soft alpha RGBA8 (4; RGB5A3, 2, when the core would not fit)
            bool soft = false, clear = false;
            for (std::size_t q = 3; q < img.px.size(); q += 4) {
              soft = soft || (img.px[q] > 16 && img.px[q] < 240);
              clear = clear || img.px[q] < 128;
            }
            const bool grey = gf <= 3;  // I4, I8, IA4, IA8 on the disc
            unsigned fmtGx;
            Bytes data;
            if (grey && (gf <= 1) && !clear) fmtGx = 1, data = encI8Any(img);
            else if (grey) fmtGx = 3, data = encIa8Any(img);
            else if (!soft) fmtGx = 14, data = encCmpr(padTiles(img, 8, 8));
            else if (!fallback) fmtGx = 6, data = encRgba8(img);
            else fmtGx = 5, data = encRgb5a3(img);
            static const char *kName[] = {"I4", "I8", "IA4", "IA8", "?", "RGB5A3", "RGBA8", "?", "?", "?", "?", "?", "?",
                                          "?", "CMPR"};
            pad(tpl);
            const std::size_t hd = tpl.size();
            tpl.resize(hd + 0x24, 0);
            pad(tpl);
            const std::size_t at = tpl.size();
            tpl.insert(tpl.end(), data.begin(), data.end());
            set32(tpl, 12 + k * 8, std::uint32_t(hd)), set32(tpl, 16 + k * 8, 0);
            // header: height, width, format, data, then the disc's wrap S/T, filters, LOD bias and edge LOD; one level
            std::copy(g.begin() + long(gho), g.begin() + long(gho + 0x24), tpl.begin() + long(hd));
            tpl[hd] = std::uint8_t(h >> 8), tpl[hd + 1] = std::uint8_t(h), tpl[hd + 2] = std::uint8_t(w >> 8),
            tpl[hd + 3] = std::uint8_t(w);
            set32(tpl, hd + 4, fmtGx), set32(tpl, hd + 8, std::uint32_t(at));
            tpl[hd + 0x21] = 0, tpl[hd + 0x22] = 0;  // min and max LOD: the one level
            if (be32(tpl, hd + 0x10) > 1) set32(tpl, hd + 0x10, 1);  // a mipmap filter without mipmaps: linear
            if (k == 0) detail += fmt("  id %3u: %u x %ux%u -> %dx%d %s", t.id, n, gw, gh, w, h, kName[fmtGx]);
          }
          bytesNew += tpl.size();
          detail += fmt(", %zu bytes\n", tpl.size());
          (void) base;
          bank.insert(bank.end(), tpl.begin(), tpl.end());
        }
        pad(bank);
        const std::uint32_t ofsAnm = be32(g, 0x1C);
        set32(bank, 0x1C, std::uint32_t(bank.size()));
        bank.insert(bank.end(), g.begin() + long(ofsAnm), g.end());
        std::vector<Bytes> entries;
        for (std::size_t e = 0; e < gc.count(); ++e) entries.push_back(e == kBank ? bank : gc.entry(e));
        const Bytes body = buildBody(entries, gc.types);
        if (body.size() > kCoreMax && !(scale == 1.0 && fallback == 1)) continue;
        if (body.size() > kCoreMax) fail(fmt("the core is too large even at the disc's sizes (%zu bytes)", body.size()));
        result = buildArchive(gc, body);
        log += detail;
        log += fmt("cockpit textures: %zu of %zu from the pack, at %.1f times the disc's size%s (%zu bytes of pictures)\n",
                   taken, gt.size(), scale, fallback ? ", the coloured ones as RGB5A3" : "", bytesNew);
        log += fmt("etc/core.das: %zu bytes, data part %zu (the disc's %zu; a core over %u bytes is read to the larger "
                   "place, at most %zu)\n", result.size(), body.size(), gc.body.size(), 0x234000u, kCoreMax);
        return true;
      }
    }
    fail("the core does not fit");
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    return false;
  }
  return false;
}

bool IsLfs(const Bytes &data) { return data.size() >= 4 && std::memcmp(data.data(), "RDLX", 4) == 0; }

Bytes UnpackLfs(const Bytes &data) {
  if (!IsLfs(data)) fail("not an .lfs file");
  const std::uint32_t total = le32(data, 8), count = le32(data, 16);
  Bytes out;
  out.reserve(total);
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t sizes = le32(data, 20 + i * 8), off = le32(data, 24 + i * 8);
    const std::size_t csize = sizes & 0xFFFF, usize = (sizes >> 16) ? (sizes >> 16) : 0x10000;
    const std::size_t start = std::size_t(off) + 0xF + 4;
    if (csize == usize) {
      if (start + usize > data.size()) fail("lfs: truncated");
      out.insert(out.end(), data.begin() + long(start), data.begin() + long(start + usize));
      continue;
    }
    const Bytes c = xmemDecompress(data, start, start + csize, usize);
    out.insert(out.end(), c.begin(), c.end());
  }
  if (out.size() != total) fail("lfs: unpacked " + std::to_string(out.size()) + " bytes, the header says " +
                                std::to_string(total));
  return out;
}

}  // namespace re4::studio::convert
