// CubeEvil4 Studio: archives, the model, its textures, the animation's pose and the CPU skinning (studio_model.h)
#include "studio_model.h"
#include "scene.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace re4::studio {
namespace {

std::uint32_t be32(const std::vector<std::uint8_t> &b, std::size_t p) {
  if (p > b.size() || b.size() - p < 4) throw std::runtime_error("truncated");
  return std::uint32_t(b[p]) << 24 | std::uint32_t(b[p + 1]) << 16 | std::uint32_t(b[p + 2]) << 8 | b[p + 3];
}

bool readFile(const std::string &path, std::vector<std::uint8_t> &out) {
  return ReadWhole(path, out, 96u * 1024 * 1024);
}

}  // namespace

// A file of the game app (StudioFiles: /proc/self/fd/N) is read through the descriptor the provider handed over:
// opening the link again would ask for the game app's own rights to the file (refused to the Studio's APK)
bool ReadWhole(const std::string &path, std::vector<std::uint8_t> &out, std::size_t limit) {
  static const char kFd[] = "/proc/self/fd/";
  if (path.compare(0, sizeof kFd - 1, kFd) == 0) {
    char *end = nullptr;
    const long fd = std::strtol(path.c_str() + sizeof kFd - 1, &end, 10);
    struct stat st {};
    if (end && !*end && fd >= 0 && fstat(int(fd), &st) == 0 && S_ISREG(st.st_mode)) {
      if (st.st_size <= 0 || std::uint64_t(st.st_size) > limit) return false;
      out.resize(std::size_t(st.st_size));
      std::size_t got = 0;
      while (got < out.size()) {
        const ssize_t n = pread(int(fd), out.data() + got, out.size() - got, off_t(got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        got += std::size_t(n);
      }
      return true;
    }
  }
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const auto n = in.tellg();
  if (n <= 0 || std::uint64_t(n) > limit) return false;
  out.resize(std::size_t(n));
  in.seekg(0);
  return bool(in.read(reinterpret_cast<char *>(out.data()), n));
}

bool Archive::open(const std::string &path, std::string *why, const std::string &name) {
  path_ = path;
  name_ = name;
  if (name_.empty()) {
    const auto slash = path.find_last_of('/');
    name_ = slash == std::string::npos ? path : path.substr(slash + 1);
  }
  body_.clear();
  entries_.clear();
  offsets_.clear();
  std::vector<std::uint8_t> file;
  if (!readFile(path, file)) {
    if (why) *why = "cannot be read";
    return false;
  }
  try {
    // a disc archive: the part table (32-byte records from 0x20), part 0 the data archive; else a DAT body
    std::size_t at = 0, len = file.size();
    if (file.size() >= 0x440 && be32(file, 0x20) == 0) {
      const auto l = be32(file, 0x24), o = be32(file, 0x2C);
      if (o >= 0x400 && l >= 16 && o <= file.size() && l <= file.size() - o) at = o, len = l;
    }
    body_.assign(file.begin() + long(at), file.begin() + long(at + len));
    if (body_.size() >= 16 && std::isxdigit(body_[0]) && std::memchr(body_.data(), '\t', 16)) {
      if (why) *why = "packed (Yz2), not read here";
      return false;
    }
    const auto n = be32(body_, 0);
    if (n == 0 || n > 4096 || 16 + std::size_t(n) * 8 > body_.size()) {
      if (why) *why = "no archive";
      return false;
    }
    std::size_t rel = be32(body_, 4);
    if (rel < 16 + std::size_t(n) * 8 || rel >= body_.size() || rel % 32) rel = body_.size();
    for (unsigned i = 0; i < n; ++i) offsets_.push_back(be32(body_, 16 + i * 4));
    for (unsigned i = 0; i < n; ++i) {
      Entry e;
      e.index = i;
      const std::size_t start = offsets_[i], stop = i + 1 < n ? offsets_[i + 1] : rel;
      e.bytes = start && start < stop && stop <= body_.size() ? stop - start : 0;
      char kind[5] = {};
      std::memcpy(kind, body_.data() + 16 + std::size_t(n) * 4 + i * 4, 4);
      for (char &c : kind)
        if (c && (c < 32 || c > 126)) c = '?';
      e.kind = kind;
      while (!e.kind.empty() && (e.kind.back() == ' ' || e.kind.back() == 0)) e.kind.pop_back();
      entries_.push_back(e);
    }
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    return false;
  }
  return true;
}

std::vector<std::uint8_t> Archive::entry(unsigned index) const {
  if (index >= entries_.size() || !entries_[index].bytes) return {};
  const auto start = offsets_[index];
  return {body_.begin() + long(start), body_.begin() + long(start + entries_[index].bytes)};
}

std::vector<unsigned> Archive::of(const char *kind) const {
  std::vector<unsigned> out;
  for (const auto &e : entries_)
    if (e.bytes && e.kind.compare(0, 3, kind) == 0) out.push_back(e.index);
  return out;
}

int Archive::paletteFor(unsigned bin) const {
  for (unsigned i = bin + 1; i < entries_.size(); ++i)
    if (entries_[i].bytes && entries_[i].kind.compare(0, 3, "TPL") == 0) return int(i);
  for (unsigned i = bin; i-- > 0;)
    if (entries_[i].bytes && entries_[i].kind.compare(0, 3, "TPL") == 0) return int(i);
  return -1;
}

namespace {

// the game's own (BIN, TPL) pairs, as its PL_ARC indices (entry + 4: the archive's header words come first)
struct GamePart {
  int bin, tpl;
  const char *label;
};
struct Character {
  const char *files;  // the archives that use it, space separated
  std::vector<GamePart> parts;
};

const std::vector<Character> &Characters() {
  // pl_leon.cpp cPlLeon::setModel (costume 0: pl00; 1..3: pl08, pl09, pl10 with 0x10), setRightHand(0) 0x12,
  // setLeftHand(1) 0x15; pl_ashley.cpp cPlAshley (pl01, pl05: right hand 0 0x11, left 0 0x14); pl02.cpp cPlAda (pl0b,
  // pl0c: right 0 0x11, left 0 0x12); pl06.cpp cPlHunk (right 0 0x12, left 1 0x15); pl_klauser.cpp cPlKlauser (pl0a:
  // the knife 0xE hidden; right 0 0x12, left 1 0x15); pl_wesker.cpp cPlWesker (pl0d: right 0 0x12, left 0 0x14);
  // pl11.cpp cSubAshley (the partner: right 0xC, left 0xF)
  static const std::vector<Character> all = {
      {"pl00", {{4, 5, "Body"}, {0xA, 5, "Outfit"}, {8, 7, "Face"}, {6, 7, "Head"}, {9, 7, "Hair"},
                {0x12, 0x11, "Right hand"}, {0x15, 0x11, "Left hand"}}},
      {"pl08 pl09 pl10", {{4, 5, "Body"}, {0xA, 5, "Outfit"}, {0x10, 5, "Costume"}, {8, 7, "Face"}, {6, 7, "Head"},
                          {9, 7, "Hair"}, {0x12, 0x11, "Right hand"}, {0x15, 0x11, "Left hand"}}},
      {"pl01 pl05", {{4, 5, "Body"}, {7, 9, "Face"}, {6, 0xB, "Head"}, {8, 5, "Outfit"}, {0xA, 5, "Outfit 2"},
                     {0x11, 5, "Right hand"}, {0x14, 5, "Left hand"}}},
      {"pl11", {{4, 5, "Body"}, {7, 0xB, "Face"}, {6, 8, "Head"}, {9, 5, "Outfit"}, {0xA, 5, "Outfit 2"},
                {0xC, 5, "Right hand"}, {0xF, 5, "Left hand"}}},
      {"pl0b pl0c", {{4, 5, "Body"}, {6, 7, "Head"}, {8, 7, "Face"}, {9, 0xA, "Outfit"}, {0x11, 5, "Right hand"},
                     {0x12, 5, "Left hand"}}},
      {"pl06", {{4, 5, "Body"}, {6, 7, "Head"}, {0x12, 0x11, "Right hand"}, {0x15, 0x11, "Left hand"}}},
      {"pl0a", {{4, 5, "Body"}, {6, 7, "Face"}, {8, 9, "Head"}, {0xA, 0xB, "Outfit"}, {0xF, 0x10, "Outfit 2"},
                {0x18, 0x19, "Outfit 3"}, {0x12, 0x11, "Right hand"}, {0x15, 0x11, "Left hand"}}},
      {"pl0d", {{4, 5, "Body"}, {0xA, 5, "Outfit"}, {8, 7, "Face"}, {6, 7, "Head"}, {0x12, 0x11, "Right hand"},
                {0x14, 0x11, "Left hand"}}},
  };
  return all;
}

bool kindIs(const Archive &archive, int index, const char *kind) {
  return index >= 0 && std::size_t(index) < archive.entries().size() && archive.entries()[std::size_t(index)].bytes &&
         archive.entries()[std::size_t(index)].kind.compare(0, 3, kind) == 0;
}

}  // namespace

std::vector<PartSpec> GameParts(const std::string &fileName, const Archive &archive) {
  std::string stem = fileName;
  if (auto slash = stem.find_last_of("/\\"); slash != std::string::npos) stem = stem.substr(slash + 1);
  if (auto dot = stem.find('.'); dot != std::string::npos) stem = stem.substr(0, dot);
  for (auto &c : stem) c = char(std::tolower(static_cast<unsigned char>(c)));
  std::vector<PartSpec> out;
  if (stem.size() != 4) return out;
  for (const auto &character : Characters()) {
    const std::string files = std::string(" ") + character.files + " ";
    if (files.find(" " + stem + " ") == std::string::npos) continue;
    for (const auto &p : character.parts) {
      const int bin = p.bin - 4, tpl = p.tpl - 4;
      if (!kindIs(archive, bin, "BIN")) continue;
      out.push_back({unsigned(bin), kindIs(archive, tpl, "TPL") ? tpl : -1, p.label});
    }
    break;
  }
  return out;
}

Viewer::Viewer() = default;
Viewer::~Viewer() = default;

bool Viewer::buildPiece(const Archive &archive, unsigned bin, int tpl, Piece &piece, std::string *why) {
  try {
    auto bytes = archive.entry(bin);
    if (bytes.empty()) throw std::runtime_error("empty entry");
    piece.model = std::make_unique<SkinnedModel>(bytes);
    const auto &m = piece.model->mesh;
    // the palette's images (a model's material says which: its header byte 12)
    if (tpl < 0) tpl = archive.paletteFor(bin);
    const int textureBase = int(textures_.size());
    unsigned images = 0;
    if (tpl >= 0) {
      const auto palette = archive.entry(unsigned(tpl));
      images = palette.size() >= 8 ? be32(palette, 4) : 0;
      for (unsigned i = 0; i < images && i < 256; ++i) {
        try {
          textures_.push_back(DecodeTpl(palette, i));
        } catch (const std::exception &) {
          textures_.push_back(Texture{});
        }
      }
    }
    for (const auto &primitive : m.primitives) {
      if (primitive.triangles.empty()) continue;
      const auto header = m.materialHeaders.empty() ? std::array<std::uint8_t, 32>{}
                                                    : m.materialHeaders.at(primitive.displayList);
      Draw d;
      d.first = piece.cornerPosition.size() / 3;
      d.count = primitive.triangles.size();
      const unsigned image = header[12];
      d.texture = image < images && !textures_[std::size_t(textureBase) + image].rgba.empty()
                      ? textureBase + int(image) : -1;
      d.alphaTest = (header[11] & 4) != 0;
      d.piece = int(pieces_.size());
      for (const auto &triangle : primitive.triangles)
        for (auto corner : triangle) {
          const auto &ref = primitive.vertices.at(corner);
          piece.cornerPosition.push_back(ref.position);
          piece.cornerNormal.push_back(ref.normal);
          const auto uv = m.texcoords.empty() ? std::array<float, 2>{} : m.texcoords.at(ref.texcoord);
          piece.uvs.push_back(uv[0]);
          piece.uvs.push_back(uv[1]);
        }
      piece.draws.push_back(d);
    }
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    return false;
  }
  return true;
}

// the draws and uvs of all pieces in one list (the corners of piece k after those of pieces 0..k-1)
void Viewer::rebuild() {
  draws_.clear();
  uvs_.clear();
  std::size_t base = 0;
  for (std::size_t k = 0; k < pieces_.size(); ++k) {
    const auto &p = pieces_[k];
    for (auto d : p.draws) {
      d.first += base;
      d.piece = int(k);
      draws_.push_back(d);
    }
    uvs_.insert(uvs_.end(), p.uvs.begin(), p.uvs.end());
    base += p.cornerPosition.size() / 3;
  }
}

bool Viewer::loadModel(const Archive &archive, unsigned bin, int tpl, std::string *why) {
  rig_.reset();
  pieces_.clear();
  pieceNames_.clear();
  pieceKeys_.clear();
  model_ = nullptr;
  textures_.clear();
  clip_ = -1;
  highlight_ = -1;
  Piece piece;
  if (!buildPiece(archive, bin, tpl, piece, why)) return false;
  try {
    parents_.clear();
    for (const auto &j : piece.model->mesh.joints) parents_.push_back(j.parent);
    rig_ = std::make_unique<AnimationRig>(*piece.model);
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    textures_.clear();
    return false;
  }
  model_ = piece.model.get();
  pieces_.push_back(std::move(piece));
  pieceKeys_.push_back({archive.path(), bin});
  pieceNames_.push_back("#" + std::to_string(bin));
  rebuild();
  skin(rig_->bones());
  restMinimum_ = minimum_;
  restMaximum_ = maximum_;
  return true;
}

bool Viewer::togglePiece(const Archive &archive, unsigned bin, int tpl, bool *added, std::string *why,
                         const std::string &label) {
  if (!model_) return false;
  for (std::size_t i = 1; i < pieces_.size(); ++i)
    if (pieceKeys_[i].first == archive.path() && pieceKeys_[i].second == bin) {
      pieces_.erase(pieces_.begin() + long(i));
      pieceKeys_.erase(pieceKeys_.begin() + long(i));
      pieceNames_.erase(pieceNames_.begin() + long(i));
      highlight_ = -1;
      if (added) *added = false;
      rebuild();
      skin(rig_->bones());
      return true;
    }
  const std::size_t texturesBefore = textures_.size();
  Piece piece;
  if (!buildPiece(archive, bin, tpl, piece, why)) {
    textures_.resize(texturesBefore);
    return false;
  }
  try {
    piece.model->validateSkeleton(model_->mesh.joints.size());
  } catch (const std::exception &) {
    if (why) *why = "it belongs to another skeleton";
    textures_.resize(texturesBefore);
    return false;
  }
  pieces_.push_back(std::move(piece));
  pieceKeys_.push_back({archive.path(), bin});
  pieceNames_.push_back(label.empty() ? "#" + std::to_string(bin) : label);
  if (added) *added = true;
  rebuild();
  skin(rig_->bones());
  return true;
}

int Viewer::addMotions(const Archive &archive, std::string *why) {
  int added = 0;
  const auto fcv = archive.of("FCV");
  const std::string base = archive.name();
  for (unsigned i : fcv) {
    AnimationBytes b;
    b.motion = archive.entry(i);
    // an empty place of the archive (all zero: no frames) is no animation (DevJunior's device test 19.11.2026:
    // "pl08.drs #23: unsupported preview frame range")
    if (b.motion.size() < 4 || !((unsigned(b.motion[0]) << 8 | b.motion[1]) & 0x3FFF)) continue;
    // its sequence: the SEQ right after it, if any (the events: steps, sounds)
    if (i + 1 < archive.entries().size() && archive.entries()[i + 1].kind.compare(0, 3, "SEQ") == 0)
      b.sequence = archive.entry(i + 1);
    clips_.push_back(std::move(b));
    names_.push_back(base + " #" + std::to_string(i));
    ++added;
  }
  if (!added && why) *why = "no animations in it";
  return added;
}

void Viewer::clearMotions() {
  clips_.clear();
  names_.clear();
  clip_ = -1;
  if (model_) {
    rig_ = std::make_unique<AnimationRig>(*model_);
    skin(rig_->bones());
  }
}

bool Viewer::setMotion(int clip, std::string *why) {
  if (!model_) return false;
  try {
    if (clip < 0 || clip >= int(clips_.size())) {
      rig_ = std::make_unique<AnimationRig>(*model_);
      clip_ = -1;
      skin(rig_->bones());
      return clip < 0;
    }
    std::unique_ptr<AnimationRig> rig;
    try {
      rig = std::make_unique<AnimationRig>(*model_, std::vector<AnimationBytes>{clips_[std::size_t(clip)]});
    } catch (const std::exception &) {
      // its events (the SEQ entry after it) may belong to another animation: the motion alone (the viewer needs no
      // footsteps or sounds)
      if (clips_[std::size_t(clip)].sequence.empty()) throw;
      AnimationBytes alone = clips_[std::size_t(clip)];
      alone.sequence.clear();
      rig = std::make_unique<AnimationRig>(*model_, std::vector<AnimationBytes>{alone});
    }
    rig->sample(0, 0);
    rig_ = std::move(rig);
    clip_ = clip;
    skin(rig_->bones());
  } catch (const std::exception &e) {
    if (why) *why = e.what();
    rig_ = std::make_unique<AnimationRig>(*model_);
    clip_ = -1;
    skin(rig_->bones());
    return false;
  }
  return true;
}

// (the frame count is maxFrame's low 14 bits; the top ones are flags, as MotionSetCore reads it)
float Viewer::motionLength() const {
  return rig_ && clip_ >= 0 ? float(unsigned(rig_->maxFrame(0)) & 0x3FFF) : 0.f;
}

bool Viewer::loadParts(const Archive &archive, const std::vector<PartSpec> &specs, std::string *why) {
  if (specs.empty()) {
    if (why) *why = "no parts";
    return false;
  }
  if (!loadModel(archive, specs[0].bin, specs[0].tpl, why)) return false;
  if (!specs[0].label.empty()) pieceNames_[0] = specs[0].label;
  for (std::size_t i = 1; i < specs.size(); ++i) {
    bool added = false;
    std::string ignored;
    togglePiece(archive, specs[i].bin, specs[i].tpl, &added, &ignored, specs[i].label);
  }
  restMinimum_ = minimum_;
  restMaximum_ = maximum_;
  return true;
}

std::vector<unsigned> Viewer::pieceEntries() const {
  std::vector<unsigned> out;
  for (const auto &k : pieceKeys_) out.push_back(k.second);
  return out;
}

void Viewer::replaceMotion(int clip, const std::vector<std::uint8_t> &motion) {
  if (clip < 0 || clip >= int(clips_.size())) return;
  clips_[std::size_t(clip)].motion = motion;
  if (clip == clip_) setMotion(clip, nullptr);
}

void Viewer::poseEdited(float frame, const std::function<void(std::vector<Matrix34> &)> &edit) {
  if (!rig_) return;
  if (clip_ >= 0) {
    frame = std::isfinite(frame) ? std::clamp(frame, 0.f, motionLength()) : 0.f;
    try {
      rig_->sample(0, frame);
    } catch (const std::exception &) {
      return;
    }
  }
  std::vector<Matrix34> bones = rig_->bones();
  edit(bones);
  skin(bones);
}

bool Viewer::attachWeapon(const Archive &archive, unsigned bin, int tpl, int part, std::string *why) {
  if (!model_) {
    if (why) *why = "no model";
    return false;
  }
  detachWeapon();
  Piece piece;
  piece.textureBase = textures_.size();
  if (!buildPiece(archive, bin, tpl, piece, why)) {
    textures_.resize(piece.textureBase);
    return false;
  }
  piece.attach = part;
  pieces_.push_back(std::move(piece));
  pieceKeys_.push_back({archive.path(), bin});
  pieceNames_.push_back("weapon");
  rebuild();
  if (rig_) skin(rig_->bones());
  return true;
}

void Viewer::detachWeapon() {
  while (!pieces_.empty() && pieces_.back().attach >= 0) {
    textures_.resize(pieces_.back().textureBase);
    pieces_.pop_back();
    pieceKeys_.pop_back();
    pieceNames_.pop_back();
  }
  rebuild();
  if (rig_) skin(rig_->bones());
}

void Viewer::pose(float frame) {
  if (!rig_) return;
  if (clip_ >= 0) {
    const float last = motionLength();
    frame = std::isfinite(frame) ? std::clamp(frame, 0.f, last) : 0.f;
    try {
      rig_->sample(0, frame);
    } catch (const std::exception &) {
      return;
    }
  }
  skin(rig_->bones());
}

void Viewer::skin(const std::vector<Matrix34> &input) {
  if (!model_) return;
  std::vector<Matrix34> bones = input;
  if (inPlace_ && !bones.empty() && clip_ >= 0) {
    // the root's travel on the floor taken off every part (the model stays over the grid's centre)
    const float dx = bones[0][3] - rig_->bindCenters()[0][0], dz = bones[0][11] - rig_->bindCenters()[0][2];
    for (auto &b : bones) b[3] -= dx, b[11] -= dz;
  }
  std::size_t corners = 0;
  for (const auto &p : pieces_) corners += p.cornerPosition.size();
  positions_.resize(corners * 3);
  normals_.resize(corners * 3);
  bool first = true;
  std::size_t c = 0;
  for (const auto &piece : pieces_) {
    const auto &model = *piece.model;
    const auto &m = model.mesh;
    std::vector<Matrix34> palette;
    try {
      if (piece.attach >= 0) {
        // a weapon in the hand: every joint of its own on the hand's part (its vertices in its own space)
        if (std::size_t(piece.attach) >= bones.size()) throw std::runtime_error("no such part");
        const std::size_t n = m.joints.size();
        palette = model.palette(std::vector<Matrix34>(n, bones[std::size_t(piece.attach)]),
                                std::vector<std::array<float, 3>>(n, std::array<float, 3>{0, 0, 0}));
      } else {
        palette = model.palette(bones, rig_->bindCenters());
      }
    } catch (const std::exception &) {
      c += piece.cornerPosition.size();
      continue;
    }
    std::vector<std::array<float, 3>> pos(m.positions.size()), nor(m.normals.size());
    for (std::size_t i = 0; i < m.positions.size(); ++i)
      pos[i] = TransformPoint(palette[model.positionWeights[i]], m.positions[i]);
    for (std::size_t i = 0; i < m.normals.size(); ++i) {
      const auto &p = palette[model.normalWeights[i]];
      const auto &n = m.normals[i];
      std::array<float, 3> r{p[0] * n[0] + p[1] * n[1] + p[2] * n[2], p[4] * n[0] + p[5] * n[1] + p[6] * n[2],
                             p[8] * n[0] + p[9] * n[1] + p[10] * n[2]};
      const float l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
      if (l > 1e-6f) r = {r[0] / l, r[1] / l, r[2] / l};
      nor[i] = r;
    }
    for (std::size_t k = 0; k < piece.cornerPosition.size(); ++k, ++c) {
      const auto &p = pos.at(piece.cornerPosition[k]);
      const auto &n = piece.cornerNormal[k] < nor.size() ? nor[piece.cornerNormal[k]] : std::array<float, 3>{0, 1, 0};
      for (unsigned a = 0; a < 3; ++a) {
        positions_[c * 3 + a] = p[a];
        normals_[c * 3 + a] = n[a];
        if (first) minimum_[a] = maximum_[a] = p[a];
        minimum_[a] = std::min(minimum_[a], p[a]);
        maximum_[a] = std::max(maximum_[a], p[a]);
      }
      first = false;
    }
  }
  jointPositions_.resize(bones.size());
  for (std::size_t i = 0; i < bones.size(); ++i) jointPositions_[i] = {bones[i][3], bones[i][7], bones[i][11]};
}

}  // namespace re4::studio
