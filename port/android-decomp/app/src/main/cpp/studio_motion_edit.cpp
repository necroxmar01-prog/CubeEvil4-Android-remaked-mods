// CubeEvil4 Studio: the animation editor (studio_motion_edit.h). Big-endian throughout, as on the disc.
#include "studio_motion_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace re4::studio::motionedit {

namespace {

const std::size_t kAlign = 32;
std::size_t alignUp(std::size_t v, std::size_t a) { return (v + a - 1) & ~(a - 1); }

unsigned u16(const std::uint8_t *p) { return unsigned(p[0]) << 8 | p[1]; }
std::uint32_t u32(const std::uint8_t *p) {
  return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3];
}
float f32(const std::uint8_t *p) {
  const std::uint32_t v = u32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void put16(Bytes &o, unsigned v) {
  o.push_back(std::uint8_t(v >> 8));
  o.push_back(std::uint8_t(v));
}
void put32(Bytes &o, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) o.push_back(std::uint8_t(v >> s));
}
void putF(Bytes &o, float f) {
  std::uint32_t v;
  std::memcpy(&v, &f, 4);
  put32(o, v);
}
void set16(Bytes &o, std::size_t at, unsigned v) {
  o[at] = std::uint8_t(v >> 8);
  o[at + 1] = std::uint8_t(v);
}
void set32(Bytes &o, std::size_t at, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) o[at + std::size_t(i)] = std::uint8_t(v >> (24 - 8 * i));
}

// the key types' fields: value and tangent sizes in bytes (4 f32, 2 s16, 1 s8; 0 no tangents), 0 / 0 unknown
struct Format {
  int value, tangent;
};
Format formatOf(int type) {
  switch (type) {
    case 0: return {4, 4};
    case 1: return {4, 2};
    case 2: return {4, 1};
    case 4: return {2, 4};
    case 5: return {2, 2};
    case 6: return {2, 1};
    case 8: return {1, 4};
    case 9: return {1, 2};
    case 10: return {1, 1};
    case 15: return {4, 0};
    default: return {0, 0};
  }
}
float field(const std::uint8_t *p, int size) {
  if (size == 4) return f32(p);
  if (size == 2) return float(std::int16_t(u16(p))) * 0.0001f;
  return float(std::int8_t(*p)) * 0.0001f;
}

float hermite(float v0, float v1, float t0, float t1, float t) {
  const float t2 = t * t, t3 = t * t2;
  const float h01 = -(t3 + t3) + 3.0f * t2, h11 = t3 - t2, h10 = h11 - t2 + t, h00 = -h01 + 1.0f;
  return v0 * h00 + v1 * h01 + t0 * h10 + t1 * h11;
}

// d/dt of the Hermite piece
float hermiteSlope(float v0, float v1, float t0, float t1, float t) {
  const float t2 = t * t;
  return v0 * (6 * t2 - 6 * t) + v1 * (-6 * t2 + 6 * t) + t0 * (3 * t2 - 4 * t + 1) + t1 * (3 * t2 - 2 * t);
}

// the axis between frames f0 and f1, f0 becoming 0: each piece of the curve inside kept exactly (a piece cut at
// parameters a..b keeps its shape with its tangents times b - a: the game's tangents are per piece)
Axis cutAxis(const Axis &a, int f0, int f1) {
  Axis o;
  const std::size_t n = a.keys.size();
  if (!n) return o;
  auto push = [&](int frame, float value, float in, float out) {
    const auto f = std::uint16_t(frame - f0);
    if (!o.frames.empty() && o.frames.back() == f) {
      o.keys.back().out = out;  // the same frame: the next piece's tangent leaves it
      return;
    }
    o.frames.push_back(f);
    o.keys.push_back({value, in, out});
  };
  const float first = a.frames.front(), last = a.frames.back();
  if (n == 1 || float(f1) <= first || float(f0) >= last) {  // flat there
    const float v = float(f0) >= last ? a.keys.back().value : a.keys.front().value;
    push(f0, v, 0, 0);
    push(f1, v, 0, 0);
    return o;
  }
  if (float(f0) < first) push(f0, a.keys.front().value, 0, 0), push(int(first), a.keys.front().value, 0, 0);
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const float s0 = a.frames[i], s1 = a.frames[i + 1];
    const float lo = std::max(s0, float(f0)), hi = std::min(s1, float(f1));
    if (hi <= lo) continue;
    const float ta = (lo - s0) / (s1 - s0), tb = (hi - s0) / (s1 - s0), span = tb - ta;
    const Key &k0 = a.keys[i], &k1 = a.keys[i + 1];
    const float va = hermite(k0.value, k1.value, k0.out, k1.in, ta), vb = hermite(k0.value, k1.value, k0.out, k1.in, tb);
    const float da = hermiteSlope(k0.value, k1.value, k0.out, k1.in, ta) * span;
    const float db = hermiteSlope(k0.value, k1.value, k0.out, k1.in, tb) * span;
    push(int(std::lround(lo)), va, da, da);
    push(int(std::lround(hi)), vb, db, db);
  }
  if (float(f1) > last) push(int(last), a.keys.back().value, 0, 0), push(f1, a.keys.back().value, 0, 0);
  return o;
}

