// CubeEvil4 Studio's GTA skin converter on the host (cpp/studio_dff.inc):
//   studio_dff_check GC_ARCHIVE SKIN.dff SKIN.txd|- OUT [--tex N] [--transfer 0..1] [--head 0|1]
#include "studio_convert.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
using namespace re4::studio::convert;
static Bytes slurp(const std::string &p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
int main(int argc, char **argv) {
  if (argc < 5) {
    std::fprintf(stderr, "usage: studio_dff_check GC SKIN.dff SKIN.txd|- OUT [--tex N] [--transfer 0..1] [--head 0|1]\n");
    return 2;
  }
  Options o;
  for (int i = 5; i + 1 < argc; i += 2)
    if (!std::strcmp(argv[i], "--tex")) o.tex = std::atoi(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--transfer")) o.transfer = std::atof(argv[i + 1]);
    else if (!std::strcmp(argv[i], "--head")) o.keepHead = std::atoi(argv[i + 1]) != 0;
    else if (!std::strcmp(argv[i], "--inner")) o.innerLayer = std::atoi(argv[i + 1]) != 0;
  Bytes out;
  std::string log, why;
  const std::string gc = argv[1];
  const Bytes txd = std::string(argv[3]) == "-" ? Bytes() : slurp(argv[3]);
  const bool ok = ConvertDff(slurp(gc), gc.substr(gc.find_last_of('/') + 1), slurp(argv[2]), txd, o, out, log, &why);
  std::fputs(log.c_str(), stdout);
  if (!ok) {
    std::fprintf(stderr, "failed: %s\n", why.c_str());
    return 1;
  }
  std::ofstream(argv[4], std::ios::binary).write(reinterpret_cast<const char *>(out.data()), std::streamsize(out.size()));
  return 0;
}
