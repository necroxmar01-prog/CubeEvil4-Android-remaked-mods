// CubeEvil4 Studio: GTA San Andreas skins (.dff with its .txd, RenderWare 3.6) on a character of the discs (DevJunior,
// 19.11.2026: "a model from another game over Leon"). Included by studio_convert.cpp inside its namespace: it uses the
// archive, model, texture and TPL helpers above.
//
// The way: the skin's mesh (positions, normals, texture coordinates, material per triangle, up to four bones per
// vertex) is read from the .dff, its textures from the .txd (Direct3D 8/9 rasters: DXT1/3/5, 8888, 888, 565, 1555,
// 4444, 8-bit palettes). The mesh is moved from the GTA skeleton onto the disc's (retarget): the GTA bones are matched
// by name to the disc character's bones (kLeon below: the pl00 skeleton, the same in pl08..pl10), the whole mesh is
// turned and scaled into the disc's space (Z up and Y forward to Y up and Z forward, the height of the head over the
// toes), then each bone's piece is turned so that the bone points as the disc's does, stretched along it to the disc
// bone's length, and moved to the disc bone's joint; a vertex blends the pieces by its weights. The result is written
// as a model of the PC release's layout (one part per material, triangles) and converted with convertModel, the disc's
// skeleton, blend and flip tables kept; its textures go behind the disc's own in the body's TPL. The disc's head, face,
// hair, outfit, wound and hand models are folded into one point each (all their vertices on one bone's joint): the
// skin is the whole character.

struct V3 {
  double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double len(V3 a) { return std::sqrt(dot(a, a)); }
V3 unit(V3 a) {
  const double l = len(a);
  return l > 1e-12 ? a * (1.0 / l) : V3{0, 1, 0};
}

struct M3 {
  double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  V3 operator*(V3 v) const {
    return {m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z, m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z,
            m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z};
  }
};
// the rotation turning unit vector a onto unit vector b
M3 turn(V3 a, V3 b) {
  const V3 v = cross(a, b);
  const double c = dot(a, b), s = len(v);
  M3 r;
  if (s < 1e-9) {
    if (c > 0) return r;
    // opposite: half a turn about any axis normal to a
    V3 ax = unit(std::fabs(a.x) < 0.9 ? cross(a, {1, 0, 0}) : cross(a, {0, 1, 0}));
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        const double ai = i == 0 ? ax.x : i == 1 ? ax.y : ax.z, aj = j == 0 ? ax.x : j == 1 ? ax.y : ax.z;
        r.m[i][j] = 2 * ai * aj - (i == j ? 1 : 0);
      }
    return r;
  }
  const V3 k = v * (1.0 / s);
  const double K[3][3] = {{0, -k.z, k.y}, {k.z, 0, -k.x}, {-k.y, k.x, 0}};
  double K2[3][3];
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      K2[i][j] = 0;
      for (int t = 0; t < 3; ++t) K2[i][j] += K[i][t] * K[t][j];
    }
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) r.m[i][j] = (i == j ? 1 : 0) + s * K[i][j] + (1 - c) * K2[i][j];
  return r;
}

// ---- RenderWare streams
struct Rw {
  std::uint32_t type, size, version;
  std::size_t data;  // after the 12-byte header
  std::size_t end() const { return data + size; }
};
Rw rwAt(const Bytes &b, std::size_t p) {
  if (p + 12 > b.size()) fail("RenderWare: truncated");
  Rw c{le32(b, p), le32(b, p + 4), le32(b, p + 8), p + 12};
  if (c.end() > b.size()) fail(fmt("RenderWare: chunk %#x runs past the file", c.type));
  return c;
}
std::vector<Rw> rwKids(const Bytes &b, const Rw &parent) {
  std::vector<Rw> out;
  for (std::size_t p = parent.data; p + 12 <= parent.end();) {
    const Rw c = rwAt(b, p);
    out.push_back(c);
    p = c.end();
  }
  return out;
}
const Rw *rwFind(const std::vector<Rw> &kids, std::uint32_t type) {
  for (const auto &k : kids)
    if (k.type == type) return &k;
  return nullptr;
}