// how much of an edit applies at frame f: 1 from f0 to f1, easing to 0 over `fade` frames outside
float weight(int f, int f0, int f1, int fade) {
  if (f >= f0 && f <= f1) return 1.0f;
  const int away = f < f0 ? f0 - f : f - f1;
  if (fade <= 0 || away > fade) return 0.0f;
  const float x = 1.0f - float(away) / float(fade + 1);
  return x * x * (3.0f - 2.0f * x);
}

// one key a frame through the values, tangents from the neighbours (frames a unit apart: the game's tangents are
// per segment)
Axis dense(const std::vector<float> &v) {
  Axis a;
  const int n = int(v.size());
  for (int f = 0; f < n; ++f) {
    const float prev = v[std::size_t(std::max(f - 1, 0))], next = v[std::size_t(std::min(f + 1, n - 1))];
    const float tan = (f == 0 || f == n - 1) ? (next - prev) : (next - prev) * 0.5f;
    a.frames.push_back(std::uint16_t(f));
    a.keys.push_back({v[std::size_t(f)], tan, tan});
  }
  return a;
}

std::string clipName(int code) {
  static const char *const kClips[16] = {"walk start", "walk stop", "run start", "run stop", "run turn left",
                                         "run turn right", "turn left", "turn right", "sprint", "sprint start",
                                         "sprint stop", "sprint from the jog", "side step left", "side step right",
                                         "side step forward-left", "side step back-right"};
  static const char *const kMoves[4] = {"stand", "walk", "jog", "back step"};
  const char *levels[3] = {"1", "2", "3"};
  switch (code) {
    case 0x00: return "Stand";
    case 0x02: return "Walk";
    case 0x06: return "Jog";
    case 0x08: return "Back step";
    default: break;
  }
  if (code >= 0x80 && code < 0x90) return std::string("Move: ") + kClips[code - 0x80];
  if (code >= 0x90 && code <= 0x92) return std::string("Shotgun reload, level ") + levels[code - 0x90];
  if (code >= 0x94 && code <= 0x96) return "Shotgun aim " + std::to_string(code - 0x93);
  if (code >= 0x98 && code <= 0x9B) return std::string("Shotgun carry: ") + kMoves[code - 0x98];
  if (code == 0xA0) return "Pick up (on the move)";
  if (code == 0xA1) return "Pick up (low)";
  if (code == 0xA2) return "Pick up (table)";
  if (code >= 0xB0 && code <= 0xB2) return std::string("Handgun reload, level ") + levels[code - 0xB0];
  if (code >= 0xB4 && code <= 0xB6) return std::string("Handgun reload (Red9 set), level ") + levels[code - 0xB4];
  if (code >= 0xB8 && code <= 0xC6 && (code & 3) != 3) {
    static const char *const kPitch[3] = {"down", "level", "up"};
    const int c = code - 0xB8;
    return std::string((c / 4) & 1 ? "Handgun fire" : "Handgun aim") + ((c / 8) ? " (Red9 set), " : ", ") +
           kPitch[c & 3];
  }
  char hex[16];
  std::snprintf(hex, sizeof(hex), "Code 0x%02X", code);
  return hex;
}

// the MOV1 table's rows (entry 0 of move.dat): where each row starts
std::vector<std::size_t> movRows(const Bytes &e) {
  std::vector<std::size_t> out;
  if (e.size() < 8 || std::memcmp(e.data(), "MOV1", 4) != 0) return out;
  const std::uint32_t n = u32(e.data() + 4);
  for (std::uint32_t r = 0; r < n && 8 + (r + 1) * 12 <= e.size(); ++r) out.push_back(8 + r * 12);
  return out;
}

}  // namespace

int Joint::target() const {
  const int k = kind();
  if (k == 1 || k == 0x40) return 0;
  if (k & 2) return 1;
  if (k & 4) return 2;
  if (k & 8) return 0;
  if (k & 0x30) return 1;
  return 0;
}

