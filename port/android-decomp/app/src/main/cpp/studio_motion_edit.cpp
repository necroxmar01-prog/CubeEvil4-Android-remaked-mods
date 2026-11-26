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

bool Editor::retime(unsigned entry, float speed, std::string *why) {
  if (!motion(entry) || !(speed > 0.1f && speed < 10.0f)) {
    if (why) *why = !motion(entry) ? "no such animation" : "the speed is out of range";
    return false;
  }
  int code = -1;
  for (const auto &c : clips())
    if (c.entry == entry) code = c.code;
  // the game picks these by their length (re4_move_reload): another length would not play
  if ((code >= 0x90 && code <= 0x92) || (code >= 0xB0 && code <= 0xB6)) {
    if (why) *why = "the game matches the reloads by their length: change their speed in the game (later)";
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

}  // namespace re4::studio::motionedit