std::string lower(std::string s) {
  for (auto &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct DffMesh {
  std::vector<V3> pos, nrm;
  std::vector<std::array<double, 2>> uv;
  std::vector<std::array<unsigned, 4>> tris;  // a, b, c, material
  std::vector<std::string> matTex;            // per material: its texture's name ("" none)
  std::vector<std::array<std::uint8_t, 4>> matColour;
  std::vector<std::string> matName;           // per material: its own name (RE4 Remake mods; "" unknown)
  std::vector<std::array<std::uint8_t, 4>> bone;  // per vertex: skin bone indices
  std::vector<std::array<float, 4>> weight;
  std::vector<std::string> boneName;  // per skin bone
  std::vector<V3> boneAt;             // per skin bone: its joint in the bind pose
  std::vector<int> boneParent;        // per skin bone: its parent skin bone, -1 none
  std::vector<float> cloth;           // per vertex: its share on a skirt's bones (.fbx; empty: none)
};

DffMesh readDff(const Bytes &b) {
  const Rw clump = rwAt(b, 0);
  if (clump.type != 0x10) fail("not a .dff (no clump)");
  const auto kids = rwKids(b, clump);
  const Rw *frames = rwFind(kids, 0x0E), *geoms = rwFind(kids, 0x1A);
  if (!frames || !geoms) fail(".dff without frames or geometry");
  // frames: parents, names, HAnim node ids
  const auto fk = rwKids(b, *frames);
  if (fk.empty() || fk[0].type != 1) fail(".dff: frame list without its data");
  const std::uint32_t nf = le32(b, fk[0].data);
  std::vector<int> fParent(nf, -1), fNode(nf, -1);
  std::vector<std::string> fName(nf);
  std::vector<V3> fLocal(nf);
  std::vector<M3> fRot(nf);
  for (std::uint32_t i = 0; i < nf; ++i) {
    const std::size_t r = fk[0].data + 4 + std::size_t(i) * 56;
    for (int a = 0; a < 3; ++a)
      for (int c = 0; c < 3; ++c) fRot[i].m[a][c] = lef32(b, r + std::size_t(a * 12 + c * 4));
    fLocal[i] = {lef32(b, r + 36), lef32(b, r + 40), lef32(b, r + 44)};
    fParent[i] = int(std::int32_t(le32(b, r + 48)));
  }
  std::vector<std::int32_t> hierarchy;  // the root's HAnim node ids, in the skin's bone order
  std::size_t ext = 0;
  for (std::size_t k = 1; k < fk.size() && ext < nf; ++k) {
    if (fk[k].type != 3) continue;
    for (const auto &e : rwKids(b, fk[k])) {
      if (e.type == 0x253F2FE) {
        fName[ext] = std::string(reinterpret_cast<const char *>(&b[e.data]), e.size);
        fName[ext] = fName[ext].c_str();
      } else if (e.type == 0x11E && e.size >= 12) {
        fNode[ext] = int(std::int32_t(le32(b, e.data + 4)));
        const std::uint32_t n = le32(b, e.data + 8);
        if (n && e.size >= 20 + n * 12)
          for (std::uint32_t j = 0; j < n; ++j) hierarchy.push_back(std::int32_t(le32(b, e.data + 20 + j * 12)));
      }
    }
    ++ext;
  }
  // the frames' bind pose in model space (row vectors: v * R + t)
  std::vector<V3> fWorld(nf);
  std::vector<M3> fWRot(nf);
  for (std::uint32_t i = 0; i < nf; ++i) {
    const int p = fParent[i];
    if (p < 0 || std::uint32_t(p) >= i) {
      fWorld[i] = fLocal[i], fWRot[i] = fRot[i];
      continue;
    }
    // world = local * parentWorld
    M3 r;
    for (int a = 0; a < 3; ++a)
      for (int c = 0; c < 3; ++c) {
        r.m[a][c] = 0;
        for (int t = 0; t < 3; ++t) r.m[a][c] += fRot[i].m[a][t] * fWRot[std::size_t(p)].m[t][c];
      }
    fWRot[i] = r;
    const V3 l = fLocal[i];
    const M3 &pr = fWRot[std::size_t(p)];
    fWorld[i] = V3{l.x * pr.m[0][0] + l.y * pr.m[1][0] + l.z * pr.m[2][0],
                   l.x * pr.m[0][1] + l.y * pr.m[1][1] + l.z * pr.m[2][1],
                   l.x * pr.m[0][2] + l.y * pr.m[1][2] + l.z * pr.m[2][2]} +
                fWorld[std::size_t(p)];
  }

  // the first geometry
  const auto gk = rwKids(b, *geoms);
  const Rw *geo = rwFind(gk, 0x0F);
  if (!geo) fail(".dff without a geometry");
  const auto parts = rwKids(b, *geo);
  if (parts.empty() || parts[0].type != 1) fail(".dff: geometry without its data");
  const Rw &gs = parts[0];
  DffMesh m;
  std::size_t p = gs.data;
  const unsigned flags = le16(b, p);
  unsigned nuv = b.at(p + 2);
  const bool native = b.at(p + 3) != 0;
  const std::uint32_t ntri = le32(b, p + 4), nv = le32(b, p + 8), nmorph = le32(b, p + 12);
  if (native) fail(".dff: a console-native geometry (PS2 / Xbox), not the PC's");
  if (!nv || nv > 200000) fail(fmt(".dff: %u vertices", nv));
  p += 16;
  const std::uint32_t ver = gs.version;
  const std::uint32_t lib = (ver & 0xFFFF0000u) ? (((ver >> 14) & 0x3FF00u) + 0x30000u) | ((ver >> 16) & 0x3Fu) : ver << 8;
  if (lib < 0x34000) p += 12;  // ambient, specular, diffuse
  if (!nuv) nuv = (flags & 0x80) ? 2 : (flags & 0x04) ? 1 : 0;
  if (flags & 0x08) p += std::size_t(nv) * 4;  // prelit colours
  m.uv.assign(nv, {0, 0});
  if (nuv) {
    for (std::uint32_t i = 0; i < nv; ++i) m.uv[i] = {lef32(b, p + i * 8), lef32(b, p + i * 8 + 4)};
    p += std::size_t(nuv) * nv * 8;
  }
  for (std::uint32_t i = 0; i < ntri; ++i) {
    const std::size_t t = p + std::size_t(i) * 8;
    m.tris.push_back({le16(b, t + 2), le16(b, t), le16(b, t + 6), le16(b, t + 4)});
  }
  p += std::size_t(ntri) * 8;
  if (!nmorph) fail(".dff: no morph target");
  p += 16;  // bounding sphere
  const bool hasPos = le32(b, p) != 0, hasNrm = le32(b, p + 4) != 0;
  p += 8;
  if (!hasPos) fail(".dff: no vertex positions");
  for (std::uint32_t i = 0; i < nv; ++i) m.pos.push_back({lef32(b, p + i * 12), lef32(b, p + i * 12 + 4), lef32(b, p + i * 12 + 8)});
  p += std::size_t(nv) * 12;
  if (hasNrm)
    for (std::uint32_t i = 0; i < nv; ++i)
      m.nrm.push_back(unit({lef32(b, p + i * 12), lef32(b, p + i * 12 + 4), lef32(b, p + i * 12 + 8)}));

  // materials
  if (const Rw *ml = rwFind(parts, 0x08)) {
    for (const auto &mat : rwKids(b, *ml)) {
      if (mat.type != 0x07) continue;
      const auto mk = rwKids(b, mat);
      std::array<std::uint8_t, 4> colour{255, 255, 255, 255};
      if (!mk.empty() && mk[0].type == 1 && mk[0].size >= 8)
        colour = {b[mk[0].data + 4], b[mk[0].data + 5], b[mk[0].data + 6], b[mk[0].data + 7]};
      std::string tex;
      if (const Rw *t = rwFind(mk, 0x06)) {
        for (const auto &s : rwKids(b, *t))
          if (s.type == 2) {
            tex = std::string(reinterpret_cast<const char *>(&b[s.data]), s.size).c_str();
            break;
          }
      }
      m.matTex.push_back(lower(tex));
      m.matColour.push_back(colour);
    }
  }
  if (m.matTex.empty()) m.matTex.push_back(""), m.matColour.push_back({255, 255, 255, 255});

  // extensions: the triangles by material (when the geometry keeps none) and the skin
  if (const Rw *ex = rwFind(parts, 0x03)) {
    const auto ek = rwKids(b, *ex);
    // the mesh split by material (BinMesh) wins over the geometry's own list: exporters often leave its material
    // numbers at 0 (DevJunior's Master Chief: all 2064 triangles on material 0, BinMesh 50 / 1910 / 104)
    if (const Rw *bm = rwFind(ek, 0x50E)) {
      m.tris.clear();
      {
        const std::uint32_t strip = le32(b, bm->data), nmesh = le32(b, bm->data + 4);
        std::size_t q = bm->data + 12;
        for (std::uint32_t k = 0; k < nmesh; ++k) {
          const std::uint32_t n = le32(b, q), mat = le32(b, q + 4);
          std::vector<unsigned> idx;
          for (std::uint32_t j = 0; j < n; ++j) idx.push_back(le32(b, q + 8 + j * 4));
          q += 8 + std::size_t(n) * 4;
          if (strip) {
            for (std::size_t j = 2; j < idx.size(); ++j) {
              unsigned a = idx[j - 2], c = idx[j - 1], d = idx[j];
              if (a == c || c == d || a == d) continue;
              if (j % 2) std::swap(a, c);
              m.tris.push_back({a, c, d, mat});
            }
          } else {
            for (std::size_t j = 0; j + 2 < idx.size(); j += 3) m.tris.push_back({idx[j], idx[j + 1], idx[j + 2], mat});
          }
        }
      }
    }
    if (const Rw *sk = rwFind(ek, 0x116)) {
      std::size_t q = sk->data;
      const unsigned nb = b.at(q), nused = b.at(q + 1);
      q += 4 + nused;
      for (std::uint32_t i = 0; i < nv; ++i) m.bone.push_back({b.at(q + i * 4), b.at(q + i * 4 + 1), b.at(q + i * 4 + 2), b.at(q + i * 4 + 3)});
      q += std::size_t(nv) * 4;
      for (std::uint32_t i = 0; i < nv; ++i)
        m.weight.push_back({lef32(b, q + i * 16), lef32(b, q + i * 16 + 4), lef32(b, q + i * 16 + 8), lef32(b, q + i * 16 + 12)});
      q += std::size_t(nv) * 16;
      const bool prefixed = nused == 0 && sk->end() - q >= std::size_t(nb) * 68;
      std::vector<V3> at(nb);
      for (unsigned k = 0; k < nb; ++k) {
        if (prefixed) q += 4;
        // the inverse of the bone's bind matrix (row vectors): its joint is -t * R^T
        double r[3][3];
        for (int a = 0; a < 3; ++a)
          for (int c = 0; c < 3; ++c) r[a][c] = lef32(b, q + std::size_t(a * 16 + c * 4));
        const V3 t{lef32(b, q + 48), lef32(b, q + 52), lef32(b, q + 56)};
        at[k] = V3{-(t.x * r[0][0] + t.y * r[0][1] + t.z * r[0][2]), -(t.x * r[1][0] + t.y * r[1][1] + t.z * r[1][2]),
                   -(t.x * r[2][0] + t.y * r[2][1] + t.z * r[2][2])};
        q += 64;
      }
      // the skin's bone k is the HAnim hierarchy's node k: its frame by node id
      m.boneAt = at;
      m.boneName.assign(nb, "");
      m.boneParent.assign(nb, -1);
      std::vector<int> frameOf(nb, -1);
      for (unsigned k = 0; k < nb && k < hierarchy.size(); ++k)
        for (std::uint32_t f = 0; f < nf; ++f)
          if (fNode[f] == hierarchy[k]) frameOf[k] = int(f);
      for (unsigned k = 0; k < nb; ++k) {
        if (frameOf[k] < 0) continue;
        m.boneName[k] = lower(fName[std::size_t(frameOf[k])]);
        // the parent bone: the nearest frame up the chain that is a bone
        for (int f = fParent[std::size_t(frameOf[k])]; f >= 0 && m.boneParent[k] < 0; f = fParent[std::size_t(f)])
          for (unsigned j = 0; j < nb; ++j)
            if (frameOf[j] == f) m.boneParent[k] = int(j);
      }
    }
  }
  if (m.bone.empty()) fail(".dff without a skin (a ped skin has one: this looks like an object or a vehicle)");
  if (m.tris.empty()) fail(".dff without triangles");
  if (m.nrm.empty()) {
    // smooth normals from the triangles
    m.nrm.assign(nv, V3{});
    for (const auto &t : m.tris) {
      if (t[0] >= nv || t[1] >= nv || t[2] >= nv) continue;
      const V3 n = cross(m.pos[t[1]] - m.pos[t[0]], m.pos[t[2]] - m.pos[t[0]]);
      for (int k = 0; k < 3; ++k) m.nrm[t[std::size_t(k)]] = m.nrm[t[std::size_t(k)]] + n;
    }
    for (auto &n : m.nrm) n = unit(n);
  }
  return m;
}

// ---- the .txd's textures
std::map<std::string, Rgba> readTxd(const Bytes &b, std::vector<std::string> &notes) {
  std::map<std::string, Rgba> out;
  const Rw dict = rwAt(b, 0);
  if (dict.type != 0x16) fail("not a .txd (no texture dictionary)");
  for (const auto &tn : rwKids(b, dict)) {
    if (tn.type != 0x15) continue;
    const auto kids = rwKids(b, tn);
    if (kids.empty() || kids[0].type != 1) continue;
    const std::size_t s = kids[0].data;
    const std::uint32_t platform = le32(b, s);
    std::string name = lower(std::string(reinterpret_cast<const char *>(&b[s + 8]), 32).c_str());
    if (platform != 8 && platform != 9) {
      notes.push_back(fmt("  %s: a console texture (platform %u), left out", name.c_str(), platform));
      continue;
    }
    const std::uint32_t raster = le32(b, s + 72), d3d = le32(b, s + 76);
    const unsigned w = le16(b, s + 80), h = le16(b, s + 82), depth = b.at(s + 84), levels = b.at(s + 85);
    const unsigned comp = b.at(s + 87);  // D3D8: 1 DXT1, 3 DXT3 (D3D9: flags)
    std::size_t q = s + 88;
    std::vector<std::array<std::uint8_t, 4>> palette;
    if (raster & 0x6000) {
      const unsigned n = (raster & 0x2000) ? 256 : 16;
      for (unsigned i = 0; i < n; ++i) palette.push_back({b.at(q + i * 4), b.at(q + i * 4 + 1), b.at(q + i * 4 + 2), b.at(q + i * 4 + 3)});
      q += std::size_t(n) * 4;
    }
    if (!levels || !w || !h || w > 4096 || h > 4096) continue;
    const std::uint32_t size = le32(b, q);
    q += 4;
    if (q + size > kids[0].end()) {
      notes.push_back("  " + name + ": truncated, left out");
      continue;
    }
    std::uint32_t four = 0;
    if (platform == 9 && (d3d == 0x31545844u || d3d == 0x33545844u || d3d == 0x35545844u)) four = d3d;
    if (platform == 8 && comp == 1) four = 0x31545844u;
    if (platform == 8 && comp == 3) four = 0x33545844u;
    Rgba img;
    img.w = int(w), img.h = int(h);
    img.px.assign(std::size_t(w) * h * 4, 255);
    bool ok = false;
    if (four) {
      // through the DDS reader: a header in front of the first level
      Bytes dds(128, 0);
      std::memcpy(dds.data(), "DDS ", 4);
      auto le = [&](std::size_t at, std::uint32_t v) {
        dds[at] = std::uint8_t(v), dds[at + 1] = std::uint8_t(v >> 8), dds[at + 2] = std::uint8_t(v >> 16),
        dds[at + 3] = std::uint8_t(v >> 24);
      };
      le(4, 124), le(12, h), le(16, w), le(76, 32), le(80, 4), le(84, four);
      dds.insert(dds.end(), b.begin() + long(q), b.begin() + long(q + size));
      std::string why;
      ok = decodeDds(dds, img, &why);
      if (!ok) notes.push_back("  " + name + ": " + why);
    } else if (!palette.empty() && depth == 8 && size >= w * h) {
      for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        const auto &c = palette[b[q + i]];
        std::copy(c.begin(), c.end(), img.px.begin() + long(i * 4));
      }
      ok = true;
    } else if (!palette.empty() && depth == 4 && size * 2 >= w * h) {
      for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
        const unsigned v = (b[q + i / 2] >> ((i % 2) * 4)) & 15;
        std::copy(palette[v].begin(), palette[v].end(), img.px.begin() + long(i * 4));
      }
      ok = true;
    } else {
      // plain rasters, by the raster format (D3D8) or the Direct3D one (D3D9)
      const unsigned kind = raster & 0x0F00;
      int bpp = 0;
      unsigned form = 0;  // 1 8888, 2 888 (in 32 bits), 3 565, 4 1555, 5 4444, 6 L8
      if (platform == 9 && d3d == 21) form = 1, bpp = 4;
      else if (platform == 9 && d3d == 22) form = 2, bpp = 4;
      else if (platform == 9 && d3d == 23) form = 3, bpp = 2;
      else if (platform == 9 && d3d == 25) form = 4, bpp = 2;
      else if (platform == 9 && d3d == 26) form = 5, bpp = 2;
      else if (platform == 9 && d3d == 50) form = 6, bpp = 1;
      else if (kind == 0x0500) form = 1, bpp = 4;
      else if (kind == 0x0600) form = 2, bpp = 4;
      else if (kind == 0x0200) form = 3, bpp = 2;
      else if (kind == 0x0100) form = 4, bpp = 2;
      else if (kind == 0x0300) form = 5, bpp = 2;
      else if (kind == 0x0400) form = 6, bpp = 1;
      if (form && size >= std::size_t(w) * h * std::size_t(bpp)) {
        for (std::size_t i = 0; i < std::size_t(w) * h; ++i) {
          auto *o = &img.px[i * 4];
          const std::size_t at = q + i * std::size_t(bpp);
          if (form == 1 || form == 2) {
            o[0] = b[at + 2], o[1] = b[at + 1], o[2] = b[at], o[3] = form == 1 ? b[at + 3] : 255;
          } else if (form == 6) {
            o[0] = o[1] = o[2] = b[at], o[3] = 255;
          } else {
            const unsigned v = unsigned(b[at] | b[at + 1] << 8);
            if (form == 3) rgb565(v, o), o[3] = 255;
            else if (form == 4)
              o[0] = std::uint8_t(((v >> 10) & 31) * 255 / 31), o[1] = std::uint8_t(((v >> 5) & 31) * 255 / 31),
              o[2] = std::uint8_t((v & 31) * 255 / 31), o[3] = (v & 0x8000) ? 255 : 0;
            else
              o[0] = std::uint8_t(((v >> 8) & 15) * 17), o[1] = std::uint8_t(((v >> 4) & 15) * 17),
              o[2] = std::uint8_t((v & 15) * 17), o[3] = std::uint8_t((v >> 12) * 17);
          }
        }
        ok = true;
      } else {
        notes.push_back(fmt("  %s: raster %#x / %u not read here", name.c_str(), raster, d3d));
      }
    }
    if (ok) out[name] = std::move(img);
  }
  return out;
}

