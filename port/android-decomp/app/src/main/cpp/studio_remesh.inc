// CubeEvil4 Studio: RE4 Remake (RE Engine, PC) character mods onto Leon (wish of DevJunior, 20.11.2026: "Halo CE -
// Master Chief (Leon)", a Remake mod; "yes please" to reading such mods in the Studio). Included by
// studio_convert.cpp inside its anonymous namespace, after studio_dff.inc (DffMesh, kLeon, the GTA skins' way onto
// Leon, which these mods take too: their skeleton is the Remake's Leon, its bones renamed to a GTA ped's below).
//
// Read here (written from the file layouts, no code taken from other tools):
//  - .mesh.221108797 (the Remake's meshes): the header of that generation (names, the main mesh's LODs and groups,
//    the skeleton with its world matrices, the weighted bones' list), the first LOD of every group, the vertex
//    buffer by its elements (0 position 3 floats, 1 normal and tangent 2 x 4 signed bytes, 2 texture coordinates 2
//    halves, 4 weights 8 bone bytes + 8 weight bytes; the rest skipped), 16-bit triangle indices (32-bit too) relative
//    to each sub-mesh's first vertex. Streamed meshes (their data in a second file) are refused.
//  - .mdf2.32 (materials): per material its name and its textures by type; the colour map is the first of
//    BaseDielectricMap, BaseMetalMap, AlbedoMap, BaseAlphaMap, BaseMap (else one whose type says "Base" or "Albedo").
//  - .tex.143221013 (textures): the first level of the first image, BC1, BC2, BC3, BC7 (third_party/bcdec.h) or
//    8-bit RGBA, its rows padded to the file's scanline length; made opaque (the colour map's alpha says how metallic
//    it is). Without its colour map a material keeps its colour.
//
// Which meshes: those of Leon's own character folder cha0/cha000 (body, head pieces), else of the first folder
// with meshes; only meshes of at least 16 vertices (the mods empty the hair and the jacket with tiny meshes).

std::uint64_t le64(const Bytes &b, std::size_t p) { return std::uint64_t(le32(b, p)) | std::uint64_t(le32(b, p + 4)) << 32; }

float halfToFloat(std::uint16_t h) {
  const std::uint32_t s = std::uint32_t(h & 0x8000u) << 16, e = (h >> 10) & 31, m = h & 0x3FFu;
  std::uint32_t u;
  if (e == 0) {
    if (m == 0) {
      u = s;
    } else {  // a subnormal half: a normal float
      int ee = -1;
      std::uint32_t mm = m;
      do ++ee, mm <<= 1; while (!(mm & 0x400u));
      u = s | std::uint32_t(127 - 15 - ee) << 23 | (mm & 0x3FFu) << 13;
    }
  } else if (e == 31) {
    u = s | 0x7F800000u | m << 13;
  } else {
    u = s | (e + 127 - 15) << 23 | m << 13;
  }
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

std::string lowerSlash(std::string s) {
  for (auto &c : s) c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  while (!s.empty() && s[0] == '/') s.erase(0, 1);
  return s;
}

std::string utf16At(const Bytes &b, std::size_t p) {
  std::string s;
  for (; p + 1 < b.size(); p += 2) {
    const unsigned c = unsigned(b[p] | b[p + 1] << 8);
    if (!c) break;
    s.push_back(c < 0x80 ? char(c) : '?');
  }
  return s;
}

// ---- textures ----------------------------------------------------------------------------------------------------

Rgba readReTex(const Bytes &b) {
  if (b.size() < 56 || le32(b, 0) != 0x00584554u) fail("not an RE Engine texture (.tex)");
  const int w = le16(b, 8), h = le16(b, 10);
  const unsigned format = le32(b, 16);
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) fail(fmt(".tex: %dx%d?", w, h));
  const std::size_t at = std::size_t(le64(b, 40)), scan = le32(b, 48);
  int block = 0;  // bytes per 4x4 block, 0: 8-bit RGBA pixels
  switch (format) {
  case 71: case 72: block = 8; break;           // BC1
  case 74: case 75: case 77: case 78: block = 16; break;  // BC2, BC3
  case 98: case 99: block = 16; break;          // BC7
  case 28: case 29: block = 0; break;           // R8G8B8A8
  default: fail(fmt(".tex: format %u is not read here", format));
  }
  Rgba img;
  img.w = w, img.h = h;
  img.px.assign(std::size_t(w) * std::size_t(h) * 4, 255);
  if (block == 0) {
    const std::size_t line = scan ? scan : std::size_t(w) * 4;
    if (at + line * std::size_t(h) > b.size()) fail(".tex: truncated");
    for (int y = 0; y < h; ++y)
      std::memcpy(&img.px[std::size_t(y) * std::size_t(w) * 4], &b[at + std::size_t(y) * line], std::size_t(w) * 4);
    return img;
  }
  const int bw = (w + 3) / 4, bh = (h + 3) / 4;
  const std::size_t line = scan ? scan : std::size_t(bw) * std::size_t(block);
  if (line < std::size_t(bw) * std::size_t(block) || at + line * std::size_t(bh) > b.size()) fail(".tex: truncated");
  std::uint8_t out[4 * 4 * 4];
  for (int by = 0; by < bh; ++by)
    for (int bx = 0; bx < bw; ++bx) {
      const std::uint8_t *src = &b[at + std::size_t(by) * line + std::size_t(bx) * std::size_t(block)];
      switch (format) {
      case 71: case 72: bcdec_bc1(src, out, 16); break;
      case 74: case 75: bcdec_bc2(src, out, 16); break;
      case 77: case 78: bcdec_bc3(src, out, 16); break;
      default: bcdec_bc7(src, out, 16); break;
      }
      for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
          const int px = bx * 4 + x, py = by * 4 + y;
          if (px < w && py < h) std::memcpy(&img.px[(std::size_t(py) * std::size_t(w) + std::size_t(px)) * 4], &out[(y * 4 + x) * 4], 4);
        }
    }
  return img;
}

