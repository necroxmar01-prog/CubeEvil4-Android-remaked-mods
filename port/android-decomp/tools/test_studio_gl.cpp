// CubeEvil4 Studio check on the host (EGL pbuffer, GLES 3): an archive's model posed by one of its animations and drawn
// with the grid, written as a PPM. Usage: studio_gl_check ARCHIVE BIN MOTION_ARCHIVE|- MOTION FRAME OUT.ppm
// STUDIO_CAM=yaw,pitch,distance,targetY (radians, mm) sets the camera (no skeleton drawn): a close look at a joint.
#include "studio_gl.h"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>
using namespace re4::studio;
static const int W = 960, H = 720;
static void check(bool ok, const char *what) { if (!ok) throw std::runtime_error(what); }
int main(int argc, char **argv) {
  try {
    if (argc != 7) throw std::runtime_error("usage: studio_gl_check ARCHIVE BIN MOTION_ARCHIVE|- MOTION FRAME OUT.ppm");
    setenv("EGL_PLATFORM", "surfaceless", 1);
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major, minor, n;
    check(eglInitialize(d, &major, &minor) && eglBindAPI(EGL_OPENGL_ES_API), "EGL");
    const EGLint attrs[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8,
                         EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_NONE};
    EGLConfig config;
    check(eglChooseConfig(d, attrs, &config, 1, &n) && n, "EGL config");
    const EGLint size[]{EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(d, config, size);
    const EGLint ctx[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    EGLContext context = eglCreateContext(d, config, EGL_NO_CONTEXT, ctx);
    check(eglMakeCurrent(d, surface, surface, context), "EGL context");
    Archive arc;
    std::string why;
    check(arc.open(argv[1], &why), ("archive: " + why).c_str());
    std::printf("%s: %zu entries, %zu BIN, %zu TPL, %zu FCV\n", argv[1], arc.entries().size(), arc.of("BIN").size(),
                arc.of("TPL").size(), arc.of("FCV").size());
    Viewer v;
    // BIN "game": the whole model as the game puts it together (GameParts)
    if (argv[2] == std::string("game")) {
      const auto specs = GameParts(arc.name(), arc);
      check(!specs.empty(), "no game parts for this archive");
      check(v.loadParts(arc, specs, &why), ("parts: " + why).c_str());
      for (const auto &n : v.pieces()) std::printf("piece: %s\n", n.c_str());
      if (const char *h = std::getenv("STUDIO_HIGHLIGHT")) v.setHighlight(std::atoi(h));
    } else {
      check(v.loadModel(arc, unsigned(std::atoi(argv[2])), -1, &why), ("model: " + why).c_str());
    }
    std::printf("model: %u parts, %zu triangles, %zu textures, rest %.0f..%.0f mm high\n", v.parts(), v.triangles(),
                v.textures().size(), v.restMinimum()[1], v.restMaximum()[1]);
    if (const char *pieces = std::getenv("STUDIO_PIECES")) {
      for (const char *q = pieces; *q;) {
        bool added = false;
        const unsigned bin = unsigned(std::strtoul(q, const_cast<char **>(&q), 10));
        const bool ok = v.togglePiece(arc, bin, -1, &added, &why);
        std::printf("piece %u: %s\n", bin, ok ? (added ? "added" : "taken off") : why.c_str());
        while (*q == ',') ++q;
      }
      std::printf("now %zu triangles\n", v.triangles());
    }
    Archive mot;
    const std::string motPath = argv[3] == std::string("-") ? argv[1] : argv[3];
    check(mot.open(motPath, &why), ("motions: " + why).c_str());
    std::printf("motions: %d\n", v.addMotions(mot, &why));
    const int clip = std::atoi(argv[4]);
    int fit = 0;
    for (int i = 0; i < v.motionCount(); ++i) {
      std::string no;
      if (v.setMotion(i, &no)) ++fit;
      else std::printf("motion %s: %s\n", v.motionName(i).c_str(), no.c_str());
    }
    std::printf("motions that fit the skeleton: %d of %d\n", fit, v.motionCount());
    if (clip >= 0) {
      check(v.setMotion(clip, &why), ("motion: " + why).c_str());
      std::printf("motion %s: %.0f frames\n", v.motionName(clip).c_str(), v.motionLength());
    }
    v.pose(float(std::atof(argv[5])));
    GlView view;
    check(view.create(&why), ("GL: " + why).c_str());
    view.upload(v);
    ViewOptions options;
    options.skeleton = !std::getenv("STUDIO_CAM");
    Camera cam = FrameBounds(v.restMinimum(), v.restMaximum());
    if (const char *c = std::getenv("STUDIO_CAM"))
      std::sscanf(c, "%f,%f,%f,%f", &cam.yaw, &cam.pitch, &cam.distance, &cam.target[1]);
    // STUDIO_GIZMO=part,mode: the animation editor's circles on the main joints and the gizmo (1 move, 2 turn)
    if (const char *g = std::getenv("STUDIO_GIZMO")) {
      int part = -1, mode = 0;
      std::sscanf(g, "%d,%d", &part, &mode);
      for (int p : {0, 1, 2, 3, 4, 7, 8, 10, 13, 14, 16, 17, 18, 19, 20, 22, 23, 24})
        if (std::size_t(p) < v.joints().size()) options.handles.push_back(v.joints()[std::size_t(p)]), options.handleParts.push_back(p);
      options.selected = part;
      options.gizmo = mode;
    }
    view.draw(v, W, H, cam, options);
    std::vector<unsigned char> px(W * H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    check(glGetError() == GL_NO_ERROR, "GL error");
    FILE *f = std::fopen(argv[6], "wb");
    std::fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = H - 1; y >= 0; --y)
      for (int x = 0; x < W; ++x) std::fwrite(&px[(y * W + x) * 4], 1, 3, f);
    std::fclose(f);
    view.destroy();
    std::printf("written %s\n", argv[6]);
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "studio_gl_check: %s\n", e.what());
    return 1;
  }
}
