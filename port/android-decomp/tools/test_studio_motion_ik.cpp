// Host check of the Studio editor's pose and IK (cpp/studio_motion_edit.cpp) against the Studio's viewer (the game's
// MotionMove, studio_model.cpp): every joint of every frame of a clip in the same place, then a hand and a foot moved
// by IK reach their targets. Usage: motion_ik_check MODEL.drs MOVE.dat [OUT.dat]
#include "studio_model.h"
#include "studio_motion_edit.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace re4::studio;
using namespace re4::studio::motionedit;

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  Archive arc, mot;
  std::string why;
  if (!arc.open(argv[1], &why) || !mot.open(argv[2], &why)) {
    std::fprintf(stderr, "open: %s\n", why.c_str());
    return 1;
  }
  Viewer v;
  if (!v.loadParts(arc, GameParts(arc.name(), arc), &why)) {
    std::fprintf(stderr, "model: %s\n", why.c_str());
    return 1;
  }
  v.setInPlace(false);
  v.addMotions(mot, &why);
  std::ifstream in(argv[2], std::ios::binary);
  const Bytes body((std::istreambuf_iterator<char>(in)), {});
  Editor e;
  if (!e.open(body, &why)) {
    std::fprintf(stderr, "editor: %s\n", why.c_str());
    return 1;
  }
  Skeleton s;
  for (const auto &j : *v.bindJoints()) {
    s.parent.push_back(j.parent == 255 ? -1 : j.parent);
    s.bind.push_back(j.center);
  }
  int bad = 0;
  const auto clips = e.clips();
  for (std::size_t c = 0; c < clips.size() && c < 6; ++c) {
    if (!v.setMotion(int(c), &why)) continue;
    float worst = 0;
    for (int f = 0; f <= clips[c].frames; f += 3) {
      v.pose(float(f));
      std::vector<Matrix> w;
      e.pose(clips[c].entry, float(f), s, w);
      const auto &j = v.joints();
      // against the root joint (the viewer carries the root's own placement)
      for (std::size_t p = 1; p < j.size() && p < w.size(); ++p) {
        float d = 0;
        for (int a = 0; a < 3; ++a) {
          const float x = (j[p][a] - j[0][a]) - (w[p][std::size_t(a * 4 + 3)] - w[0][std::size_t(a * 4 + 3)]);
          d += x * x;
        }
        worst = std::max(worst, std::sqrt(d));
      }
    }
    std::printf("%-28s pose vs viewer: %.3f mm at worst\n", clips[c].name.c_str(), double(worst));
    if (worst > 1.0f) ++bad;
  }
  // IK: the right hand 100 mm up over frames 5..20 of the walk, the left foot 80 mm forward over all of the stand
  const unsigned walk = clips[1].entry, stand = clips[0].entry;
  std::vector<Matrix> before, after;
  e.pose(walk, 12, s, before);
  const float up[3] = {0, 100, 0};
  if (!e.reach(walk, s, 0, up, 5, 20, 4, &why)) {
    std::fprintf(stderr, "reach: %s\n", why.c_str());
    return 1;
  }
  e.pose(walk, 12, s, after);
  std::printf("right hand at frame 12: %.1f mm higher (want 100), %.1f sideways/forward; miss %.2f mm\n",
              double(after[10][7] - before[10][7]), double(std::hypot(after[10][3] - before[10][3], after[10][11] - before[10][11])),
              double(e.lastReachMiss()));
  float rot = 0;
  for (int k : {0, 1, 2, 4, 5, 6, 8, 9, 10}) rot = std::max(rot, std::fabs(after[10][std::size_t(k)] - before[10][std::size_t(k)]));
  std::printf("right hand's turn in the world: %.5f at most changed\n", double(rot));
  if (std::fabs(after[10][7] - before[10][7] - 100) > 2 || rot > 0.01f) ++bad;
  e.pose(walk, 30, s, before);
  std::vector<Matrix> orig;
  Editor o;
  o.open(body, nullptr);
  o.pose(walk, 30, s, orig);
  std::printf("frame 30 (outside): %.3f mm\n", double(std::fabs(before[10][7] - orig[10][7])));
  const float fwd[3] = {0, 0, 80};
  o.pose(stand, 40, s, before);
  if (!e.reach(stand, s, 3, fwd, 0, 1000, 0, &why)) {
    std::fprintf(stderr, "reach foot: %s\n", why.c_str());
    return 1;
  }
  e.pose(stand, 40, s, after);
  std::printf("left foot at frame 40: %.1f mm forward (want 80); miss %.2f mm\n", double(after[24][11] - before[24][11]),
              double(e.lastReachMiss()));
  if (std::fabs(after[24][11] - before[24][11] - 80) > 2) ++bad;
  if (argc > 3) {
    std::ofstream out(argv[3], std::ios::binary);
    const auto b = e.save();
    out.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
  }
  std::printf("%s\n", bad ? "FAILED" : "ok");
  return bad ? 1 : 0;
}