bool ParseMotion(const Bytes &d, Motion &m, std::string *why) {
  auto fail = [&](const std::string &w) {
    if (why) *why = w;
    return false;
  };
  m = {};
  if (d.size() < 8) return fail("too short");
  m.rawMax = std::uint16_t(u16(d.data()));
  const unsigned n = d[2];
  if (!n) return fail("no bones in it");
  if (d.size() < 3 + 3 * n) return fail("cut short");
  std::size_t o = (3 + 3 * n + 3) & ~std::size_t(3);
  if (o + 4 + 4 * n > d.size()) return fail("cut short");
  o += 4;
  std::size_t end = o + 4 * n;
  m.joints.resize(n);
  std::vector<std::size_t> ofs(n);
  for (unsigned i = 0; i < n; ++i) {
    Joint &j = m.joints[i];
    j.info = std::uint16_t(u16(d.data() + 3 + 2 * i));
    j.part = d[3 + 2 * n + i];
    const Format f = formatOf(j.type());
    if (!f.value) return fail("unknown key type " + std::to_string(j.type()));
    const std::size_t ks = std::size_t(f.value + 2 * f.tangent);
    std::size_t p = ofs[i] = u32(d.data() + o + 4 * i);
    const std::size_t start = p;
    for (auto &a : j.axes) {
      if (p + 2 > d.size()) return fail("a key block runs past the end");
      const unsigned k = u16(d.data() + p);
      if (p + 2 + 2 * k + ks * k > d.size()) return fail("a key block runs past the end");
      for (unsigned q = 0; q < k; ++q) a.frames.push_back(std::uint16_t(u16(d.data() + p + 2 + 2 * q)));
      p += 2 + 2 * k;
      for (unsigned q = 0; q < k; ++q) {
        const std::uint8_t *e = d.data() + p + ks * q;
        Key key;
        key.value = field(e, f.value);
        if (f.tangent) {
          key.in = field(e + f.value, f.tangent);
          key.out = field(e + f.value + f.tangent, f.tangent);
        }
        a.keys.push_back(key);
      }
      p += ks * k;
    }
    j.raw.assign(d.begin() + std::ptrdiff_t(start), d.begin() + std::ptrdiff_t(p));
    end = std::max(end, p);
  }
  m.layout.resize(n);
  for (unsigned i = 0; i < n; ++i) m.layout[i] = int(i);
  std::stable_sort(m.layout.begin(), m.layout.end(), [&](int a, int b) { return ofs[std::size_t(a)] < ofs[std::size_t(b)]; });
  if (end < d.size()) m.fill = d[end];
  return true;
}

Bytes WriteMotion(const Motion &m) {
  const std::size_t n = m.joints.size();
  Bytes out;
  put16(out, m.rawMax);
  out.push_back(std::uint8_t(n));
  for (const auto &j : m.joints) put16(out, j.edited ? unsigned(j.info & 0x0FFF) : j.info);
  for (const auto &j : m.joints) out.push_back(j.part);
  while (out.size() & 3) out.push_back(0);
  const std::size_t sizeAt = out.size();
  put32(out, 0);
  const std::size_t ofsAt = out.size();
  for (std::size_t i = 0; i < n; ++i) put32(out, 0);
  for (int i : m.layout) {
    const Joint &j = m.joints[std::size_t(i)];
    if (!j.edited && j.type() == 15)
      while (out.size() & 3) out.push_back(0);
    set32(out, ofsAt + 4 * std::size_t(i), std::uint32_t(out.size()));
    if (!j.edited) {
      out.insert(out.end(), j.raw.begin(), j.raw.end());
      continue;
    }
    for (const auto &a : j.axes) {
      put16(out, unsigned(a.frames.size()));
      for (auto f : a.frames) put16(out, f);
      for (const auto &k : a.keys) {
        putF(out, k.value);
        putF(out, k.in);
        putF(out, k.out);
      }
    }
  }
  const std::size_t total = alignUp(out.size(), kAlign);
  set32(out, sizeAt, std::uint32_t(total));
  out.resize(total, m.fill);
  return out;
}

float Evaluate(const Axis &a, float frame, int maxFrame) {
  const std::size_t n = a.keys.size();
  if (!n) return 0.0f;
  if (float(maxFrame) <= frame || frame >= float(a.frames.back())) return a.keys.back().value;
  if (frame <= float(a.frames.front())) return a.keys.front().value;
  for (std::size_t i = 0; i + 1 < n; ++i) {
    const float f0 = a.frames[i], f1 = a.frames[i + 1];
    if (frame == f0) return a.keys[i].value;
    if (frame > f0 && frame < f1)
      return hermite(a.keys[i].value, a.keys[i + 1].value, a.keys[i].out, a.keys[i + 1].in, (frame - f0) / (f1 - f0));
  }
  return a.keys.back().value;
}

