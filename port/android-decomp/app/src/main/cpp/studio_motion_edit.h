#pragma once
// CubeEvil4 Studio (Mods, 26.11.2026, DevJunior: "I want to edit the animations I have and take them to the game
// myself"): the animation editor of the Edit tab. It opens a motion archive's body (a mod pack's melee/move.dat,
// finish.dat ...: count, offsets, types, 32-aligned entries; port/tools/uhd_skin_convert.py build_body), turns a
// bone of one clip (or moves the root) over a range of frames with a soft edge, changes a clip's speed, undoes, and
// writes the body again. The FCV format is the one of tools/motion/fcv.py; what is not edited keeps its bytes.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace re4::studio::motionedit {

using Bytes = std::vector<std::uint8_t>;

struct Key {
  float value{}, in{}, out{};  // as the game's Fcc_get_data_* reads them (s16 / s8 in 1/10000)
};

struct Axis {
  std::vector<std::uint16_t> frames;
  std::vector<Key> keys;
};

struct Joint {
  std::uint16_t info{};  // kind (bits 0-7), channel (8-11), key type (12-15)
  std::uint8_t part{};
  std::array<Axis, 3> axes;
  Bytes raw;             // its key block as read (written again unless edited)
  bool edited{};         // written as key type 0 (f32 value and tangents)
  int kind() const { return info & 0xFF; }
  int type() const { return info >> 12; }
  // 1 a rotation, 2 a position, 0 anything else (the root's own tracks, scale), MotionMoveCore's tests
  int target() const;
};

struct Motion {
  std::uint16_t rawMax{};  // maxFrame with its flags (bits 14-15)
  std::vector<Joint> joints;
  std::vector<int> layout;  // joint indices in key block order
  std::uint8_t fill = 0xCD;
  int maxFrame() const { return rawMax & 0x3FFF; }
};

bool ParseMotion(const Bytes &d, Motion &m, std::string *why);
Bytes WriteMotion(const Motion &m);
// the axis' value at `frame` as HermiteInterpolation gives it (not looping)
float Evaluate(const Axis &a, float frame, int maxFrame);

// The skeleton the clips pose (the model's joints: parent, 255 none, and bind position against it)
struct Skeleton {
  std::vector<int> parent;
  std::vector<std::array<float, 3>> bind;
};
using Matrix = std::array<float, 12>;  // 3x4, row major (the game's Mtx)

class Editor {
 public:
  struct Clip {
    unsigned entry{};
    int code = -1;  // its move.dat code (the MOV1 table), -1 none
    int frames{};   // its last frame
    std::string name;
  };

  bool open(const Bytes &file, std::string *why);
  bool isOpen() const { return !entries_.empty(); }
  Bytes save() const;
  std::vector<Clip> clips() const;
  // the parts with a rotation track (pos false) or a position track in the clip
  std::vector<int> parts(unsigned entry, bool pos) const;
  // adds d (radians for a rotation, millimetres for a position) on X, Y, Z from frame f0 to f1, easing in and out
  // over `fade` frames on either side
  bool adjust(unsigned entry, int part, bool pos, const float d[3], int f0, int f1, int fade, std::string *why);
  // plays the clip `speed` times as fast (its frames, its MOV1 rows' length and foot frames)
  bool retime(unsigned entry, float speed, std::string *why);
  // keeps frames f0..f1 of the clip (frame f0 becomes 0): every curve cut exactly (the Hermite pieces split with their
  // tangents scaled), its MOV1 rows' length and foot frames with it
  bool trim(unsigned entry, int f0, int f1, std::string *why);
  // the model-space matrix of every part at `frame` (the game's cCoord: T(pos) R(z) R(y) R(x), the parent's times the
  // own; the root's own placement left out)
  bool pose(unsigned entry, float frame, const Skeleton &s, std::vector<Matrix> &out) const;
  // the pose at `frame` before and after a change that is being dragged (mode 2: the part's X, Y, Z angles + d
  // radians; 1: the root's place + d mm, or a hand or foot moved by d mm through IK); nothing is kept
  bool posePreview(unsigned entry, float frame, const Skeleton &s, int mode, int part, const float d[3],
                   std::vector<Matrix> &before, std::vector<Matrix> &after) const;
  // the clip's bytes as they are now (edited or as read)
  Bytes clipBytes(unsigned entry) const;
  // the three model-space directions that a change of the part's X, Y, Z angle turns it about at `frame` (the game's
  // R(z) R(y) R(x): X its own axis, Y the axis after Z, Z its parent's)
  bool turnAxes(unsigned entry, float frame, const Skeleton &s, int part, std::array<std::array<float, 3>, 3> &out) const;
  // IK (DevJunior, 25.11.2026: "IK bones to edit the animations"): the hand or foot (end) moved by d (mm, model
  // space: X side, Y up, Z forward) from frame f0 to f1, easing over `fade` frames; its two bones above (upper arm and
  // forearm, thigh and calf) turn to reach it, the end keeps its turn in the world
  bool reach(unsigned entry, const Skeleton &s, int end, const float d[3], int f0, int f1, int fade, std::string *why);
  bool undo();
  int undoCount() const { return int(undo_.size()); }
  bool edited() const { return edited_; }
  // how far (mm) the last reach stayed from its target at worst (out of the arm's or leg's length)
  float lastReachMiss() const { return lastReachMiss_; }

 private:
  struct Saved {
    std::vector<std::pair<unsigned, Motion>> motions;
    Bytes table;  // entry 0 (the MOV1 table) before
  };
  Motion *motion(unsigned entry);
  const Motion *motion(unsigned entry) const;
  void remember(std::vector<unsigned> which);
  bool retimeOne(unsigned entry, float speed);
  bool trimOne(unsigned entry, int f0, int f1);
  // the clip's code and the shotgun carry layers that share its frames (gCarry: 0x98 + k for the moves)
  int codeOf(unsigned entry) const;
  std::vector<unsigned> withLayers(unsigned entry) const;

  Bytes head_;  // bytes 4..15 of the body (written back as they were)
  std::vector<Bytes> entries_;
  std::vector<std::array<std::uint8_t, 4>> types_;
  std::vector<Motion> motions_;   // per entry (no joints: not an FCV or not read)
  std::vector<bool> isMotion_;
  std::vector<bool> changed_;     // written from motions_ (else the entry's bytes)
  std::vector<Saved> undo_;
  bool edited_{};
  float lastReachMiss_{};
};

}  // namespace re4::studio::motionedit
