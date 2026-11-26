// CubeEvil4 Studio: rigged models as .fbx (DevJunior, 25.11.2026: "the GTA SA skins deform a lot, help me with these
// models so they work with the tool": an Ashley of the RE4 Remake as a binary FBX 7.4 with her textures as .png and
// .tga beside it). Read here into the GTA skins' terms (DffMesh): the skinned meshes in the bind pose, their bones
// named as a GTA ped's where the rig has them (by their names: Blender's, Mixamo's, 3ds Max's Biped, Unreal's), every
// other bone's weights on the nearest of those above it, the colour textures by the materials. skinOnLeon then puts it
// on the character as a GTA skin.
//
// Included by studio_convert.cpp (namespace of its helpers).

#include <unordered_map>

// zlib's uncompress (libz; its header is not included: the NDK's zconf.h defines `far` and `near` away, words used in
// studio_convert.cpp)
extern "C" int uncompress(unsigned char *dest, unsigned long *destLen, const unsigned char *source, unsigned long sourceLen);

struct FbxProp {
  char type = 0;
  std::int64_t i = 0;
  double d = 0;
  std::string s;               // S, R
  std::vector<double> ad;      // f, d arrays
  std::vector<std::int64_t> ai;  // i, l, b arrays
};

struct FbxNode {
  std::string name;
  std::vector<FbxProp> props;
  std::vector<FbxNode> kids;
  const FbxNode *first(const char *n) const {
    for (const auto &k : kids)
      if (k.name == n) return &k;
    return nullptr;
  }
};

Bytes inflateZ(const std::uint8_t *src, std::size_t n, std::size_t want) {
  Bytes out(want);
  unsigned long got = static_cast<unsigned long>(want);
  if (want == 0) return out;
  if (uncompress(out.data(), &got, src, static_cast<unsigned long>(n)) != 0 || got != want) fail("a packed block does not unpack");
  return out;
}

class FbxReader {
 public:
  explicit FbxReader(const Bytes &b) : b_(b) {
    if (b.size() < 27 || std::memcmp(b.data(), "Kaydara FBX Binary", 18) != 0)
      fail("not a binary FBX (export it as binary FBX, version 7.x)");
    wide_ = le32(b, 23) >= 7500;
    std::size_t o = 27;
    while (o < b.size()) {
      FbxNode n;
      if (!node(o, n)) break;
      top.push_back(std::move(n));
    }
  }
  std::vector<FbxNode> top;

 private:
  const Bytes &b_;
  bool wide_ = false;
  std::uint64_t u(std::size_t o, int bytes) const {
    if (o + std::size_t(bytes) > b_.size()) fail("the FBX is cut short");
    std::uint64_t v = 0;
    for (int k = bytes - 1; k >= 0; --k) v = (v << 8) | b_[o + std::size_t(k)];
    return v;
  }
  bool node(std::size_t &o, FbxNode &n) {
    const std::uint64_t end = u(o, wide_ ? 8 : 4), np = u(o + (wide_ ? 8 : 4), wide_ ? 8 : 4);
    o += wide_ ? 24 : 12;
    const std::size_t nl = b_.at(o);
    o += 1;
    if (end == 0) return false;
    if (end > b_.size()) fail("the FBX is cut short");
    n.name.assign(reinterpret_cast<const char *>(b_.data() + o), nl);
    o += nl;
    for (std::uint64_t k = 0; k < np; ++k) n.props.push_back(prop(o));
    while (o < end) {
      FbxNode c;
      if (!node(o, c)) break;
      n.kids.push_back(std::move(c));
    }
    o = std::size_t(end);
    return true;
  }
  FbxProp prop(std::size_t &o) {
    FbxProp p;
    p.type = char(b_.at(o));
    o += 1;
    auto f32 = [&](std::size_t q) {
      const std::uint32_t v = std::uint32_t(u(q, 4));
      float f;
      std::memcpy(&f, &v, 4);
      return double(f);
    };
    auto f64 = [&](std::size_t q) {
      const std::uint64_t v = u(q, 8);
      double f;
      std::memcpy(&f, &v, 8);
      return f;
    };
    switch (p.type) {
      case 'Y': p.i = std::int16_t(u(o, 2)), o += 2; break;
      case 'C': p.i = b_.at(o) != 0, o += 1; break;
      case 'I': p.i = std::int32_t(u(o, 4)), o += 4; break;
      case 'L': p.i = std::int64_t(u(o, 8)), o += 8; break;
      case 'F': p.d = f32(o), o += 4; break;
      case 'D': p.d = f64(o), o += 8; break;
      case 'S':
      case 'R': {
        const std::size_t n = std::size_t(u(o, 4));
        if (o + 4 + n > b_.size()) fail("the FBX is cut short");
        p.s.assign(reinterpret_cast<const char *>(b_.data() + o + 4), n);
        o += 4 + n;
        break;
      }
      case 'f': case 'd': case 'i': case 'l': case 'b': {
        const std::size_t n = std::size_t(u(o, 4)), enc = std::size_t(u(o + 4, 4)), clen = std::size_t(u(o + 8, 4));
        const std::size_t es = p.type == 'd' || p.type == 'l' ? 8 : p.type == 'b' ? 1 : 4;
        if (o + 12 + clen > b_.size()) fail("the FBX is cut short");
        Bytes raw = enc ? inflateZ(b_.data() + o + 12, clen, n * es)
                        : Bytes(b_.begin() + std::ptrdiff_t(o + 12), b_.begin() + std::ptrdiff_t(o + 12 + n * es));
        auto rd = [&](std::size_t q, int bytes) {
          std::uint64_t v = 0;
          for (int k = bytes - 1; k >= 0; --k) v = (v << 8) | raw[q + std::size_t(k)];
          return v;
        };
        if (p.type == 'f' || p.type == 'd') {
          p.ad.resize(n);
          for (std::size_t k = 0; k < n; ++k) {
            if (p.type == 'd') {
              const std::uint64_t v = rd(k * 8, 8);
              std::memcpy(&p.ad[k], &v, 8);
            } else {
              const std::uint32_t v = std::uint32_t(rd(k * 4, 4));
              float f;
              std::memcpy(&f, &v, 4);
              p.ad[k] = f;
            }
          }
        } else {
          p.ai.resize(n);
          for (std::size_t k = 0; k < n; ++k)
            p.ai[k] = p.type == 'l' ? std::int64_t(rd(k * 8, 8)) : p.type == 'i' ? std::int32_t(rd(k * 4, 4)) : raw[k];
        }
        o += 12 + clen;
        break;
      }
      default:
        fail(fmt("FBX: a property of the unknown type '%c'", p.type));
    }
    return p;
  }
};