// ---- the editor
bool Editor::open(const Bytes &file, std::string *why) {
  auto fail = [&](const std::string &w) {
    if (why) *why = w;
    return false;
  };
  if (file.size() < 16) return fail("not a motion archive");
  const std::uint32_t n = u32(file.data());
  if (!n || n > 4096 || 16 + std::size_t(n) * 8 > file.size()) return fail("not a motion archive");
  std::vector<std::size_t> ofs(n + 1);
  for (std::uint32_t i = 0; i < n; ++i) {
    ofs[i] = u32(file.data() + 16 + 4 * i);
    if (ofs[i] > file.size() || (i && ofs[i] < ofs[i - 1])) return fail("not a motion archive (its offsets)");
  }
  ofs[n] = file.size();
  head_.assign(file.begin() + 4, file.begin() + 16);
  entries_.assign(n, {});
  types_.assign(n, {});
  motions_.assign(n, {});
  isMotion_.assign(n, false);
  changed_.assign(n, false);
  undo_.clear();
  edited_ = false;
  int found = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    std::memcpy(types_[i].data(), file.data() + 16 + 4 * n + 4 * i, 4);
    entries_[i].assign(file.begin() + std::ptrdiff_t(ofs[i]), file.begin() + std::ptrdiff_t(ofs[i + 1]));
    if (std::memcmp(types_[i].data(), "FCV", 3) != 0 || entries_[i].size() < 8) continue;
    if (!(u16(entries_[i].data()) & 0x3FFF)) continue;  // an empty place (no frames)
    if (ParseMotion(entries_[i], motions_[i], nullptr)) {
      isMotion_[i] = true;
      ++found;
    }
  }
  if (!found) {
    entries_.clear();
    return fail("no animations in it");
  }
  return true;
}

Bytes Editor::save() const {
  const std::size_t n = entries_.size();
  Bytes out(alignUp(16 + n * 8, kAlign), 0);
  set32(out, 0, std::uint32_t(n));
  std::copy(head_.begin(), head_.end(), out.begin() + 4);
  for (std::size_t i = 0; i < n; ++i) {
    std::copy(types_[i].begin(), types_[i].end(), out.begin() + std::ptrdiff_t(16 + n * 4 + i * 4));
    set32(out, 16 + i * 4, std::uint32_t(out.size()));
    const Bytes blob = changed_[i] ? WriteMotion(motions_[i]) : entries_[i];
    out.insert(out.end(), blob.begin(), blob.end());
    if (i + 1 < n) out.resize(alignUp(out.size(), kAlign), 0);
  }
  return out;
}

std::vector<Editor::Clip> Editor::clips() const {
  std::vector<int> codes(entries_.size(), -1);
  if (!entries_.empty())
    for (auto r : movRows(entries_[0])) {
      const unsigned e = u16(entries_[0].data() + r + 2);
      if (e < codes.size() && codes[e] < 0) codes[e] = entries_[0][r];
    }
  std::vector<Clip> out;
  for (unsigned i = 0; i < entries_.size(); ++i) {
    if (!isMotion_[i]) continue;
    Clip c;
    c.entry = i;
    c.code = codes[i];
    c.frames = motions_[i].maxFrame();
    c.name = c.code >= 0 ? clipName(c.code) : "Animation #" + std::to_string(i);
    out.push_back(c);
  }
  return out;
}

Motion *Editor::motion(unsigned entry) { return entry < motions_.size() && isMotion_[entry] ? &motions_[entry] : nullptr; }
const Motion *Editor::motion(unsigned entry) const {
  return entry < motions_.size() && isMotion_[entry] ? &motions_[entry] : nullptr;
}

