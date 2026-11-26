#pragma once
// CubeEvil4 Studio's 3D view (GLES 3): the model of studio::Viewer, a floor grid on X/Z with the axes, the skeleton.
#include "studio_model.h"

namespace re4::studio {

struct Camera {
  float yaw = 0.6f, pitch = 0.25f;   // radians around the target
  float distance = 3500.f;           // mm
  std::array<float, 3> target{0, 900, 0};
};

struct ViewOptions {
  bool grid = true, axes = true, skeleton = false, textured = true;
  // the animation editor's handles (DevJunior, 26.11.2026: "a skeleton with little circles and a cross to control its
  // new position and rotation"): a circle on each joint that can be edited, the chosen one bigger with the gizmo
  // (gizmo 1 move: three arrows; 2 turn: three rings) along `gizmoAxes` (unit vectors, model space)
  std::vector<std::array<float, 3>> handles;
  std::vector<int> handleParts;
  int selected = -1;  // a part number of handleParts
  int gizmo = 0;
  std::array<std::array<float, 3>, 3> gizmoAxes{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
};

// The editor's gizmo on the screen: a touch (pixels, y down) against the last draw. GizmoSize: its arrows' length and
// rings' radius (mm) for a camera distance
float GizmoSize(float distance);
// the screen point (pixels) of a model-space point; false behind the camera
bool ToScreen(const std::array<float, 16> &viewProjection, int width, int height, const std::array<float, 3> &p,
              float out[2]);

class GlView {
public:
  // with a current GLES 3 context (the surface made); false and `why` on a shader error
  bool create(std::string *why);
  void destroy();
  // the textures of a newly loaded model (once per model)
  void upload(const Viewer &viewer);
  void draw(const Viewer &viewer, int width, int height, const Camera &camera, const ViewOptions &options);
  // the matrices of the last draw (for picking): projection * view
  const std::array<float, 16> &viewProjection() const { return viewProjection_; }

private:
  unsigned meshProgram_ = 0, lineProgram_ = 0;
  unsigned meshVao_ = 0, lineVao_ = 0;
  unsigned positionBuffer_ = 0, normalBuffer_ = 0, uvBuffer_ = 0, lineBuffer_ = 0;
  std::vector<unsigned> textures_;
  std::size_t uvCount_ = 0;
  std::array<float, 16> viewProjection_{};
};

// the camera that frames a model's bounds (mm)
Camera FrameBounds(const std::array<float, 3> &minimum, const std::array<float, 3> &maximum);

}  // namespace re4::studio