// ---- pictures: .png and .tga ---------------------------------------------------------------------------------------

Rgba readPng(const Bytes &b) {
  static const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  if (b.size() < 33 || std::memcmp(b.data(), sig, 8) != 0) fail("not a .png");
  auto be = [&](std::size_t o) { return (std::uint32_t(b.at(o)) << 24) | (std::uint32_t(b.at(o + 1)) << 16) |
                                        (std::uint32_t(b.at(o + 2)) << 8) | b.at(o + 3); };
  std::uint32_t w = 0, h = 0;
  int depth = 0, colour = 0, interlace = 0;
  Bytes idat, plte, trns;
  for (std::size_t o = 8; o + 12 <= b.size();) {
    const std::uint32_t n = be(o);
    const std::string type(reinterpret_cast<const char *>(b.data() + o + 4), 4);
    if (o + 12 + n > b.size()) fail(".png cut short");
    const std::uint8_t *d = b.data() + o + 8;
    if (type == "IHDR") w = be(o + 8), h = be(o + 12), depth = d[8], colour = d[9], interlace = d[12];
    else if (type == "PLTE") plte.assign(d, d + n);
    else if (type == "tRNS") trns.assign(d, d + n);
    else if (type == "IDAT") idat.insert(idat.end(), d, d + n);
    else if (type == "IEND") break;
    o += 12 + n;
  }
  if (!w || !h || w > 16384 || h > 16384) fail(".png without a sensible size");
  if (interlace) fail("an interlaced .png (save it without interlacing)");
  if (depth != 8 && depth != 16) fail(fmt("a .png with %d bits per channel", depth));
  const int ch = colour == 0 ? 1 : colour == 2 ? 3 : colour == 3 ? 1 : colour == 4 ? 2 : colour == 6 ? 4 : 0;
  if (!ch) fail(".png of an unknown colour type");
  const std::size_t bpp = std::size_t(ch) * std::size_t(depth / 8), stride = std::size_t(w) * bpp;
  Bytes raw = inflateZ(idat.data(), idat.size(), (stride + 1) * h);
  Bytes cur(stride), prev(stride, 0);
  Rgba img;
  img.w = int(w), img.h = int(h);
  img.px.resize(std::size_t(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    const std::uint8_t f = raw[y * (stride + 1)];
    const std::uint8_t *s = raw.data() + y * (stride + 1) + 1;
    for (std::size_t x = 0; x < stride; ++x) {
      const int a = x >= bpp ? cur[x - bpp] : 0, up = prev[x], c = x >= bpp ? prev[x - bpp] : 0;
      int v = s[x];
      switch (f) {
        case 1: v += a; break;
        case 2: v += up; break;
        case 3: v += (a + up) / 2; break;
        case 4: {
          const int p = a + up - c, pa = std::abs(p - a), pb = std::abs(p - up), pc = std::abs(p - c);
          v += (pa <= pb && pa <= pc) ? a : pb <= pc ? up : c;
          break;
        }
        default: break;
      }
      cur[x] = std::uint8_t(v);
    }
    for (std::uint32_t x = 0; x < w; ++x) {
      auto at = [&](int k) { return cur[x * bpp + std::size_t(k * (depth / 8))]; };  // the high byte at 16 bits
      std::uint8_t *o = &img.px[(std::size_t(y) * w + x) * 4];
      if (colour == 3) {
        const std::size_t i = at(0);
        o[0] = i * 3 + 2 < plte.size() ? plte[i * 3] : 0, o[1] = i * 3 + 2 < plte.size() ? plte[i * 3 + 1] : 0;
        o[2] = i * 3 + 2 < plte.size() ? plte[i * 3 + 2] : 0, o[3] = i < trns.size() ? trns[i] : 255;
      } else if (ch <= 2) {
        o[0] = o[1] = o[2] = at(0), o[3] = ch == 2 ? at(1) : 255;
      } else {
        o[0] = at(0), o[1] = at(1), o[2] = at(2), o[3] = ch == 4 ? at(3) : 255;
      }
    }
    std::swap(cur, prev);
  }
  return img;
}

Rgba readTga(const Bytes &b) {
  if (b.size() < 18) fail("not a .tga");
  const int idLen = b[0], cmap = b[1], type = b[2], w = b[12] | (b[13] << 8), h = b[14] | (b[15] << 8), bits = b[16];
  const bool top = (b[17] & 0x20) != 0;
  if (cmap || (type != 2 && type != 10 && type != 3 && type != 11)) fail(".tga with a palette (save it as true colour)");
  const int bpp = bits / 8;
  if (w <= 0 || h <= 0 || (bpp != 1 && bpp != 3 && bpp != 4)) fail(fmt(".tga with %d bits per pixel", bits));
  std::size_t o = 18 + std::size_t(idLen) + std::size_t(b[5] | (b[6] << 8)) * std::size_t((b[7] + 7) / 8);
  Rgba img;
  img.w = w, img.h = h;
  img.px.resize(std::size_t(w) * std::size_t(h) * 4);
  const std::size_t n = std::size_t(w) * std::size_t(h);
  std::size_t i = 0;
  auto put = [&](const std::uint8_t *p) {
    const std::size_t x = i % std::size_t(w), y = i / std::size_t(w), row = top ? y : std::size_t(h) - 1 - y;
    std::uint8_t *q = &img.px[(row * std::size_t(w) + x) * 4];
    if (bpp == 1) q[0] = q[1] = q[2] = p[0], q[3] = 255;
    else q[0] = p[2], q[1] = p[1], q[2] = p[0], q[3] = bpp == 4 ? p[3] : 255;
    ++i;
  };
  while (i < n) {
    if (type == 2 || type == 3) {
      if (o + std::size_t(bpp) > b.size()) fail(".tga cut short");
      put(&b[o]), o += std::size_t(bpp);
    } else {
      if (o >= b.size()) fail(".tga cut short");
      const int c = b[o++], run = (c & 0x7F) + 1;
      if (c & 0x80) {
        if (o + std::size_t(bpp) > b.size()) fail(".tga cut short");
        for (int k = 0; k < run && i < n; ++k) put(&b[o]);
        o += std::size_t(bpp);
      } else {
        for (int k = 0; k < run && i < n; ++k) {
          if (o + std::size_t(bpp) > b.size()) fail(".tga cut short");
          put(&b[o]), o += std::size_t(bpp);
        }
      }
    }
  }
  return img;
}

Rgba readPicture(const Bytes &b) {
  if (b.size() >= 8 && b[0] == 0x89 && b[1] == 'P') return readPng(b);
  if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8) fail("a .jpg (save it as .png or .tga)");
  if (b.size() >= 4 && std::memcmp(b.data(), "DDS ", 4) == 0) fail("a .dds (save it as .png or .tga)");
  return readTga(b);
}