std::vector<int> Editor::parts(unsigned entry, bool pos) const {
  std::vector<int> out;
  if (const Motion *m = motion(entry))
    for (const auto &j : m->joints)
      if (j.target() == (pos ? 2 : 1) && j.type() != 15) out.push_back(j.part);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

void Editor::remember(std::vector<unsigned> which) {
  Saved s;
  for (unsigned e : which)
    if (const Motion *m = motion(e)) s.motions.push_back({e, *m});
  if (!entries_.empty()) s.table = entries_[0];
  undo_.push_back(std::move(s));
  if (undo_.size() > 40) undo_.erase(undo_.begin());
}

bool Editor::undo() {
  if (undo_.empty()) return false;
  Saved s = std::move(undo_.back());
  undo_.pop_back();
  for (auto &p : s.motions) {
    motions_[p.first] = std::move(p.second);
    changed_[p.first] = true;
  }
  if (!entries_.empty()) entries_[0] = s.table;
  edited_ = true;
  return true;
}

bool Editor::adjust(unsigned entry, int part, bool pos, const float d[3], int f0, int f1, int fade, std::string *why) {
  Motion *m = motion(entry);
  if (!m) {
    if (why) *why = "no such animation";
    return false;
  }
  Joint *j = nullptr;
  for (auto &k : m->joints)
    if (k.part == part && k.target() == (pos ? 2 : 1) && k.type() != 15) j = &k;
  if (!j) {
    if (why) *why = pos ? "this bone does not move in this animation" : "this bone does not turn in this animation";
    return false;
  }
  if (!d[0] && !d[1] && !d[2]) {
    if (why) *why = "nothing to change (all three at 0)";
    return false;
  }
  const int last = m->maxFrame();
  f0 = std::clamp(f0, 0, last);
  f1 = std::clamp(f1, 0, last);
  if (f1 < f0) std::swap(f0, f1);
  remember({entry});
  for (int a = 0; a < 3; ++a) {
    if (!d[a]) continue;
    std::vector<float> v(std::size_t(last) + 1);
    for (int f = 0; f <= last; ++f)
      v[std::size_t(f)] = Evaluate(j->axes[std::size_t(a)], float(f), last) + d[a] * weight(f, f0, f1, fade);
    j->axes[std::size_t(a)] = dense(v);
  }
  j->edited = true;
  changed_[entry] = true;
  edited_ = true;
  return true;
}

bool Editor::retimeOne(unsigned entry, float speed) {
  Motion *m = motion(entry);
  if (!m) return false;
  const int last = m->maxFrame();
  const int now = std::clamp(int(std::lround(float(last) / speed)), 1, 0x3FFF);
  for (auto &j : m->joints) {
    if (j.type() == 15) continue;
    for (auto &a : j.axes) {
      Axis b;
      for (std::size_t k = 0; k < a.frames.size(); ++k) {
        const int f = int(std::lround(double(a.frames[k]) * now / last));
        if (!b.frames.empty() && f <= b.frames.back()) continue;  // two keys on one frame: the first stays
        b.frames.push_back(std::uint16_t(f));
        b.keys.push_back(a.keys[k]);
      }
      a = std::move(b);
    }
    j.edited = true;
  }
  m->rawMax = std::uint16_t((m->rawMax & 0xC000) | now);
  changed_[entry] = true;
  if (!entries_.empty())
    for (auto r : movRows(entries_[0])) {
      Bytes &t = entries_[0];
      if (u16(t.data() + r + 2) != entry) continue;
      set16(t, r + 4, unsigned(now));
      for (std::size_t at : {r + 6, r + 8}) {
        const unsigned f = u16(t.data() + at);
        if (f != 0xFFFF) set16(t, at, unsigned(std::lround(double(f) * now / last)));
      }
    }
  return true;
}

int Editor::codeOf(unsigned entry) const {
  for (const auto &c : clips())
    if (c.entry == entry) return c.code;
  return -1;
}

std::vector<unsigned> Editor::withLayers(unsigned entry) const {
  std::vector<unsigned> which{entry};
  const int code = codeOf(entry);
  const int moves[4] = {0x00, 0x02, 0x06, 0x08};
  for (int k = 0; k < 4; ++k)
    if (code == moves[k])
      for (const auto &c : clips())
        if (c.code == 0x98 + k) which.push_back(c.entry);
  return which;
}

bool Editor::trimOne(unsigned entry, int f0, int f1) {
  Motion *m = motion(entry);
  if (!m) return false;
  const int last = m->maxFrame();
  for (auto &j : m->joints) {
    if (j.type() == 15) continue;
    for (auto &a : j.axes) a = cutAxis(a, f0, f1);
    j.edited = true;
  }
  const int now = f1 - f0;
  m->rawMax = std::uint16_t((m->rawMax & 0xC000) | now);
  changed_[entry] = true;
  if (!entries_.empty())
    for (auto r : movRows(entries_[0])) {
      Bytes &t = entries_[0];
      if (u16(t.data() + r + 2) != entry) continue;
      set16(t, r + 4, unsigned(now));
      for (std::size_t at : {r + 6, r + 8}) {
        const unsigned f = u16(t.data() + at);
        if (f != 0xFFFF) set16(t, at, unsigned(std::clamp(int(f) - f0, 0, now)));
      }
    }
  (void) last;
  return true;
}

bool Editor::trim(unsigned entry, int f0, int f1, std::string *why) {
  const Motion *m = motion(entry);
  if (!m) {
    if (why) *why = "no such animation";
    return false;
  }
  f0 = std::clamp(f0, 0, m->maxFrame());
  f1 = std::clamp(f1, 0, m->maxFrame());
  if (f1 < f0) std::swap(f0, f1);
  if (f1 - f0 < 2) {
    if (why) *why = "keep at least 2 frames";
    return false;
  }
  if (f0 == 0 && f1 == m->maxFrame()) {
    if (why) *why = "nothing to cut (the whole animation is marked)";
    return false;
  }
  const int code = codeOf(entry);
  if ((code >= 0x90 && code <= 0x92) || (code >= 0xB0 && code <= 0xC6)) {
    if (why) *why = "the game matches the weapon animations by their length: cutting one would stop it playing";
    return false;
  }
  const auto which = withLayers(entry);
  remember(which);
  for (unsigned e : which) trimOne(e, f0, f1);
  edited_ = true;
  return true;
}

bool Editor::retime(unsigned entry, float speed, std::string *why) {
  if (!motion(entry) || !(speed > 0.1f && speed < 10.0f)) {
    if (why) *why = !motion(entry) ? "no such animation" : "the speed is out of range";
    return false;
  }
  int code = -1;
  for (const auto &c : clips())
    if (c.entry == entry) code = c.code;
  // the game picks these by their length (re4_move_reload): another length would not play
  if ((code >= 0x90 && code <= 0x92) || (code >= 0xB0 && code <= 0xC6)) {
    if (why) *why = "the game matches the weapon animations by their length: their speed cannot change here";
    return false;
  }
  std::vector<unsigned> which{entry};
  // the shotgun's carry layers have the frames of their move (locomotion.cpp gCarry): they change with it
  const int moves[4] = {0x00, 0x02, 0x06, 0x08};
  for (int k = 0; k < 4; ++k)
    if (code == moves[k])
      for (const auto &c : clips())
        if (c.code == 0x98 + k) which.push_back(c.entry);
  remember(which);
  for (unsigned e : which) retimeOne(e, speed);
  edited_ = true;
  return true;
}

// ---- the pose and IK
namespace {

Matrix rotation(float x, float y, float z) {
  // RotMatrix (ports/motion64/math.cpp): R(z) R(y) R(x)
  const float sx = std::sin(x), sy = std::sin(y), sz = std::sin(z), cx = std::cos(x), cy = std::cos(y), cz = std::cos(z);
  return {cz * cy, cz * sx * sy - sz * cx, cz * cx * sy + sz * sx, 0,
          sz * cy, sz * sx * sy + cz * cx, sz * cx * sy - cz * sx, 0,
          -sy,     cy * sx,                cy * cx,                0};
}

Matrix multiply(const Matrix &a, const Matrix &b) {
  Matrix o{};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) {
      float v = c == 3 ? a[std::size_t(r * 4 + 3)] : 0.0f;
      for (int k = 0; k < 3; ++k) v += a[std::size_t(r * 4 + k)] * b[std::size_t(k * 4 + c)];
      o[std::size_t(r * 4 + c)] = v;
    }
  }
  return o;
}

