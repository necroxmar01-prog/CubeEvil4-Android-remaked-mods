// An RE Engine .mesh dumped per material for port/tools/skin_head_fbx.py --re-mesh (NAME.P/.N/.T float32, .tri int32,
// index.txt). Build: g++ -O2 -std=c++17 -Iapp/src/main/cpp -Iapp/src/main/cpp/include tools/re_mesh_dump.cpp
//   app/src/main/cpp/third_party/meshoptimizer/{simplifier,allocator}.cpp LIBRE4_PLATFORM.a -lz; run: re_mesh_dump X.mesh OUTDIR
#include "studio_convert.cpp"
#include <fstream>
#include <iterator>
using namespace re4::studio::convert;
static Bytes slurp(const char *p) { std::ifstream i(p, std::ios::binary); return Bytes(std::istreambuf_iterator<char>(i), {}); }
int main(int argc, char **argv) {
  auto m = readReMesh(slurp(argv[1]));
  std::string out = argv[2];
  FILE *idx = std::fopen((out + "/index.txt").c_str(), "w");
  for (std::size_t k = 0; k < m.mats.size(); ++k) {
    std::vector<int> map(m.pos.size(), -1);
    std::vector<float> P, N, T;
    std::vector<int> tri;
    for (const auto &t : m.tris) {
      if (t[3] != k) continue;
      for (int c = 0; c < 3; ++c) {
        unsigned v = t[std::size_t(c)];
        if (map[v] < 0) {
          map[v] = int(P.size() / 3);
          P.insert(P.end(), {float(m.pos[v].x), float(m.pos[v].y), float(m.pos[v].z)});
          N.insert(N.end(), {float(m.nrm[v].x), float(m.nrm[v].y), float(m.nrm[v].z)});
          T.insert(T.end(), {float(m.uv[v][0]), float(m.uv[v][1])});
        }
        tri.push_back(map[v]);
      }
    }
    std::string base = out + "/" + m.mats[k];
    auto w = [&](const std::string &ext, const void *d, std::size_t n) { FILE *f = std::fopen((base + ext).c_str(), "wb"); std::fwrite(d, 1, n, f); std::fclose(f); };
    w(".P", P.data(), P.size() * 4); w(".N", N.data(), N.size() * 4); w(".T", T.data(), T.size() * 4); w(".tri", tri.data(), tri.size() * 4);
    std::fprintf(idx, "%s %zu %zu\n", m.mats[k].c_str(), P.size() / 3, tri.size() / 3);
  }
  std::fclose(idx);
  double ymin = 1e9, ymax = -1e9;
  for (auto &p : m.pos) ymin = std::min(ymin, p.y), ymax = std::max(ymax, p.y);
  std::printf("y %.4f .. %.4f\n", ymin, ymax);
}