// ---- materials: name -> the colour map's path (lower case, '/', without ".tex") ------------------------------------

std::map<std::string, std::string> readMdf2(const Bytes &b) {
  std::map<std::string, std::string> out;
  if (b.size() < 16 || le32(b, 0) != 0x0046444Du) return out;  // "MDF\0"
  const unsigned count = le16(b, 6);
  static const char *const kColour[] = {"BaseDielectricMap", "BaseMetalMap", "AlbedoMap", "BaseAlphaMap", "BaseMap"};
  for (unsigned i = 0; i < count; ++i) {
    const std::size_t e = 16 + std::size_t(i) * 100;
    if (e + 100 > b.size()) break;
    const std::string name = utf16At(b, std::size_t(le64(b, e)));
    const unsigned textures = le32(b, e + 20);
    const std::size_t heads = std::size_t(le64(b, e + 60));
    std::string best;
    int rank = 99;
    for (unsigned t = 0; t < textures && heads + std::size_t(t + 1) * 32 <= b.size(); ++t) {
      const std::size_t h = heads + std::size_t(t) * 32;
      const std::string type = utf16At(b, std::size_t(le64(b, h))), path = utf16At(b, std::size_t(le64(b, h + 16)));
      if (path.empty() || path.find("systems/rendering/") != std::string::npos) continue;
      int r = 50;
      for (int k = 0; k < 5; ++k)
        if (type == kColour[k]) r = k;
      if (r == 50 && type.find("Base") == std::string::npos && type.find("Albedo") == std::string::npos) continue;
      if (r < rank) rank = r, best = path;
    }
    if (!best.empty()) {
      best = lowerSlash(best);
      if (best.size() > 4 && best.compare(best.size() - 4, 4, ".tex") == 0) best.resize(best.size() - 4);
      out[name] = best;
    }
  }
  return out;
}

// ---- meshes --------------------------------------------------------------------------------------------------------

struct ReMeshPart {
  std::vector<V3> pos, nrm;
  std::vector<std::array<double, 2>> uv;
  std::vector<std::array<unsigned, 4>> tris;  // a, b, c, the mesh's material
  std::vector<std::string> mats;              // the mesh's materials by name
  std::vector<std::array<std::uint8_t, 8>> bone;  // per vertex: skeleton bones
  std::vector<std::array<float, 8>> weight;
  std::vector<std::string> boneName;
  std::vector<int> boneParent;
  std::vector<V3> boneAt;
};