// the angles RotMatrix makes `m` from (its rotation part)
std::array<float, 3> anglesOf(const Matrix &m) {
  const float sy = std::clamp(-m[8], -1.0f, 1.0f);
  return {std::atan2(m[9], m[10]), std::asin(sy), std::atan2(m[4], m[0])};
}

// the angle nearest `near` that turns as `v` does
float unwrap(float v, float near) {
  const float tau = 6.28318530718f;
  while (v - near > tau / 2) v -= tau;
  while (v - near < -tau / 2) v += tau;
  return v;
}

// the transpose of a rotation (its inverse), translation dropped
Matrix inverseRotation(const Matrix &m) {
  return {m[0], m[4], m[8], 0, m[1], m[5], m[9], 0, m[2], m[6], m[10], 0};
}

struct Frame {
  std::vector<std::array<float, 3>> angle, pos;
};

const Joint *track(const Motion &m, int part, int target) {
  for (const auto &j : m.joints)
    if (j.part == part && j.target() == target && j.type() != 15) return &j;
  return nullptr;
}

Frame frameOf(const Motion &m, const Skeleton &s, float frame) {
  Frame f;
  const std::size_t n = s.parent.size();
  f.angle.assign(n, {0, 0, 0});
  f.pos = s.bind;
  for (std::size_t p = 0; p < n; ++p) {
    if (const Joint *j = track(m, int(p), 1))
      for (int a = 0; a < 3; ++a) f.angle[p][std::size_t(a)] = Evaluate(j->axes[std::size_t(a)], frame, m.maxFrame());
    if (const Joint *j = track(m, int(p), 2))
      for (int a = 0; a < 3; ++a) f.pos[p][std::size_t(a)] = Evaluate(j->axes[std::size_t(a)], frame, m.maxFrame());
  }
  return f;
}

void worldOf(const Frame &f, const Skeleton &s, std::vector<Matrix> &out) {
  const std::size_t n = s.parent.size();
  out.assign(n, {});
  std::vector<int> done(n, 0);
  for (std::size_t start = 0; start < n; ++start) {
    // the chain up to a done (or root) part, then down again
    std::vector<std::size_t> chain;
    for (std::size_t p = start; p < n && !done[p]; p = std::size_t(s.parent[p])) {
      chain.push_back(p);
      if (s.parent[p] < 0 || std::size_t(s.parent[p]) >= n) break;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
      const std::size_t p = *it;
      Matrix local = rotation(f.angle[p][0], f.angle[p][1], f.angle[p][2]);
      local[3] = f.pos[p][0];
      local[7] = f.pos[p][1];
      local[11] = f.pos[p][2];
      const int up = s.parent[p];
      out[p] = up >= 0 && std::size_t(up) < n && done[std::size_t(up)] ? multiply(out[std::size_t(up)], local) : local;
      done[p] = 1;
    }
  }
}