// ---- the disc character's skeleton
struct DiscBone {
  int parent;
  V3 at;  // its joint in the bind pose (the model's space: Y up, Z forward)
};
std::map<int, DiscBone> discSkeleton(const Bytes &model) {
  std::map<int, std::pair<int, V3>> local;
  for (const auto &r : gcBoneRecords(model))
    local[r[0]] = {r[1] == 0xFF ? -1 : int(r[1]), V3{bef32(r, 4), bef32(r, 8), bef32(r, 12)}};
  std::map<int, DiscBone> out;
  std::function<V3(int, int)> world = [&](int id, int depth) -> V3 {
    auto it = local.find(id);
    if (it == local.end() || depth > 256) return V3{};
    const V3 own = it->second.second;
    return it->second.first < 0 ? own : own + world(it->second.first, depth + 1);
  };
  for (const auto &[id, v] : local) out[id] = {v.first, world(id, 0)};
  return out;
}

// GTA ped bone (lower case, "bip01 " dropped) -> the disc bone it drives, the GTA bone and the disc bone it points to
// ("" / -1: it keeps its parent's turn). Leon's pl00..pl10 skeleton: 17 hips, 2 / 3 spine, 4 head, 27 jaw, 11 / 13 /
// 14 / 16 the left clavicle, upper arm, forearm, hand (5 / 7 / 8 / 10 the right), 22 / 23 / 24 / 25 the left thigh,
// calf, foot, toes (18 / 19 / 20 / 21 the right); 55 / 40 the middle fingers' first joints.
struct BoneMap {
  const char *gta;
  int disc;
  const char *gtaTo;
  int discTo;
};
const BoneMap kLeon[] = {
    {"root", 0, "", -1},
    {"pelvis", 17, "spine", 2},
    {"spine", 2, "spine1", 3},
    {"spine1", 3, "neck", 4},
    {"neck", 4, "head", 28},
    {"head", 4, "", -1},
    {"jaw", 27, "", -1},
    {"l brow", 4, "", -1},
    {"r brow", 4, "", -1},
    {"l clavicle", 11, "l upperarm", 13},
    {"l upperarm", 13, "l forearm", 14},
    {"l forearm", 14, "l hand", 16},
    {"l hand", 16, "l finger", 55},
    {"l finger", 16, "", -1},
    {"l finger01", 16, "", -1},
    {"r clavicle", 5, "r upperarm", 7},
    {"r upperarm", 7, "r forearm", 8},
    {"r forearm", 8, "r hand", 10},
    {"r hand", 10, "r finger", 40},
    {"r finger", 10, "", -1},
    {"r finger01", 10, "", -1},
    {"l breast", 3, "", -1},
    {"r breast", 3, "", -1},
    {"belly", 2, "", -1},
    {"l thigh", 22, "l calf", 23},
    {"l calf", 23, "l foot", 24},
    {"l foot", 24, "l toe0", 25},
    {"l toe0", 25, "", -1},
    {"r thigh", 18, "r calf", 19},
    {"r calf", 19, "r foot", 20},
    {"r foot", 20, "r toe0", 21},
    {"r toe0", 21, "", -1},
};

