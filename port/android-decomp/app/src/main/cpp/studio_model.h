#pragma once
// CubeEvil4 Studio (Mods, 19.11.2026; port/docs/studio-plan.md, DevJunior's choice: an app of its own without the
// game running, a model viewer with the animations first): one model of a disc archive (or of a mod pack's archive)
// with its texture palette, posed by an animation (the original MotionMove, AnimationRig) and skinned on the CPU for
// the viewer (studio_gl.cpp). Reads the files directly; nothing of the game core runs.
#include "animation.h"
#include "model_skin.h"
#include "texture.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace re4::studio {

// The whole file (at most `limit` bytes); /proc/self/fd/N (a file of the game app) through that descriptor
bool ReadWhole(const std::string &path, std::vector<std::uint8_t> &out, std::size_t limit);

struct Entry {
  unsigned index{};
  std::string kind;      // "BIN", "TPL", "FCV", "SEQ" ... (the archive's tag)
  std::size_t bytes{};   // 0: an empty entry
};

// One draw of the skinned mesh: triangles [first, first + count) of the vertex arrays, one texture image
struct Draw {
  std::size_t first{}, count{};
  int texture = -1;  // index into textures(), -1 none (untextured)
  bool alphaTest{};
  int piece = 0;     // which piece of the model it belongs to (the highlight)
};

// A part the game puts on a character: a BIN entry, its palette (TPL entry) and what it is
struct PartSpec {
  unsigned bin{};
  int tpl = -1;
  std::string label;
};

// An archive opened for the Studio: a disc archive with its part table (.drs, .das, .dat with one) or a DAT body
// (a mod pack's melee/*.dat)
class Archive {
public:
  // `name`: the file's own name when `path` does not carry it (a file the CubeEvil4 app hands over, /proc/self/fd/N)
  bool open(const std::string &path, std::string *why, const std::string &name = "");
  const std::string &name() const { return name_; }
  const std::vector<Entry> &entries() const { return entries_; }
  std::vector<std::uint8_t> entry(unsigned index) const;
  const std::string &path() const { return path_; }
  // the entries of a kind, in order
  std::vector<unsigned> of(const char *kind) const;
  // the texture palette a model uses: the next TPL after it, else the last one before it (-1 none)
  int paletteFor(unsigned bin) const;

private:
  std::string path_, name_;
  std::vector<std::uint8_t> body_;
  std::vector<Entry> entries_;
  std::vector<std::size_t> offsets_;
};

// The parts the game itself puts together for a character (its setModel: cPlLeon, cPlAshley, cPlAda, cPlHunk,
// cPlKlauser, cPlWesker, cSubAshley), by the archive's file name (pl00.drs ...), the body first; each with the palette
// the game gives it (Mods, 19.11.2026, DevJunior's device test: "the models do not load complete", "texture errors":
// the next TPL after a BIN is not always its own, Leon's face took the wound's). Empty: a file it does not know.
std::vector<PartSpec> GameParts(const std::string &fileName, const Archive &archive);