// the chain c (upper, lower, end) turned so that the end reaches `target` (model space), the end keeping its turn in
// the world; returns how far it stays from it (mm)
float solveChain(Frame &fr, const Skeleton &s, const int *c, const float target[3]) {
  std::vector<Matrix> world;
  worldOf(fr, s, world);
  const Matrix endWorld = world[std::size_t(c[2])];
  float theta[6], theta0[6];
  for (int k = 0; k < 2; ++k)
    for (int a = 0; a < 3; ++a) theta[k * 3 + a] = theta0[k * 3 + a] = fr.angle[std::size_t(c[k])][std::size_t(a)];
  auto endAt = [&](const float *t, float out[3]) {
    Frame g = fr;
    for (int k = 0; k < 2; ++k)
      for (int a = 0; a < 3; ++a) g.angle[std::size_t(c[k])][std::size_t(a)] = t[k * 3 + a];
    std::vector<Matrix> wd;
    worldOf(g, s, wd);
    const Matrix &e = wd[std::size_t(c[2])];
    out[0] = e[3], out[1] = e[7], out[2] = e[11];
  };
  // damped least squares: the end on the target, the bones as little away from the animation as it takes
  const float keep = 40.0f;  // mm^2 a radian^2 away from the animation's angles
  for (int it = 0; it < 40; ++it) {
    float p[3];
    endAt(theta, p);
    const float e[3] = {target[0] - p[0], target[1] - p[1], target[2] - p[2]};
    float J[3][6];
    for (int k = 0; k < 6; ++k) {
      float t2[6];
      std::copy(theta, theta + 6, t2);
      t2[k] += 1e-3f;
      float q[3];
      endAt(t2, q);
      for (int r = 0; r < 3; ++r) J[r][k] = (q[r] - p[r]) / 1e-3f;
    }
    double A[6][7] = {};
    for (int i = 0; i < 6; ++i) {
      for (int j = 0; j < 6; ++j) {
        double v = 0;
        for (int r = 0; r < 3; ++r) v += double(J[r][i]) * J[r][j];
        A[i][j] = v + (i == j ? keep + 10.0 : 0.0);
      }
      double bb = -keep * double(theta[i] - theta0[i]);
      for (int r = 0; r < 3; ++r) bb += double(J[r][i]) * e[r];
      A[i][6] = bb;
    }
    for (int i = 0; i < 6; ++i) {  // Gauss-Jordan (A is positive definite)
      const double piv = A[i][i];
      for (int j = i; j < 7; ++j) A[i][j] /= piv;
      for (int r = 0; r < 6; ++r) {
        if (r == i) continue;
        const double f2 = A[r][i];
        for (int j = i; j < 7; ++j) A[r][j] -= f2 * A[i][j];
      }
    }
    float step = 0;
    for (int i = 0; i < 6; ++i) {
      const float dx = std::clamp(float(A[i][6]), -0.3f, 0.3f);
      theta[i] += dx;
      step = std::max(step, std::fabs(dx));
    }
    if (step < 1e-5f) break;
  }
  float p[3];
  endAt(theta, p);
  const float miss = std::sqrt((p[0] - target[0]) * (p[0] - target[0]) + (p[1] - target[1]) * (p[1] - target[1]) +
                               (p[2] - target[2]) * (p[2] - target[2]));
  Frame g = fr;
  for (int k = 0; k < 2; ++k)
    for (int a = 0; a < 3; ++a) g.angle[std::size_t(c[k])][std::size_t(a)] = theta[k * 3 + a];
  std::vector<Matrix> wd;
  worldOf(g, s, wd);
  const int up = s.parent[std::size_t(c[2])];
  const Matrix local = multiply(inverseRotation(wd[std::size_t(up)]), endWorld);
  const auto ang = anglesOf(local);
  for (int a = 0; a < 3; ++a) {
    g.angle[std::size_t(c[2])][std::size_t(a)] = unwrap(ang[std::size_t(a)], fr.angle[std::size_t(c[2])][std::size_t(a)]);
    for (int k = 0; k < 2; ++k)
      g.angle[std::size_t(c[k])][std::size_t(a)] = unwrap(g.angle[std::size_t(c[k])][std::size_t(a)],
                                                           fr.angle[std::size_t(c[k])][std::size_t(a)]);
  }
  fr = g;
  return miss;
}

const int kChains[4][3] = {{7, 8, 10}, {13, 14, 16}, {18, 19, 20}, {22, 23, 24}};  // upper, lower, end

}  // namespace

bool Editor::pose(unsigned entry, float frame, const Skeleton &s, std::vector<Matrix> &out) const {
  const Motion *m = motion(entry);
  if (!m || s.parent.empty()) return false;
  worldOf(frameOf(*m, s, frame), s, out);
  return true;
}