std::string gtaKey(std::string n) {
  n = lower(n);
  auto trim = [](std::string &s) {
    while (!s.empty() && s.back() == ' ') s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  };
  trim(n);
  if (n.rfind("bip01 ", 0) == 0) n = n.substr(6);
  trim(n);
  return n;
}

// A BIN of the disc folded into one point: every vertex on the first bone of its first weight entry; marked 0xF0 in
// byte 0x29
Bytes foldModel(const Bytes &model, const std::map<int, DiscBone> &skel) {
  Bytes m = model;
  const std::uint32_t wgt = be32(m, 0x14), vtx = be32(m, 0x30);
  const unsigned nv = be16(m, 0x38), shift = m.at(0x28);
  const bool big = be16(m, 0x2A) > 0xFF;
  const int bone = big ? int(be16(m, wgt)) : int(m.at(wgt));
  auto it = skel.find(bone);
  const V3 at = it != skel.end() ? it->second.at : V3{};
  const double s = double(1 << shift);
  auto q = [&](double v) { return unsigned(std::max(-32768, std::min(32767, int(std::lround(v * s)))) & 0xFFFF); };
  for (unsigned i = 0; i < nv; ++i) {
    const std::size_t p = vtx + std::size_t(i) * 8;
    const unsigned x = q(at.x), y = q(at.y), z = q(at.z);
    m.at(p) = std::uint8_t(x >> 8), m[p + 1] = std::uint8_t(x), m[p + 2] = std::uint8_t(y >> 8), m[p + 3] = std::uint8_t(y);
    m[p + 4] = std::uint8_t(z >> 8), m[p + 5] = std::uint8_t(z), m[p + 6] = 0, m[p + 7] = 0;
  }
  m.at(0x29) = 0xF0;  // folded: the game draws no weapon grip hand over the skin's own (runtime/skin_hands.cpp)
  return m;
}

