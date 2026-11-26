// CubeEvil4 Studio's FBX converter on the host (cpp/studio_fbx.inc):
//   studio_fbx_check GC_ARCHIVE MODEL.fbx OUT [--tex N] [--head 0|1]   (its pictures: the files beside the .fbx)
#include "studio_convert.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <string>
using namespace re4::studio::convert;
static Bytes slurp(const std::string &p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
int main(int argc, char **argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: studio_fbx_check GC MODEL.fbx OUT [--tex N] [--head 0|1]\n");
    return 2;
  }
  Options o;
  for (int i = 4; i + 1 < argc; i += 2)
    if (!std::strcmp(argv[i], "--tex")) o.tex = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--head")) o.keepHead = std::atoi(argv[i + 1]) != 0;
    else if (!std::strcmp(argv[i], "--transfer")) o.transfer = std::atof(argv[i + 1]);
  const std::string fbx = argv[2];
  const std::string dir = fbx.find('/') == std::string::npos ? "." : fbx.substr(0, fbx.find_last_of('/'));
  std::map<std::string, Bytes> files;
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *e = readdir(d)) {
      std::string n = e->d_name, low = n;
      for (auto &c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
      if (low.size() > 4 && (low.compare(low.size() - 4, 4, ".png") == 0 || low.compare(low.size() - 4, 4, ".tga") == 0 ||
                             low.compare(low.size() - 4, 4, ".jpg") == 0))
        files[low] = slurp(dir + "/" + n);
    }
    closedir(d);
  }
  Bytes out;
  std::string log, why;
  const std::string gc = argv[1];
  const bool ok = ConvertFbx(slurp(gc), gc.substr(gc.find_last_of('/') + 1), slurp(fbx), files, o, out, log, &why);
  std::fputs(log.c_str(), stdout);
  if (!ok) {
    std::fprintf(stderr, "failed: %s\n", why.c_str());
    return 1;
  }
  std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
  return 0;
}
