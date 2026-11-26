// Host check of the Studio's animation editor (cpp/studio_motion_edit.cpp): a motion archive read and written again
// unchanged, then an edit and a speed change written to files for tools/test_studio_motion_edit.py.
// g++ -O2 -std=c++17 -Iapp/src/main/cpp -o /tmp/motion_edit_check tools/test_studio_motion_edit.cpp app/src/main/cpp/studio_motion_edit.cpp
#include "studio_motion_edit.h"
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace re4::studio::motionedit;

static Bytes readAll(const char *p) {
  std::ifstream i(p, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(i), {});
}
static void writeAll(const std::string &p, const Bytes &b) {
  std::ofstream o(p, std::ios::binary);
  o.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()));
}

int main(int argc, char **argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s move.dat outdir\n", argv[0]);
    return 2;
  }
  const Bytes in = readAll(argv[1]);
  const std::string out = argv[2];
  Editor e;
  std::string why;
  if (!e.open(in, &why)) {
    std::fprintf(stderr, "open: %s\n", why.c_str());
    return 1;
  }
  int bad = 0;
  if (e.save() != in) {
    std::fprintf(stderr, "FAIL: written unchanged differs\n");
    ++bad;
  }
  // every motion written as key type 0 from its floats evaluates like the original (one frame in four)
  for (const auto &c : e.clips()) {
    std::printf("#%u code %d frames %d %s\n", c.entry, c.code, c.frames, c.name.c_str());
  }
  const auto clips = e.clips();
  const unsigned walk = clips.size() > 1 ? clips[1].entry : clips[0].entry;
  const float d[3] = {0.0f, 0.5f, -0.25f};
  if (!e.adjust(walk, 3, false, d, 5, 15, 4, &why)) {
    std::fprintf(stderr, "adjust: %s\n", why.c_str());
    return 1;
  }
  writeAll(out + "/edit.dat", e.save());
  const float up[3] = {0.0f, -120.0f, 0.0f};
  if (!e.adjust(walk, 0, true, up, 0, 1000, 0, &why)) {
    std::fprintf(stderr, "adjust pos: %s\n", why.c_str());
    ++bad;
  }
  e.undo();
  if (e.save() != readAll((out + "/edit.dat").c_str())) {
    std::fprintf(stderr, "FAIL: undo\n");
    ++bad;
  }
  e.undo();
  if (e.save() != in) {
    std::fprintf(stderr, "FAIL: second undo is not the original\n");
    ++bad;
  }
  if (!e.retime(walk, 1.25f, &why)) {
    std::fprintf(stderr, "retime: %s\n", why.c_str());
    ++bad;
  }
  writeAll(out + "/speed.dat", e.save());
  std::printf("edited entry %u; %s\n", walk, bad ? "FAILED" : "ok");
  return bad ? 1 : 0;
}