// A BIN of the disc with only its part below `cut` (rest height, mm) folded away, each vertex there onto the first bone
// of its weight entry: Ashley's "outfit" model holds her hair and her skirt; a new outfit under her own head keeps the
// hair (no part spans the cut: the hair ends at her neck, the skirt at her hips)
Bytes foldBelow(const Bytes &model, const std::map<int, DiscBone> &skel, double cut) {
  Bytes m = model;
  const std::uint32_t wgt = be32(m, 0x14), vtx = be32(m, 0x30);
  const unsigned nv = be16(m, 0x38), shift = m.at(0x28);
  const bool big = be16(m, 0x2A) > 0xFF;
  const double s = double(1 << shift);
  auto q = [&](double v) { return unsigned(std::max(-32768, std::min(32767, int(std::lround(v * s)))) & 0xFFFF); };
  for (unsigned i = 0; i < nv; ++i) {
    const std::size_t p = vtx + std::size_t(i) * 8;
    if (double(std::int16_t(be16(m, p + 2))) / s >= cut) continue;
    const unsigned wi = be16(m, p + 6);
    const std::size_t e = wgt + std::size_t(wi) * (big ? 12 : 8);
    const int bone = e < m.size() ? (big ? int(be16(m, e)) : int(m.at(e))) : 0;
    auto it = skel.find(bone);
    const V3 at = it != skel.end() ? it->second.at : V3{};
    const unsigned x = q(at.x), y = q(at.y), z = q(at.z);
    m.at(p) = std::uint8_t(x >> 8), m[p + 1] = std::uint8_t(x), m[p + 2] = std::uint8_t(y >> 8), m[p + 3] = std::uint8_t(y);
    m[p + 4] = std::uint8_t(z >> 8), m[p + 5] = std::uint8_t(z);
  }
  return m;
}