// ---- the rig's bone names as a GTA ped's ----------------------------------------------------------------------------

// its words, lower case: "mixamorig:LeftUpLeg" -> left up leg, "arm left forearm twist 1", "upperarm_l" -> upperarm l
std::vector<std::string> nameWords(const std::string &raw) {
  std::string n = raw;
  if (auto c = n.find_last_of(':'); c != std::string::npos) n = n.substr(c + 1);
  if (auto c = n.find('|'); c != std::string::npos) n = n.substr(c + 1);
  std::string s;
  for (std::size_t k = 0; k < n.size(); ++k) {
    const char c = n[k];
    if (std::isupper(static_cast<unsigned char>(c)) && k > 0 && std::islower(static_cast<unsigned char>(n[k - 1])))
      s += ' ';
    if (std::isalnum(static_cast<unsigned char>(c))) s += char(std::tolower(static_cast<unsigned char>(c)));
    else s += ' ';
  }
  std::vector<std::string> w;
  std::string cur;
  for (char c : s + " ") {
    if (c == ' ') {
      if (!cur.empty()) w.push_back(cur), cur.clear();
    } else {
      cur += c;
    }
  }
  return w;
}

// the GTA key of a bone ("l upperarm"; "spine" for any spine bone, sorted out later), "" a helper or none
// `loose`: a helper's part too ("arm left upperarm twist 1" -> l upperarm: its twist and shape bones are often the
// clavicle's children, the upper arm's skin stayed at the side then)
std::string fbxBoneKind(const std::string &raw, char &side, bool loose = false) {
  const auto w = nameWords(raw);
  auto has = [&](const char *x) { return std::find(w.begin(), w.end(), x) != w.end(); };
  auto part = [&](const char *x) {
    for (const auto &t : w)
      if (t.find(x) != std::string::npos) return true;
    return false;
  };
  side = 0;
  if (has("left") || has("l") || has("lft")) side = 'l';
  else if (has("right") || has("r") || has("rgt")) side = 'r';
  // bip01 l ... / prefixes like "l_hand" end up as the word "l"
  if (!loose)
    for (const char *helper : {"twist", "adj", "end", "roll", "helper", "ik", "pole", "nub", "target", "ctrl", "corrective",
                             "sleeve", "bend", "share", "trapezius", "unused", "grp", "group", "dummy", "jiggle", "breast", "pendant", "cloth", "hair",
                             "eye", "jaw", "brow", "lip", "cheek", "ear", "tongue", "teeth", "boots", "pants", "skirt"})
      if (part(helper)) return "";
  if (loose)
    for (const char *other : {"hair", "eye", "jaw", "brow", "lip", "cheek", "ear", "tongue", "teeth", "breast", "cloth",
                              "pendant", "boots", "pants", "skirt", "unused", "grp", "trapezius", "knit"})
      if (part(other)) return "";
  if (part("finger") || part("thumb") || part("index") || part("middle") || part("ring") || part("pinky")) return "";
  if (part("toe") || has("ball")) return side ? "toe0" : "";
  if (part("foot") || part("ankle")) return side ? "foot" : "";
  if (part("thigh") || part("upleg") || part("upperleg")) return side ? "thigh" : "";
  if (part("calf") || part("knee") || part("shin") || part("lowerleg") || has("leg")) return side ? "calf" : "";
  if (part("forearm") || part("lowerarm") || part("elbow")) return side ? "forearm" : "";
  if (part("upperarm") || part("uparm")) return side ? "upperarm" : "";
  if (part("hand") || part("wrist") || part("palm")) return side ? "hand" : "";
  if (part("clavicle") || part("shoulder") || part("collar")) return side ? "clavicle" : "";
  if (has("arm")) return side ? "upperarm" : "";
  if (side) return "";
  if (part("pelvis") || part("hip")) return "pelvis";
  if (part("spine") || part("chest") || part("torso") || part("back")) return "spine";
  if (part("neck")) return "neck";
  if (has("head")) return "head";
  return "";
}