bool Editor::posePreview(unsigned entry, float frame, const Skeleton &s, int mode, int part, const float d[3],
                         std::vector<Matrix> &before, std::vector<Matrix> &after) const {
  const Motion *m = motion(entry);
  if (!m || s.parent.empty() || part < 0 || std::size_t(part) >= s.parent.size()) return false;
  Frame fr = frameOf(*m, s, frame);
  worldOf(fr, s, before);
  if (mode == 2) {  // turn: the part's X, Y, Z angles
    for (int a = 0; a < 3; ++a) fr.angle[std::size_t(part)][std::size_t(a)] += d[a];
  } else if (part == 0) {  // move the root: the body's place
    for (int a = 0; a < 3; ++a) fr.pos[0][std::size_t(a)] += d[a];
  } else {
    int end = -1;
    for (int k = 0; k < 4; ++k)
      if (kChains[k][2] == part) end = k;
    if (end < 0) return false;
    const Matrix &e = before[std::size_t(part)];
    const float target[3] = {e[3] + d[0], e[7] + d[1], e[11] + d[2]};
    solveChain(fr, s, kChains[end], target);
  }
  worldOf(fr, s, after);
  return true;
}

Bytes Editor::clipBytes(unsigned entry) const {
  if (entry >= entries_.size()) return {};
  return changed_[entry] && isMotion_[entry] ? WriteMotion(motions_[entry]) : entries_[entry];
}

bool Editor::turnAxes(unsigned entry, float frame, const Skeleton &s, int part,
                      std::array<std::array<float, 3>, 3> &out) const {
  const Motion *m = motion(entry);
  if (!m || part < 0 || std::size_t(part) >= s.parent.size()) return false;
  const Frame f = frameOf(*m, s, frame);
  std::vector<Matrix> world;
  worldOf(f, s, world);
  const int up = s.parent[std::size_t(part)];
  Matrix parent = rotation(0, 0, 0);
  if (up >= 0 && std::size_t(up) < world.size()) parent = world[std::size_t(up)];
  const auto &a = f.angle[std::size_t(part)];
  const Matrix pz = multiply(parent, rotation(0, 0, a[2]));
  const Matrix own = world[std::size_t(part)];
  out[0] = {own[0], own[4], own[8]};   // X: the part's own x
  out[1] = {pz[1], pz[5], pz[9]};      // Y: y after the parent and Z
  out[2] = {parent[2], parent[6], parent[10]};  // Z: the parent's z
  return true;
}

bool Editor::reach(unsigned entry, const Skeleton &s, int end, const float d[3], int f0, int f1, int fade,
                   std::string *why) {
  auto fail = [&](const std::string &w) {
    if (why) *why = w;
    return false;
  };
  Motion *m = motion(entry);
  if (!m) return fail("no such animation");
  if (end < 0 || end > 3) return fail("no such hand or foot");
  if (!d[0] && !d[1] && !d[2]) return fail("nothing to change (all three at 0)");
  const int *c = kChains[end];
  const std::size_t n = s.parent.size();
  for (int k = 0; k < 3; ++k) {
    if (std::size_t(c[k]) >= n) return fail("this model has no such bone (" + std::to_string(c[k]) + ")");
    if (!track(*m, c[k], 1)) return fail("bone " + std::to_string(c[k]) + " does not turn in this animation");
  }
  // the end below the lower bone, the lower below the upper
  auto below = [&](int child, int up) {
    for (int p = child; p >= 0 && std::size_t(p) < n; p = s.parent[std::size_t(p)])
      if (p == up) return true;
    return false;
  };
  if (!below(c[2], c[1]) || !below(c[1], c[0])) return fail("the bones are not one chain on this model");
  const int last = m->maxFrame();
  f0 = std::clamp(f0, 0, last);
  f1 = std::clamp(f1, 0, last);
  if (f1 < f0) std::swap(f0, f1);
  // per bone of the chain, per axis: its value at each frame
  std::vector<std::array<std::vector<float>, 3>> values(3);
  float worst = 0;
  for (int f = 0; f <= last; ++f) {
    Frame fr = frameOf(*m, s, float(f));
    const float w = weight(f, f0, f1, fade);
    if (w > 0) {
      std::vector<Matrix> world;
      worldOf(fr, s, world);
      const Matrix &endWorld = world[std::size_t(c[2])];
      const float target[3] = {endWorld[3] + d[0] * w, endWorld[7] + d[1] * w, endWorld[11] + d[2] * w};
      worst = std::max(worst, solveChain(fr, s, c, target));
    }
    for (int k = 0; k < 3; ++k)
      for (int a = 0; a < 3; ++a) values[std::size_t(k)][std::size_t(a)].push_back(fr.angle[std::size_t(c[k])][std::size_t(a)]);
  }
  remember({entry});
  for (int k = 0; k < 3; ++k) {
    Joint *j = nullptr;
    for (auto &jj : m->joints)
      if (jj.part == c[k] && jj.target() == 1 && jj.type() != 15) j = &jj;
    for (int a = 0; a < 3; ++a) j->axes[std::size_t(a)] = dense(values[std::size_t(k)][std::size_t(a)]);
    j->edited = true;
  }
  changed_[entry] = true;
  edited_ = true;
  lastReachMiss_ = worst;
  return true;
}

}  // namespace re4::studio::motionedit