ReMeshPart readReMesh(const Bytes &b) {
  if (b.size() < 0xB0 || le32(b, 0) != 0x4853454Du) fail("not an RE Engine mesh (.mesh)");  // "MESH"
  const unsigned nameCount = le16(b, 20);
  // the offsets of the 2022-2023 generation (Street Fighter 6, RE4 Remake): from 0x20
  const std::size_t oGroups = le64(b, 0x20), oSkeleton = le64(b, 0x68), oMatRemap = le64(b, 0x70),
                    oBoneRemap = le64(b, 0x78), oStreaming = le64(b, 0x88), oNames = le64(b, 0x90), oMesh = le64(b, 0x48);
  if (oStreaming && le32(b, oStreaming) != 0) fail(".mesh: its data is streamed from a second file (not read here)");
  std::vector<std::string> names(nameCount);
  for (unsigned i = 0; i < nameCount; ++i) {
    const std::size_t at = std::size_t(le64(b, oNames + std::size_t(i) * 8));
    std::size_t e = at;
    while (e < b.size() && b[e]) ++e;
    names[i] = std::string(b.begin() + long(at), b.begin() + long(e));
  }
  ReMeshPart m;
  if (!oGroups || !oMesh) fail(".mesh: no main mesh");
  const unsigned lods = b[oGroups], materials = b[oGroups + 1], wide = b[oGroups + 6];
  if (!lods) fail(".mesh: no level of detail");
  for (unsigned i = 0; i < materials; ++i) m.mats.push_back(names.at(le16(b, oMatRemap + i * 2)));
  // the vertex buffer and its elements
  const std::size_t vElems = le64(b, oMesh), vBuf = le64(b, oMesh + 8);
  const std::size_t vSize = le32(b, oMesh + 28), nElems = le16(b, oMesh + 34);
  const std::size_t faceEnd = le32(b, oMesh + 36);  // the vertices and the triangles together
  const std::size_t fBuf = vBuf + vSize;
  std::size_t at[8] = {0};
  bool has[8] = {false};
  for (std::size_t i = 0; i < nElems; ++i) {
    const unsigned type = le16(b, vElems + i * 8);
    if (type < 8 && !has[type]) has[type] = true, at[type] = le32(b, vElems + i * 8 + 4);
  }
  if (!has[0]) fail(".mesh: no positions");
  const std::size_t total = ((has[1] && at[1] > at[0] ? at[1] : vSize) - at[0]) / 12;
  // the weighted bones: the skeleton's bones by the weights' numbers
  std::vector<int> weighted;
  if (oSkeleton) {
    const unsigned bones = le32(b, oSkeleton), remaps = le32(b, oSkeleton + 4);
    const std::size_t heads = le64(b, oSkeleton + 16), world = le64(b, oSkeleton + 32);
    for (unsigned i = 0; i < remaps; ++i) weighted.push_back(le16(b, oSkeleton + 48 + i * 2));
    weighted.push_back(0);  // one past the list: the root (some meshes weight it)
    for (unsigned i = 0; i < bones; ++i) {
      m.boneName.push_back(names.at(le16(b, oBoneRemap + i * 2)));
      m.boneParent.push_back(les16(b, heads + i * 16 + 2));
      const std::size_t w = world + std::size_t(i) * 64;
      m.boneAt.push_back({lef32(b, w + 48), lef32(b, w + 52), lef32(b, w + 56)});
    }
  }
  // every vertex the first LOD's groups use
  const std::size_t lod0 = le64(b, oGroups + 0x40);
  const unsigned groups = b[lod0];
  std::map<std::size_t, unsigned> taken;  // buffer vertex -> ours
  auto vertex = [&](std::size_t v) -> unsigned {
    auto it = taken.find(v);
    if (it != taken.end()) return it->second;
    if (v >= total) fail(".mesh: a triangle past the vertices");
    const unsigned k = unsigned(m.pos.size());
    taken.emplace(v, k);
    const std::size_t p = vBuf + at[0] + v * 12;
    m.pos.push_back({lef32(b, p), lef32(b, p + 4), lef32(b, p + 8)});
    if (has[1]) {
      const std::size_t q = vBuf + at[1] + v * 8;
      m.nrm.push_back(unit(V3{double(std::int8_t(b[q])) / 127.0, double(std::int8_t(b[q + 1])) / 127.0,
                              double(std::int8_t(b[q + 2])) / 127.0}));
    } else {
      m.nrm.push_back({0, 1, 0});
    }
    if (has[2]) {
      const std::size_t q = vBuf + at[2] + v * 4;
      m.uv.push_back({halfToFloat(le16(b, q)), halfToFloat(le16(b, q + 2))});
    } else {
      m.uv.push_back({0, 0});
    }
    std::array<std::uint8_t, 8> bi{};
    std::array<float, 8> bw{};
    if (has[4] && !weighted.empty()) {
      const std::size_t q = vBuf + at[4] + v * 16;
      for (int j = 0; j < 8; ++j) {
        const unsigned idx = b[q + std::size_t(j)];
        bi[std::size_t(j)] = std::uint8_t(idx < weighted.size() ? weighted[idx] : 0);
        bw[std::size_t(j)] = float(b[q + 8 + std::size_t(j)]) / 255.0f;
      }
    } else {
      bw[0] = 1;  // no weights: on the root
    }
    m.bone.push_back(bi), m.weight.push_back(bw);
    return k;
  };
  for (unsigned g = 0; g < groups; ++g) {
    const std::size_t go = le64(b, lod0 + 16 + g * 8);
    const unsigned subs = b[go + 1];
    for (unsigned s = 0; s < subs; ++s) {
      const std::size_t e = go + 16 + std::size_t(s) * 24;
      const unsigned mat = b[e], buffer = b[e + 2];
      const std::size_t faces = le32(b, e + 4), first = le32(b, e + 8), base = le32(b, e + 12);
      if (buffer != 0) continue;  // a streamed buffer
      for (std::size_t f = 0; f + 2 < faces; f += 3) {
        std::array<unsigned, 4> t{};
        for (int c = 0; c < 3; ++c) {
          const std::size_t i = first + f + std::size_t(c);
          const std::size_t idx = wide ? le32(b, fBuf + i * 4) : le16(b, fBuf + i * 2);
          if (fBuf + (i + 1) * (wide ? 4 : 2) > vBuf + faceEnd + 16) fail(".mesh: triangles past their buffer");
          t[std::size_t(c)] = vertex(base + idx);
        }
        t[3] = mat;
        m.tris.push_back(t);
      }
    }
  }
  return m;
}