// The disc's TPL with more images behind its own (offsets of the old ones moved by the longer table)
Bytes appendTpl(const Bytes &tpl, const std::vector<std::tuple<unsigned, unsigned, unsigned, Bytes>> &add) {
  if (tpl.size() < 12 || be32(tpl, 0) != 0x0020AF30) fail("the body's TPL is not one");
  const std::uint32_t n = be32(tpl, 4), table = be32(tpl, 8);
  if (table != 12) fail("the body's TPL has an unusual table");
  const std::size_t oldHead = 12 + std::size_t(n) * 8, newHead = 12 + std::size_t(n + add.size()) * 8;
  const std::size_t delta = align(newHead - oldHead, 32);
  Bytes out(oldHead + delta, 0);
  set32(out, 0, 0x0020AF30), set32(out, 4, std::uint32_t(n + add.size())), set32(out, 8, 12);
  out.insert(out.end(), tpl.begin() + long(oldHead), tpl.end());
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t ih = be32(tpl, 12 + i * 8), ph = be32(tpl, 16 + i * 8);
    set32(out, 12 + i * 8, ih ? std::uint32_t(ih + delta) : 0), set32(out, 16 + i * 8, ph ? std::uint32_t(ph + delta) : 0);
    if (ih) set32(out, ih + delta + 8, std::uint32_t(be32(tpl, ih + 8) + delta));
    if (ph) set32(out, ph + delta + 8, std::uint32_t(be32(tpl, ph + 8) + delta));
  }
  for (std::size_t k = 0; k < add.size(); ++k) {
    const auto &[w, h, gx, data] = add[k];
    pad(out);
    const std::size_t hd = out.size();
    out.resize(hd + 0x40, 0);
    const std::size_t at = align(out.size());
    out.resize(at, 0);
    set32(out, 12 + (n + k) * 8, std::uint32_t(hd)), set32(out, 16 + (n + k) * 8, 0);
    Bytes h2;
    put16(h2, h), put16(h2, w), put32(h2, gx), put32(h2, std::uint32_t(at)), put32(h2, 1), put32(h2, 1), put32(h2, 1),
        put32(h2, 1), putf(h2, 0.0f), put32(h2, 0);
    std::copy(h2.begin(), h2.end(), out.begin() + long(hd));
    out.insert(out.end(), data.begin(), data.end());
  }
  pad(out);
  return out;
}