// ---- the model -----------------------------------------------------------------------------------------------------

struct FbxMat {
  double m[16];  // column-major (FBX): x' = m0 x + m4 y + m8 z + m12
  V3 apply(const V3 &v) const {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z + m[12], m[1] * v.x + m[5] * v.y + m[9] * v.z + m[13],
            m[2] * v.x + m[6] * v.y + m[10] * v.z + m[14]};
  }
  V3 turn(const V3 &v) const {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z, m[1] * v.x + m[5] * v.y + m[9] * v.z, m[2] * v.x + m[6] * v.y + m[10] * v.z};
  }
  static FbxMat identity() {
    FbxMat r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1;
    return r;
  }
  FbxMat operator*(const FbxMat &o) const {
    FbxMat r{};
    for (int c = 0; c < 4; ++c)
      for (int rr = 0; rr < 4; ++rr) {
        double s = 0;
        for (int k = 0; k < 4; ++k) s += m[k * 4 + rr] * o.m[c * 4 + k];
        r.m[c * 4 + rr] = s;
      }
    return r;
  }
};

DffMesh readFbx(const Bytes &data, const std::map<std::string, Bytes> &files, std::map<std::string, Rgba> &images,
                std::vector<std::string> &notes, std::string &log) {
  const FbxReader fbx(data);
  const FbxNode *objects = nullptr, *connections = nullptr;
  for (const auto &n : fbx.top) {
    if (n.name == "Objects") objects = &n;
    if (n.name == "Connections") connections = &n;
  }
  if (!objects || !connections) fail("the FBX has no objects or connections");
  struct Obj {
    const FbxNode *node;
    std::string kind, name, sub;
  };
  std::map<std::int64_t, Obj> obj;
  for (const auto &n : objects->kids) {
    if (n.props.size() < 3) continue;
    std::string name = n.props[1].s;
    if (auto z = name.find('\0'); z != std::string::npos) name = name.substr(0, z);
    obj[n.props[0].i] = {&n, n.name, name, n.props[2].s};
  }
  // child -> parents (OO), and (child, parent) -> property (OP)
  std::multimap<std::int64_t, std::int64_t> up;
  std::map<std::int64_t, std::vector<std::int64_t>> down;
  std::map<std::pair<std::int64_t, std::int64_t>, std::string> via;
  for (const auto &c : connections->kids) {
    if (c.name != "C" || c.props.size() < 3) continue;
    const std::int64_t child = c.props[1].i, parent = c.props[2].i;
    up.emplace(child, parent);
    down[parent].push_back(child);
    if (c.props.size() > 3) via[{child, parent}] = c.props[3].s;
  }
  auto parentsOf = [&](std::int64_t id, const char *kind) {
    std::vector<std::int64_t> out;
    for (auto [a, z] = up.equal_range(id); a != z; ++a)
      if (obj.count(a->second) && obj.at(a->second).kind == kind) out.push_back(a->second);
    return out;
  };
  auto childrenOf = [&](std::int64_t id, const char *kind) {
    std::vector<std::int64_t> out;
    if (down.count(id))
      for (auto c : down.at(id))
        if (obj.count(c) && obj.at(c).kind == kind) out.push_back(c);
    return out;
  };
  auto prop70 = [](const FbxNode *n, const char *name, double def[3], double out[3]) {
    for (int k = 0; k < 3; ++k) out[k] = def[k];
    const FbxNode *p = n ? n->first("Properties70") : nullptr;
    if (!p) return false;
    for (const auto &q : p->kids)
      if (q.props.size() >= 7 && q.props[0].s == name) {
        for (int k = 0; k < 3; ++k) out[k] = q.props[std::size_t(4 + k)].d;
        return true;
      }
    return false;
  };
  // a model's own transform (translation, pre-rotation, rotation XYZ in degrees, scale) and its global one
  auto local = [&](const FbxNode *n) {
    double z[3] = {0, 0, 0}, one[3] = {1, 1, 1}, t[3], r[3], pre[3], s[3];
    prop70(n, "Lcl Translation", z, t), prop70(n, "Lcl Rotation", z, r), prop70(n, "PreRotation", z, pre);
    prop70(n, "Lcl Scaling", one, s);
    auto rot = [](const double e[3]) {
      FbxMat x = FbxMat::identity(), y = FbxMat::identity(), zz = FbxMat::identity();
      const double a = e[0] * M_PI / 180, b = e[1] * M_PI / 180, c = e[2] * M_PI / 180;
      x.m[5] = std::cos(a), x.m[6] = std::sin(a), x.m[9] = -std::sin(a), x.m[10] = std::cos(a);
      y.m[0] = std::cos(b), y.m[2] = -std::sin(b), y.m[8] = std::sin(b), y.m[10] = std::cos(b);
      zz.m[0] = std::cos(c), zz.m[1] = std::sin(c), zz.m[4] = -std::sin(c), zz.m[5] = std::cos(c);
      return zz * y * x;
    };
    FbxMat T = FbxMat::identity(), S = FbxMat::identity();
    T.m[12] = t[0], T.m[13] = t[1], T.m[14] = t[2];
    S.m[0] = s[0], S.m[5] = s[1], S.m[10] = s[2];
    return T * rot(pre) * rot(r) * S;
  };
  std::map<std::int64_t, FbxMat> globalOf;
  std::function<FbxMat(std::int64_t)> global = [&](std::int64_t id) -> FbxMat {
    if (auto it = globalOf.find(id); it != globalOf.end()) return it->second;
    globalOf[id] = FbxMat::identity();  // a loop in the hierarchy ends here
    const auto ps = parentsOf(id, "Model");
    const FbxMat g = (ps.empty() ? FbxMat::identity() : global(ps[0])) * local(obj.at(id).node);
    return globalOf[id] = g;
  };
  auto mat16 = [](const FbxNode *n, FbxMat &m) {
    if (!n || n->props.empty() || n->props[0].ad.size() != 16) return false;
    for (int k = 0; k < 16; ++k) m.m[k] = n->props[0].ad[std::size_t(k)];
    return true;
  };

  // the bones: every model a cluster names, and their parents up to the top
  struct Bone {
    std::string name;
    std::int64_t parent = 0;
    V3 at{};
    bool placed = false;
    int depth = 0;
  };
  std::map<std::int64_t, Bone> bones;
  struct Cluster {
    std::int64_t bone;
    const FbxNode *node;
  };
  std::vector<std::int64_t> geoms;
  for (const auto &[id, o] : obj)
    if (o.kind == "Geometry" && o.sub == "Mesh") geoms.push_back(id);
  if (geoms.empty()) fail("the FBX has no mesh");
  std::map<std::int64_t, std::vector<Cluster>> clustersOf;
  for (auto g : geoms) {
    for (auto skin : childrenOf(g, "Deformer")) {
      if (obj.at(skin).sub != "Skin") continue;
      for (auto cl : childrenOf(skin, "Deformer")) {
        const auto bm = childrenOf(cl, "Model");
        if (bm.empty()) continue;
        clustersOf[g].push_back({bm[0], obj.at(cl).node});
        Bone &b = bones[bm[0]];
        b.name = obj.at(bm[0]).name;
        FbxMat link;
        if (mat16(obj.at(cl).node->first("TransformLink"), link)) b.at = {link.m[12], link.m[13], link.m[14]}, b.placed = true;
      }
    }
  }
  if (bones.empty()) fail("the FBX's meshes are not skinned to a skeleton (no Skin deformer)");
  {  // their parents, up to the top
    std::vector<std::int64_t> todo;
    for (const auto &[id, b] : bones) todo.push_back(id);
    while (!todo.empty()) {
      const std::int64_t id = todo.back();
      todo.pop_back();
      const auto ps = parentsOf(id, "Model");
      if (ps.empty()) continue;
      const std::string &sub = obj.at(ps[0]).sub;
      if (sub != "LimbNode" && sub != "Null" && sub != "Root" && !bones.count(ps[0])) continue;
      bones[id].parent = ps[0];
      if (!bones.count(ps[0])) bones[ps[0]].name = obj.at(ps[0]).name, todo.push_back(ps[0]);
    }
  }
  for (auto &[id, b] : bones) {
    if (!b.placed) {
      const FbxMat g = global(id);
      b.at = {g.m[12], g.m[13], g.m[14]};
    }
    for (std::int64_t q = b.parent; q && bones.count(q) && b.depth < 512; q = bones.at(q).parent) ++b.depth;
  }

  // their GTA names: per kind and side the bone nearest the top; the spine's first and last as spine and spine1
  std::map<std::string, std::pair<int, std::int64_t>> pick;
  std::vector<std::pair<int, std::int64_t>> spines;
  for (const auto &[id, b] : bones) {
    char side = 0;
    const std::string kind = fbxBoneKind(b.name, side);
    if (kind.empty()) continue;
    if (kind == "spine") {
      spines.push_back({b.depth, id});
      continue;
    }
    const std::string key = side ? std::string(1, side) + " " + kind : kind;
    auto it = pick.find(key);
    if (it == pick.end() || b.depth < it->second.first) pick[key] = {b.depth, id};
  }
  std::sort(spines.begin(), spines.end());
  if (!spines.empty()) pick["spine"] = spines.front();
  if (spines.size() > 1) pick["spine1"] = spines.back();
  if (!pick.count("pelvis") && pick.count("spine")) {  // a rig whose hips are its first spine bone
    pick["pelvis"] = pick["spine"];
    if (spines.size() > 2) pick["spine"] = spines[1];
    else pick.erase("spine");
  }
  std::map<std::int64_t, std::string> keyOf;
  for (const auto &[key, v] : pick) keyOf[v.second] = key;
  std::string missing;
  for (const char *need : {"pelvis", "head", "l foot", "r foot", "l upperarm", "r upperarm"})
    if (!pick.count(need)) missing += std::string(missing.empty() ? "" : ", ") + need;
  if (!missing.empty()) fail("the FBX's skeleton has no bone for: " + missing + " (by their names)");

  DffMesh m;
  std::map<std::int64_t, unsigned> index;
  for (const auto &[id, key] : keyOf) index[id] = unsigned(m.boneName.size()), m.boneName.push_back(key), m.boneAt.push_back(bones.at(id).at);
  m.boneParent.assign(m.boneName.size(), -1);
  // each bone onto the nearest GTA one above it (or itself); none: the hips
  auto mapped = [&](std::int64_t id) -> unsigned {
    if (!index.count(id) && bones.count(id)) {  // a helper of a limb: that limb's bone
      char side = 0;
      const std::string kind = fbxBoneKind(bones.at(id).name, side, true);
      if (!kind.empty() && kind != "spine") {
        const std::string key = side ? std::string(1, side) + " " + kind : kind;
        if (pick.count(key)) return index.at(pick.at(key).second);
      }
    }
    for (int hops = 0; id && hops < 512; ++hops) {
      if (auto it = index.find(id); it != index.end()) return it->second;
      id = bones.count(id) ? bones.at(id).parent : 0;
    }
    return index.at(pick.at("pelvis").second);
  };
  for (const auto &[id, k] : index)
    if (bones.at(id).parent) {
      const unsigned p = mapped(bones.at(id).parent);
      if (p != k) m.boneParent[k] = int(p);
    }
  log += "the FBX's skeleton: " + std::to_string(bones.size()) + " bones, as a GTA ped's:";
  for (const auto &[key, v] : pick) log += " " + key + "=" + bones.at(v.second).name + ";";
  log += "\n";

  // the meshes
  std::map<std::string, unsigned> matOf;
  std::size_t polys = 0;
  for (auto g : geoms) {
    const FbxNode &gn = *obj.at(g).node;
    const auto models = parentsOf(g, "Model");
    const FbxNode *V = gn.first("Vertices"), *PV = gn.first("PolygonVertexIndex");
    if (!V || !PV || V->props.empty() || PV->props.empty()) continue;
    const auto &vs = V->props[0].ad;
    const auto &pv = PV->props[0].ai;
    const std::size_t ncp = vs.size() / 3;
    // into the bind pose's space. The FBX SDK writes each cluster's Transform as the mesh at binding; Blender writes
    // it as the mesh in the bone's space (TransformLink x Transform is the mesh then, the same for every cluster: its
    // Ashley lay on her back with her arms up otherwise). Neither: the model's own.
    FbxMat place = models.empty() ? FbxMat::identity() : global(models[0]);
    if (clustersOf.count(g)) {
      const auto &cs = clustersOf.at(g);
      FbxMat t0, l0;
      if (mat16(cs.front().node->first("Transform"), t0)) {
        place = t0;
        if (cs.size() >= 2 && mat16(cs.front().node->first("TransformLink"), l0)) {
          const FbxMat a = l0 * t0;
          bool same = true;
          for (std::size_t k = 1; k < cs.size() && k < 8 && same; ++k) {
            FbxMat tk, lk;
            if (!mat16(cs[k].node->first("Transform"), tk) || !mat16(cs[k].node->first("TransformLink"), lk)) continue;
            const FbxMat b = lk * tk;
            for (int q = 0; q < 16; ++q)
              if (std::fabs(a.m[q] - b.m[q]) > 1e-3 * (1 + std::fabs(a.m[q]))) same = false;
          }
          if (same) place = a;
        }
      }
    }
    // weights per control point, on the GTA bones
    std::vector<std::vector<std::pair<unsigned, double>>> wOf(ncp);
    if (clustersOf.count(g))
      for (const auto &cl : clustersOf.at(g)) {
        const FbxNode *I = cl.node->first("Indexes"), *W = cl.node->first("Weights");
        if (!I || !W || I->props.empty() || W->props.empty()) continue;
        const unsigned to = mapped(cl.bone);
        const auto &ii = I->props[0].ai;
        const auto &ww = W->props[0].ad;
        for (std::size_t k = 0; k < ii.size() && k < ww.size(); ++k) {
          if (ii[k] < 0 || std::size_t(ii[k]) >= ncp || ww[k] <= 0) continue;
          auto &list = wOf[std::size_t(ii[k])];
          bool merged = false;
          for (auto &e : list)
            if (e.first == to) e.second += ww[k], merged = true;
          if (!merged) list.push_back({to, ww[k]});
        }
      }
    // the layers: normals, texture coordinates, materials
    auto layer = [&](const char *el, const char *dataName, const char *indexName, std::vector<double> &vals,
                     std::vector<std::int64_t> &idx, std::string &mapping) {
      const FbxNode *L = gn.first(el);
      if (!L) return false;
      const FbxNode *D = L->first(dataName), *M = L->first("MappingInformationType"), *R = L->first("ReferenceInformationType");
      if (!D || D->props.empty()) return false;
      vals = D->props[0].ad;
      mapping = M && !M->props.empty() ? M->props[0].s : "";
      const bool direct = !(R && !R->props.empty() && R->props[0].s == "IndexToDirect");
      idx.clear();
      if (!direct) {
        const FbxNode *I = L->first(indexName);
        if (I && !I->props.empty()) idx = I->props[0].ai;
      }
      return true;
    };
    std::vector<double> nv, uvv;
    std::vector<std::int64_t> ni, ui;
    std::string nmap, umap;
    const bool hasN = layer("LayerElementNormal", "Normals", "NormalsIndex", nv, ni, nmap);
    const bool hasU = layer("LayerElementUV", "UV", "UVIndex", uvv, ui, umap);
    std::vector<std::int64_t> polyMat;
    std::string mmap;
    if (const FbxNode *L = gn.first("LayerElementMaterial")) {
      if (const FbxNode *M = L->first("Materials"); M && !M->props.empty()) polyMat = M->props[0].ai;
      if (const FbxNode *MI = L->first("MappingInformationType"); MI && !MI->props.empty()) mmap = MI->props[0].s;
    }
    // the mesh's materials in their order, as our materials (by the colour texture)
    std::vector<unsigned> matMap;
    for (auto mid : models.empty() ? std::vector<std::int64_t>{} : childrenOf(models[0], "Material")) {
      std::string tex;
      for (auto t : childrenOf(mid, "Texture")) {
        const std::string pn = lower(via.count({t, mid}) ? via.at({t, mid}) : "");
        const bool colour = pn.find("diffuse") != std::string::npos || pn.find("base") != std::string::npos ||
                            pn.find("albedo") != std::string::npos;
        if (!colour && !tex.empty()) continue;
        std::string file;
        for (const char *f : {"RelativeFilename", "FileName"})
          if (const FbxNode *F = obj.at(t).node->first(f); F && !F->props.empty() && file.empty()) file = F->props[0].s;
        if (auto s = file.find_last_of("/\\"); s != std::string::npos) file = file.substr(s + 1);
        file = lower(file);
        if (file.empty()) continue;
        if (!images.count(file)) {
          // the picture: beside the FBX, else packed into it (its Video's Content)
          Bytes pic;
          if (files.count(file)) pic = files.at(file);
          for (auto v : childrenOf(t, "Video"))
            if (pic.empty())
              if (const FbxNode *C = obj.at(v).node->first("Content"); C && !C->props.empty() && !C->props[0].s.empty())
                pic.assign(C->props[0].s.begin(), C->props[0].s.end());
          if (pic.empty()) {
            notes.push_back("  " + file + ": not beside the FBX (grey)");
          } else {
            try {
              images[file] = readPicture(pic);
            } catch (const std::exception &e) {
              notes.push_back("  " + file + ": " + e.what() + " (grey)");
            }
          }
        }
        if (images.count(file)) tex = file;
        if (colour && !tex.empty()) break;
      }
      // a layer drawn see-through over another (the knit's fur shells and the eyes' wet film: a clear picture, or one
      // even, half clear colour): the game draws a material solid or cut out, so it is left out (DevJunior's Ashley:
      // a white sweater)
      if (!tex.empty()) {
        const Rgba &img = images.at(tex);
        std::size_t clear = 0, n = img.px.size() / 4;
        double sum = 0, sq = 0;
        for (std::size_t q = 0; q < n; ++q) {
          const double a = img.px[q * 4 + 3];
          clear += a < 16, sum += a, sq += a * a;
        }
        const double mean = n ? sum / double(n) : 255, sd = n ? std::sqrt(std::max(0.0, sq / double(n) - mean * mean)) : 0;
        if (n && (clear >= n * 98 / 100 || (mean < 192 && sd < 8))) {
          notes.push_back("  " + obj.at(mid).name + ": " + tex + " is see-through (a layer over another): left out");
          matMap.push_back(~0u);
          continue;
        }
      }
      const std::string key = tex.empty() ? "material " + obj.at(mid).name : tex;
      auto it = matOf.find(key);
      if (it == matOf.end()) {
        it = matOf.emplace(key, unsigned(m.matTex.size())).first;
        m.matTex.push_back(tex.empty() ? "" : key);
        double grey[3] = {0.7, 0.7, 0.7}, c[3];
        prop70(obj.at(mid).node, "DiffuseColor", grey, c);
        m.matColour.push_back({std::uint8_t(std::clamp(c[0], 0.0, 1.0) * 255), std::uint8_t(std::clamp(c[1], 0.0, 1.0) * 255),
                               std::uint8_t(std::clamp(c[2], 0.0, 1.0) * 255), 255});
        m.matName.push_back(obj.at(mid).name);
      }
      matMap.push_back(it->second);
    }
    if (matMap.empty()) {
      matMap.push_back(unsigned(m.matTex.size()));
      m.matTex.push_back(""), m.matColour.push_back({180, 180, 180, 255}), m.matName.push_back("");
    }
    // the corners: a vertex per control point, texture coordinate and normal
    std::unordered_map<std::uint64_t, unsigned> vertexOf;
    auto value = [&](const std::vector<double> &vals, const std::vector<std::int64_t> &idx, const std::string &mapping,
                     std::size_t corner, std::size_t cp, int dims, double *out) {
      std::size_t k = mapping == "ByPolygonVertex" ? corner : cp;
      if (!idx.empty()) k = k < idx.size() && idx[k] >= 0 ? std::size_t(idx[k]) : 0;
      for (int d = 0; d < dims; ++d) out[d] = k * std::size_t(dims) + std::size_t(d) < vals.size() ? vals[k * std::size_t(dims) + std::size_t(d)] : 0;
      return k;
    };
    std::size_t start = 0, poly = 0;
    for (std::size_t c = 0; c < pv.size(); ++c) {
      if (pv[c] >= 0) continue;
      const std::size_t end = c;  // corners start..end
      const std::size_t mat = mmap == "AllSame" || polyMat.empty() ? 0
                              : std::size_t(poly < polyMat.size() ? std::max<std::int64_t>(0, polyMat[poly]) : 0);
      const unsigned pm = matMap[std::min(mat, matMap.size() - 1)];
      if (pm == ~0u) {  // a left-out layer
        start = c + 1, ++poly;
        continue;
      }
      std::vector<unsigned> ids;
      for (std::size_t k = start; k <= end; ++k) {
        const std::size_t cp = std::size_t(pv[k] < 0 ? ~pv[k] : pv[k]);
        if (cp >= ncp) break;
        double n[3] = {0, 1, 0}, uv[2] = {0, 0};
        if (hasN) value(nv, ni, nmap, k, cp, 3, n);
        const std::size_t uk = hasU ? value(uvv, ui, umap, k, cp, 2, uv) : 0;
        const auto q = [](double x) { return std::uint64_t(std::int64_t(std::lround(x * 127)) & 0xFF); };
        const std::uint64_t key = (std::uint64_t(cp) << 40) ^ (std::uint64_t(uk & 0xFFFFF) << 24) ^ (q(n[0]) << 16) ^
                                  (q(n[1]) << 8) ^ q(n[2]);
        auto it = vertexOf.find(key);
        if (it == vertexOf.end()) {
          it = vertexOf.emplace(key, unsigned(m.pos.size())).first;
          m.pos.push_back(place.apply({vs[cp * 3], vs[cp * 3 + 1], vs[cp * 3 + 2]}));
          m.nrm.push_back(unit(place.turn({n[0], n[1], n[2]})));
          m.uv.push_back({uv[0], 1.0 - uv[1]});
          // the 4 heaviest weights
          auto w = wOf[cp];
          std::sort(w.begin(), w.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
          std::array<std::uint8_t, 4> bi{};
          std::array<float, 4> bw{};
          double sum = 0;
          for (std::size_t j = 0; j < 4 && j < w.size(); ++j) bi[j] = std::uint8_t(w[j].first), bw[j] = float(w[j].second), sum += w[j].second;
          if (sum <= 0) bi = {std::uint8_t(index.at(pick.at("pelvis").second)), 0, 0, 0}, bw = {1, 0, 0, 0}, sum = 1;
          for (auto &x : bw) x = float(x / sum);
          m.bone.push_back(bi), m.weight.push_back(bw);
        }
        ids.push_back(it->second);
      }
      for (std::size_t k = 1; k + 1 < ids.size(); ++k) m.tris.push_back({ids[0], ids[k], ids[k + 1], pm});
      start = c + 1, ++poly;
    }
    polys += poly;
  }
  if (m.tris.empty()) fail("the FBX's meshes have no faces");
  log += fmt("the FBX: %zu meshes, %zu faces, %zu materials, %zu pictures\n", geoms.size(), polys, m.matTex.size(), images.size());
  if (m.boneName.size() > 255) fail("more than 255 bones");
  return m;
}