class Viewer {
public:
  Viewer();
  ~Viewer();
  // the model (a BIN entry) with the palette (a TPL entry, -1 the archive's guess): its skeleton is the one posed
  bool loadModel(const Archive &archive, unsigned bin, int tpl, std::string *why);
  bool hasModel() const { return model_ != nullptr; }
  // another model on the same skeleton (a head, hands, a jacket: its weights name the first model's joints); false
  // when it does not fit. Taken off again by a second call (toggle); `added` says which.
  bool togglePiece(const Archive &archive, unsigned bin, int tpl, bool *added, std::string *why,
                   const std::string &label = "");
  // the whole model as the game puts it together: specs[0] the main model, the others pieces on its skeleton (those
  // that do not fit are left out); false when the main model cannot be read
  bool loadParts(const Archive &archive, const std::vector<PartSpec> &specs, std::string *why);
  // the BIN entries shown (the main model first) and their names
  std::vector<unsigned> pieceEntries() const;
  // one piece drawn tinted (the one the list points at); -1 none
  void setHighlight(int piece) { highlight_ = piece; }
  int highlight() const { return highlight_; }
  // the entries shown: the first model's, then the pieces'
  std::vector<std::string> pieces() const { return pieceNames_; }
  // the animations: FCV entries of an archive (the model's own or a pack's); returns how many were added
  int addMotions(const Archive &archive, std::string *why);
  void clearMotions();
  // the animation editor (Mods, 26.11.2026): a clip's bytes replaced (an edit: the rig made again only for it, not
  // the whole archive read again)
  void replaceMotion(int clip, const std::vector<std::uint8_t> &motion);
  // the pose at `frame` with `edit` applied to its bone matrices (the editor's drag, shown live)
  void poseEdited(float frame, const std::function<void(std::vector<Matrix34> &)> &edit);
  // a weapon in the hand (DevJunior, 26.11.2026: "use the weapon models we have as examples"): a model of a weapon
  // archive drawn on part `part` (the right hand, 10: the weapon model is made in its space) as it is, not skinned
  bool attachWeapon(const Archive &archive, unsigned bin, int tpl, int part, std::string *why);
  void detachWeapon();
  bool hasWeapon() const { return !pieces_.empty() && pieces_.back().attach >= 0; }
  int motionCount() const { return int(clips_.size()); }
  const std::string &motionName(int i) const { return names_.at(std::size_t(i)); }
  // -1: the rest pose; false when the animation does not fit this model's skeleton
  bool setMotion(int clip, std::string *why);
  int motion() const { return clip_; }
  float motionLength() const;  // its last frame (30 a second), 0 for the rest pose
  // the pose at `frame` (clamped), skinned into the vertex arrays
  void pose(float frame);
  // keep the root on the spot (the animation's walk does not carry the model away)
  void setInPlace(bool on) { inPlace_ = on; }

  const std::vector<float> &positions() const { return positions_; }  // xyz per corner
  const std::vector<float> &normals() const { return normals_; }
  const std::vector<float> &uvs() const { return uvs_; }
  const std::vector<Draw> &draws() const { return draws_; }
  const std::vector<Texture> &textures() const { return textures_; }
  // the skeleton: joint positions and parents (255 the root) of the current pose
  const std::vector<std::array<float, 3>> &joints() const { return jointPositions_; }
  const std::vector<std::uint8_t> &parents() const { return parents_; }
  // the model's own joints (parent, bind position against it): the animation editor's skeleton (studio_motion_edit.h)
  const std::vector<SmdJoint> *bindJoints() const { return model_ ? &model_->mesh.joints : nullptr; }
  std::array<float, 3> minimum() const { return minimum_; }
  std::array<float, 3> maximum() const { return maximum_; }
  unsigned parts() const { return unsigned(parents_.size()); }
  std::size_t triangles() const { return positions_.size() / 9; }
  // the rest pose's bounds (the camera frames it once)
  std::array<float, 3> restMinimum() const { return restMinimum_; }
  std::array<float, 3> restMaximum() const { return restMaximum_; }

private:
  struct Piece {
    std::unique_ptr<SkinnedModel> model;
    std::vector<std::uint16_t> cornerPosition, cornerNormal;
    std::vector<float> uvs;
    std::vector<Draw> draws;  // `first` within the piece, `texture` within textures_
    int attach = -1;               // a weapon: the part it is drawn on (not skinned)
    std::size_t textureBase = 0;   // its first image in textures_
  };
  bool buildPiece(const Archive &archive, unsigned bin, int tpl, Piece &piece, std::string *why);
  void rebuild();
  void skin(const std::vector<Matrix34> &bones);
  std::vector<Piece> pieces_;
  std::vector<std::string> pieceNames_;
  std::vector<std::pair<std::string, unsigned>> pieceKeys_;
  SkinnedModel *model_ = nullptr;  // pieces_[0]'s
  std::unique_ptr<AnimationRig> rig_;
  std::vector<AnimationBytes> clips_;
  std::vector<std::string> names_;
  int clip_ = -1;
  bool inPlace_ = true;
  int highlight_ = -1;
  std::vector<float> positions_, normals_, uvs_;
  std::vector<Draw> draws_;
  std::vector<Texture> textures_;
  std::vector<std::array<float, 3>> jointPositions_;
  std::vector<std::uint8_t> parents_;
  std::array<float, 3> minimum_{}, maximum_{}, restMinimum_{}, restMaximum_{};
};

}  // namespace re4::studio