// little-endian writers for the PC-layout model handed to convertModel
void le16w(Bytes &o, unsigned v) { o.push_back(std::uint8_t(v)), o.push_back(std::uint8_t(v >> 8)); }
void le32w(Bytes &o, std::uint32_t v) { le16w(o, v & 0xFFFF), le16w(o, v >> 16); }
void lefw(Bytes &o, float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  le32w(o, u);
}
void le32at(Bytes &o, std::size_t p, std::uint32_t v) {
  o[p] = std::uint8_t(v), o[p + 1] = std::uint8_t(v >> 8), o[p + 2] = std::uint8_t(v >> 16), o[p + 3] = std::uint8_t(v >> 24);
}
void le16at(Bytes &o, std::size_t p, unsigned v) { o[p] = std::uint8_t(v), o[p + 1] = std::uint8_t(v >> 8); }

// ---- Leon's own skinning, for the skin's vertices (weight transfer)
// The bones a transferred weight may name: his trunk, arms with their twists, legs; the others (the jaw, fingers,
// face, hair, the jacket's straps) give their weight to the nearest of these up their chain.
bool transferBone(int id) { return id >= 0 && id <= 25; }  // not his jaw (27): it talks in the cutscenes
// 0 trunk, 1 left arm, 2 right arm, 3 left leg, 4 right leg
int chainOf(int id) {
  if (id >= 11 && id <= 16) return 1;
  if (id >= 5 && id <= 10) return 2;
  if (id >= 22 && id <= 25) return 3;
  if (id >= 18 && id <= 21) return 4;
  return 0;
}
struct SrcVert {
  V3 p;
  std::vector<std::pair<int, double>> w;
  int chain;
};
void collectSkin(const Bytes &model, const std::map<int, DiscBone> &skel, std::vector<SrcVert> &out) {
  if (model.size() < 0x60) return;
  const unsigned shift = model[0x28], nv = be16(model, 0x38);
  const std::uint32_t vtx = be32(model, 0x30), wgt = be32(model, 0x14);
  const unsigned ext = be16(model, 0x2A), n = ext > 0xFF ? ext : model[0x18];
  const bool big = ext > 0xFF;
  auto up = [&](int id) {
    for (int hops = 0; !transferBone(id) && hops < 64; ++hops) {
      auto it = skel.find(id);
      if (it == skel.end() || it->second.parent < 0) return 0;
      id = it->second.parent;
    }
    return transferBone(id) ? id : 0;
  };
  std::vector<std::vector<std::pair<int, double>>> entries;
  for (unsigned k = 0; k < n; ++k) {
    const std::size_t e = wgt + std::size_t(k) * (big ? 12 : 8);
    const unsigned cnt = std::min<unsigned>(big ? be16(model, e + 6) : model.at(e + 3), 3);
    std::map<int, double> acc;
    for (unsigned j = 0; j < cnt; ++j) {
      const int id = big ? int(be16(model, e + j * 2)) : int(model.at(e + j));
      const double r = model.at(e + (big ? 8 : 4) + j);
      if (r > 0) acc[up(id)] += r;
    }
    entries.emplace_back(acc.begin(), acc.end());
  }
  const double s = 1.0 / double(1 << shift);
  for (unsigned i = 0; i < nv; ++i) {
    const std::size_t q = vtx + std::size_t(i) * 8;
    const unsigned wi = be16(model, q + 6);
    if (wi >= entries.size() || entries[wi].empty()) continue;
    SrcVert sv;
    sv.p = V3{double(std::int16_t(be16(model, q))) * s, double(std::int16_t(be16(model, q + 2))) * s,
              double(std::int16_t(be16(model, q + 4))) * s};
    sv.w = entries[wi];
    auto best = std::max_element(sv.w.begin(), sv.w.end(), [](const auto &a, const auto &b) { return a.second < b.second; });
    sv.chain = chainOf(best->first);
    out.push_back(std::move(sv));
  }
}