// The Remake's bone names as a GTA ped's (kLeon): the bones the conversion turns and stretches; the rest keep their
// own names and move with their parent
const char *reBoneAsGta(const std::string &n) {
  static const std::pair<const char *, const char *> kMap[] = {
      {"Hip", "pelvis"},        {"Spine_1", "spine"},          {"Spine_2", "spine1"},         {"Neck_0", "neck"},
      {"Head", "head"},         {"L_Shoulder", "l clavicle"},  {"L_UpperArm", "l upperarm"},  {"L_Forearm", "l forearm"},
      {"L_Hand", "l hand"},     {"R_Shoulder", "r clavicle"},  {"R_UpperArm", "r upperarm"},  {"R_Forearm", "r forearm"},
      {"R_Hand", "r hand"},     {"L_Thigh", "l thigh"},        {"L_Shin", "l calf"},          {"L_Foot", "l foot"},
      {"L_Toe", "l toe0"},      {"R_Thigh", "r thigh"},        {"R_Shin", "r calf"},          {"R_Foot", "r foot"},
      {"R_Toe", "r toe0"},      {"root", "root"},
  };
  for (const auto &[re, gta] : kMap)
    if (n == re) return gta;
  return nullptr;
}

// The mod's files (their paths below natives/STM in lower case with '/', the variant's laid over the base) as one
// skinned mesh in the GTA skins' terms, and its textures
// The meshes of an RE4 Remake mod in one piece: a character's (`weapon` false: natives/.../character/ch/cha0/chaNNN,
// Leon's cha000 first) or a weapon's (true: .../character/wp/wpNN/wpNNNN; `want` that one, "" the first). A weapon
// keeps its muzzle bones (vfx_muzzle*, no vertex on them) for the placing.
DffMesh readReMod(const std::map<std::string, Bytes> &files, std::map<std::string, Rgba> &images,
                  std::vector<std::string> &notes, std::string &log, bool weapon = false, const std::string &want = "",
                  std::string *picked = nullptr) {
  // the meshes, by their character (weapon) folder
  std::map<std::string, std::vector<std::string>> byChar;
  for (const auto &[path, data] : files) {
    const auto dot = path.find(".mesh.");
    if (dot == std::string::npos) continue;
    if (!weapon) {
      // character/ch/chaN/chaNNN: Leon 0, Ashley 1, Ada 2
      const auto ch = path.find("character/ch/cha");
      if (ch != std::string::npos && path.size() > ch + 24 && path[ch + 17] == '/')
        byChar[path.substr(ch + 18, 6)].push_back(path);
    } else {
      const auto wp = path.find("character/wp/wp");
      if (wp != std::string::npos && path.size() > wp + 24 && path[wp + 17] == '/') byChar[path.substr(wp + 18, 6)].push_back(path);
    }
  }
  if (byChar.empty())
    fail(weapon ? "no weapon meshes of the RE4 Remake (natives/STM/_Chainsaw/Character/wp/wpNN/wpNNNN/...)"
                : "no character meshes of the RE4 Remake (natives/STM/_Chainsaw/Character/ch/chaN/...)");
  // `want`: a folder (wp4000, cha101), or a character's first letters (cha0 Leon, cha1 Ashley, cha2 Ada): its NN0 first
  std::string chr;
  if (byChar.count(want)) {
    chr = want;
  } else if (!want.empty()) {
    for (const auto &[key, list] : byChar)
      if (key.compare(0, want.size(), want) == 0 && (chr.empty() || key.compare(4, 2, "00") == 0)) chr = key;
    if (chr.empty() && !weapon) {  // another character's outfit on this one
      chr = byChar.count("cha000") ? "cha000" : byChar.begin()->first;
      log += "no outfit of " + want + "... in the mod: " + chr + " (another character's) taken\n";
    }
    if (chr.empty()) fail("the mod has no meshes of " + want);
  } else {
    chr = byChar.count("cha000") ? "cha000" : byChar.begin()->first;
  }
  if (picked) *picked = chr;
  DffMesh m;
  std::map<std::string, unsigned> boneOf;  // by the Remake's name
  std::map<std::string, unsigned> matOf;   // by the colour map's path, or the material's name without one
  std::vector<std::string> used;
  for (const auto &path : byChar.at(chr)) {
    ReMeshPart p;
    try {
      p = readReMesh(files.at(path));
    } catch (const std::exception &e) {
      notes.push_back("  " + path + ": " + e.what() + " (left out)");
      continue;
    }
    if (p.pos.size() < 16) continue;  // an emptied piece (hair, jacket)
    // its materials' colour maps: the .mdf2 beside it (the same name)
    std::map<std::string, std::string> colourOf;
    const std::string stem = path.substr(0, path.find(".mesh."));
    bool hasMdf2 = false;
    for (const auto &[fp, fd] : files)
      if (fp.compare(0, stem.size() + 6, stem + ".mdf2.") == 0) colourOf = readMdf2(fd), hasMdf2 = true;
    // a mesh the mod ships without its materials draws with the game's own textures (the Claire outfit's head is
    // Ashley's): left out, the character's own part stays
    // (Mods, 26.11.2026, DevJunior's "Hybrid face" head: cha000_10.mesh with its face pictures, no .mdf2) a head
    // mesh without materials but with its face picture beside it (<stem>_face_albd): the face and the mouth on it,
    // the eyes dark (their pictures are the game's), the brows' and lashes' cards and the eyes' wet and shade layers
    // left out (the face picture has the brows painted)
    std::set<std::string> dropMat;
    std::map<std::string, std::array<std::uint8_t, 4>> flatMat;
    if (!hasMdf2) {
      std::string face;
      for (const auto &[fp, fd] : files)
        if (fp.compare(0, stem.size() + 15, stem + "_face_albd.tex.") == 0) face = stem + "_face_albd";
      if (face.empty()) {
        log += "  " + path.substr(path.find_last_of('/') + 1) + ": no materials in the mod (the game's own), left out\n";
        continue;
      }
      for (const auto &name : p.mats) {
        std::string n = name;
        for (auto &ch : n) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (n.find("face") != std::string::npos || n.find("mouth") != std::string::npos ||
            n.find("skin") != std::string::npos || n.find("head") != std::string::npos)
          colourOf[name] = face;
        else if (n.find("eye_inside") != std::string::npos || n.find("eyeball") != std::string::npos ||
                 n.find("iris") != std::string::npos)
          flatMat[name] = {58, 52, 48, 255};
        else
          dropMat.insert(name);
      }
      log += "  " + path.substr(path.find_last_of('/') + 1) + ": no materials in the mod: the face picture " + face +
             " on the face and the mouth, the eyes dark, the other layers left out\n";
    }
    std::vector<unsigned> matMap;
    for (const auto &name : p.mats) {
      const std::string tex = colourOf.count(name) ? colourOf.at(name) : "";
      const std::string key = tex.empty() ? "material " + name : tex;
      auto it = matOf.find(key);
      if (it == matOf.end()) {
        it = matOf.emplace(key, unsigned(m.matTex.size())).first;
        m.matTex.push_back(key);
        m.matColour.push_back(flatMat.count(name) ? flatMat.at(name) : std::array<std::uint8_t, 4>{160, 160, 160, 255});
        m.matName.push_back(name);
        if (!tex.empty() && !images.count(key)) {
          bool found = false;
          for (const auto &[fp, fd] : files)
            if (fp.compare(0, tex.size() + 5, tex + ".tex.") == 0) {
              try {
                Rgba img = readReTex(fd);
                // the colour maps' alpha is no opacity (BaseDielectricMap: how metallic; the opacity is in the ATOC
                // map): opaque, or the blending of the downscale and the game would eat the armour (seen in the
                // Studio's view: a camouflage)
                for (std::size_t q = 3; q < img.px.size(); q += 4) img.px[q] = 255;
                images[key] = std::move(img);
                found = true;
              } catch (const std::exception &e) {
                notes.push_back("  " + fp + ": " + e.what());
              }
              break;
            }
          if (!found) notes.push_back("  " + name + ": its texture " + tex + " is not in the mod (grey)");
        }
      }
      matMap.push_back(it->second);
    }
    // its bones into the shared list (by name; the first mesh's place wins): those its vertices hang on, the ones
    // the conversion turns, and their parents (a head's face rig has hundreds no vertex uses)
    // a real face (Ashley's head: 231 bones) rigs its skin to hundreds of face bones the disc has no use for: those
    // under the head (not a bone the conversion knows) hand their vertices to the head
    if (!weapon) {
      int head = -1;
      for (std::size_t i = 0; i < p.boneName.size(); ++i)
        if (p.boneName[i] == "Head") head = int(i);
      if (head >= 0) {
        std::vector<int> to(p.boneName.size(), -1);
        for (std::size_t i = 0; i < p.boneName.size(); ++i) {
          if (int(i) == head || reBoneAsGta(p.boneName[i])) continue;
          for (int q = p.boneParent[i], hops = 0; q >= 0 && std::size_t(q) < p.boneName.size() && hops < 256;
               q = p.boneParent[std::size_t(q)], ++hops)
            if (q == head) {
              to[i] = head;
              break;
            }
        }
        std::size_t moved = 0;
        for (std::size_t v = 0; v < p.pos.size(); ++v) {
          auto &bv = p.bone[v];
          auto &wv = p.weight[v];
          for (std::size_t j = 0; j < 8; ++j)
            if (bv[j] < to.size() && to[bv[j]] >= 0 && wv[j] > 0)
              bv[j] = static_cast<std::remove_reference_t<decltype(bv[j])>>(to[bv[j]]), ++moved;
          for (std::size_t j = 0; j < 8; ++j)  // one weight per bone
            for (std::size_t k = j + 1; k < 8; ++k)
              if (wv[j] > 0 && wv[k] > 0 && bv[k] == bv[j]) wv[j] += wv[k], wv[k] = 0;
        }
        if (moved) log += fmt("  %s: %zu weights of face bones on the head\n", path.c_str(), moved);
      }
    }
    std::vector<char> need(p.boneName.size(), 0);
    for (std::size_t v = 0; v < p.pos.size(); ++v)
      for (int j = 0; j < 8; ++j)
        if (p.weight[v][std::size_t(j)] > 0 && p.bone[v][std::size_t(j)] < need.size()) need[p.bone[v][std::size_t(j)]] = 1;
    for (std::size_t i = 0; i < p.boneName.size(); ++i)
      if (weapon ? p.boneName[i].rfind("vfx_muzzle", 0) == 0 : reBoneAsGta(p.boneName[i]) != nullptr) need[i] = 1;
    for (std::size_t i = 0; i < p.boneName.size(); ++i)
      if (need[i])
        for (int q = p.boneParent[i], hops = 0; q >= 0 && std::size_t(q) < need.size() && hops < 256; q = p.boneParent[std::size_t(q)], ++hops)
          need[std::size_t(q)] = 1;
    std::vector<unsigned> boneMap(p.boneName.size(), 0);
    for (std::size_t i = 0; i < p.boneName.size(); ++i) {
      if (!need[i]) continue;
      const std::string &n = p.boneName[i];
      auto it = boneOf.find(n);
      if (it == boneOf.end()) {
        it = boneOf.emplace(n, unsigned(m.boneName.size())).first;
        const char *gta = weapon ? nullptr : reBoneAsGta(n);
        m.boneName.push_back(gta ? gta : n);
        m.boneAt.push_back(p.boneAt[i]);
        m.boneParent.push_back(-1);
      }
      boneMap[i] = it->second;
    }
    for (std::size_t i = 0; i < p.boneName.size(); ++i)
      if (need[i] && p.boneParent[i] >= 0 && std::size_t(p.boneParent[i]) < p.boneName.size() && m.boneParent[boneMap[i]] < 0 &&
          boneMap[std::size_t(p.boneParent[i])] != boneMap[i])
        m.boneParent[boneMap[i]] = int(boneMap[std::size_t(p.boneParent[i])]);
    if (m.boneName.size() > 255) fail("more than 255 bones in the mod's meshes");
    // its vertices and triangles
    const unsigned first = unsigned(m.pos.size());
    for (std::size_t v = 0; v < p.pos.size(); ++v) {
      m.pos.push_back(p.pos[v]), m.nrm.push_back(p.nrm[v]), m.uv.push_back(p.uv[v]);
      // the 4 heaviest of its 8 weights
      std::array<int, 8> order{0, 1, 2, 3, 4, 5, 6, 7};
      std::stable_sort(order.begin(), order.end(), [&](int a, int c) { return p.weight[v][std::size_t(a)] > p.weight[v][std::size_t(c)]; });
      std::array<std::uint8_t, 4> bi{};
      std::array<float, 4> bw{};
      float sum = 0;
      for (int j = 0; j < 4; ++j) {
        const int k = order[std::size_t(j)];
        const unsigned sb = p.bone[v][std::size_t(k)];
        bi[std::size_t(j)] = std::uint8_t(sb < boneMap.size() ? boneMap[sb] : 0);
        bw[std::size_t(j)] = p.weight[v][std::size_t(k)];
        sum += bw[std::size_t(j)];
      }
      if (sum <= 0) bw = {1, 0, 0, 0}, sum = 1;
      for (auto &x : bw) x /= sum;
      m.bone.push_back(bi), m.weight.push_back(bw);
    }
    for (const auto &t : p.tris)
      if (t[3] >= p.mats.size() || !dropMat.count(p.mats[t[3]]))
        m.tris.push_back({t[0] + first, t[1] + first, t[2] + first, matMap.at(t[3])});
    used.push_back(fmt("%s (%zu vertices, %zu triangles)", path.substr(path.find_last_of('/') + 1).c_str(), p.pos.size(),
                       p.tris.size()));
  }
  if (m.pos.empty()) fail("the mod's meshes of " + chr + " are empty");
  std::string list;
  for (const auto &u : used) list += (list.empty() ? "" : ", ") + u;
  log += std::string("RE4 Remake mod, ") + (weapon ? "weapon " : "character ") + chr + ": " + list + "\n";
  return m;
}
