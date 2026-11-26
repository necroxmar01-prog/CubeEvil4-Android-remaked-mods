// CubeEvil4 Studio's 3D view (studio_gl.h)
#include "studio_gl.h"
#include <GLES3/gl3.h>
#include <algorithm>
#include <cmath>

namespace re4::studio {
namespace {

const char *kMeshVs = R"(#version 300 es
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
uniform mat4 viewProjection;
out vec3 vNormal;
out vec2 vUv;
void main() {
  vNormal = normal;
  vUv = uv;
  gl_Position = viewProjection * vec4(position, 1.0);
}
)";

const char *kMeshFs = R"(#version 300 es
precision mediump float;
in vec3 vNormal;
in vec2 vUv;
uniform sampler2D image;
uniform int textured;
uniform int alphaTest;
uniform int highlight;
uniform vec3 light;
out vec4 color;
void main() {
  vec4 base = textured != 0 ? texture(image, vUv) : vec4(0.72, 0.74, 0.78, 1.0);
  if (alphaTest != 0 && base.a < 0.5) discard;
  vec3 n = normalize(vNormal);
  float d = max(dot(n, light), 0.0);
  float rim = pow(1.0 - abs(n.z), 2.0) * 0.12;
  color = vec4(base.rgb * (0.38 + 0.72 * d) + rim, 1.0);
  // the piece the list points at: tinted in the Studio's accent
  if (highlight != 0) color.rgb = mix(color.rgb, vec3(0.88, 0.64, 0.23), 0.5);
}
)";

const char *kLineVs = R"(#version 300 es
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 tint;
uniform mat4 viewProjection;
out vec4 vTint;
void main() {
  vTint = tint;
  gl_Position = viewProjection * vec4(position, 1.0);
  gl_PointSize = 9.0;
}
)";

const char *kLineFs = R"(#version 300 es
precision mediump float;
in vec4 vTint;
out vec4 color;
void main() { color = vTint; }
)";

unsigned compile(unsigned kind, const char *source, std::string *why) {
  const unsigned s = glCreateShader(kind);
  glShaderSource(s, 1, &source, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512] = {};
    glGetShaderInfoLog(s, sizeof(log), nullptr, log);
    if (why) *why = log;
    glDeleteShader(s);
    return 0;
  }
  return s;
}

unsigned link(const char *vs, const char *fs, std::string *why) {
  const unsigned v = compile(GL_VERTEX_SHADER, vs, why), f = v ? compile(GL_FRAGMENT_SHADER, fs, why) : 0;
  if (!v || !f) return 0;
  const unsigned p = glCreateProgram();
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);
  glDeleteShader(v);
  glDeleteShader(f);
  GLint ok = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512] = {};
    glGetProgramInfoLog(p, sizeof(log), nullptr, log);
    if (why) *why = log;
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

using Mat4 = std::array<float, 16>;  // column-major

Mat4 multiply(const Mat4 &a, const Mat4 &b) {
  Mat4 r{};
  for (int c = 0; c < 4; ++c)
    for (int row = 0; row < 4; ++row) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[c * 4 + k];
      r[c * 4 + row] = s;
    }
  return r;
}

Mat4 perspective(float fovY, float aspect, float nearZ, float farZ) {
  const float f = 1.f / std::tan(fovY / 2);
  Mat4 m{};
  m[0] = f / aspect;
  m[5] = f;
  m[10] = (farZ + nearZ) / (nearZ - farZ);
  m[11] = -1;
  m[14] = 2 * farZ * nearZ / (nearZ - farZ);
  return m;
}

Mat4 lookAt(const std::array<float, 3> &eye, const std::array<float, 3> &at) {
  std::array<float, 3> f{at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
  float l = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  for (auto &v : f) v /= l;
  std::array<float, 3> s{f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0};  // f x up(0,1,0)
  l = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
  if (l < 1e-6f) s = {1, 0, 0}, l = 1;
  for (auto &v : s) v /= l;
  const std::array<float, 3> u{s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0]};
  Mat4 m{};
  m[0] = s[0], m[4] = s[1], m[8] = s[2];
  m[1] = u[0], m[5] = u[1], m[9] = u[2];
  m[2] = -f[0], m[6] = -f[1], m[10] = -f[2];
  m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
  m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
  m[14] = f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2];
  m[15] = 1;
  return m;
}

void pushLine(std::vector<float> &out, const std::array<float, 3> &a, const std::array<float, 3> &b,
              const std::array<float, 4> &c) {
  out.insert(out.end(), {a[0], a[1], a[2], c[0], c[1], c[2], c[3], b[0], b[1], b[2], c[0], c[1], c[2], c[3]});
}

}  // namespace

float GizmoSize(float distance) { return std::max(60.f, distance * 0.07f); }

bool ToScreen(const std::array<float, 16> &m, int width, int height, const std::array<float, 3> &p, float out[2]) {
  const float x = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
  const float y = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
  const float w = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
  if (w <= 1e-4f) return false;
  out[0] = (x / w * 0.5f + 0.5f) * float(width);
  out[1] = (0.5f - y / w * 0.5f) * float(height);
  return true;
}

Camera FrameBounds(const std::array<float, 3> &minimum, const std::array<float, 3> &maximum) {
  Camera c;
  c.target = {(minimum[0] + maximum[0]) / 2, (minimum[1] + maximum[1]) / 2, (minimum[2] + maximum[2]) / 2};
  float size = 0;
  for (int k = 0; k < 3; ++k) size = std::max(size, maximum[k] - minimum[k]);
  c.distance = std::max(300.f, size * 1.7f);
  return c;
}

bool GlView::create(std::string *why) {
  meshProgram_ = link(kMeshVs, kMeshFs, why);
  lineProgram_ = meshProgram_ ? link(kLineVs, kLineFs, why) : 0;
  if (!meshProgram_ || !lineProgram_) return false;
  glGenVertexArrays(1, &meshVao_);
  glGenVertexArrays(1, &lineVao_);
  glGenBuffers(1, &positionBuffer_);
  glGenBuffers(1, &normalBuffer_);
  glGenBuffers(1, &uvBuffer_);
  glGenBuffers(1, &lineBuffer_);
  glBindVertexArray(meshVao_);
  glBindBuffer(GL_ARRAY_BUFFER, positionBuffer_);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, normalBuffer_);
  glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(1);
  glBindBuffer(GL_ARRAY_BUFFER, uvBuffer_);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glEnableVertexAttribArray(2);
  glBindVertexArray(lineVao_);
  glBindBuffer(GL_ARRAY_BUFFER, lineBuffer_);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), nullptr);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float), reinterpret_cast<void *>(3 * sizeof(float)));
  glEnableVertexAttribArray(1);
  glBindVertexArray(0);
  return true;
}

void GlView::destroy() {
  if (!textures_.empty()) glDeleteTextures(GLsizei(textures_.size()), textures_.data());
  textures_.clear();
  const unsigned buffers[] = {positionBuffer_, normalBuffer_, uvBuffer_, lineBuffer_};
  glDeleteBuffers(4, buffers);
  const unsigned vaos[] = {meshVao_, lineVao_};
  glDeleteVertexArrays(2, vaos);
  glDeleteProgram(meshProgram_);
  glDeleteProgram(lineProgram_);
  meshProgram_ = lineProgram_ = 0;
}

void GlView::upload(const Viewer &viewer) {
  if (!textures_.empty()) glDeleteTextures(GLsizei(textures_.size()), textures_.data());
  textures_.assign(viewer.textures().size(), 0);
  for (std::size_t i = 0; i < textures_.size(); ++i) {
    const auto &t = viewer.textures()[i];
    if (t.rgba.empty() || t.rgba.size() < std::size_t(t.width) * t.height * 4) continue;
    glGenTextures(1, &textures_[i]);
    glBindTexture(GL_TEXTURE_2D, textures_[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, GLsizei(t.width), GLsizei(t.height), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 t.rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    const auto wrap = [](unsigned w) { return w == 1 ? GL_REPEAT : w == 2 ? GL_MIRRORED_REPEAT : GL_CLAMP_TO_EDGE; };
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap(t.wrapS));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap(t.wrapT));
  }
  glBindBuffer(GL_ARRAY_BUFFER, uvBuffer_);
  glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(viewer.uvs().size() * sizeof(float)), viewer.uvs().data(), GL_STATIC_DRAW);
  uvCount_ = viewer.uvs().size();
}

void GlView::draw(const Viewer &viewer, int width, int height, const Camera &camera, const ViewOptions &options) {
  glViewport(0, 0, width, height);
  glClearColor(0.105f, 0.11f, 0.125f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  const float cp = std::cos(camera.pitch), sp = std::sin(camera.pitch);
  const std::array<float, 3> eye{camera.target[0] + camera.distance * cp * std::sin(camera.yaw),
                                 camera.target[1] + camera.distance * sp,
                                 camera.target[2] + camera.distance * cp * std::cos(camera.yaw)};
  const float aspect = height > 0 ? float(width) / float(height) : 1.f;
  viewProjection_ = multiply(perspective(0.8f, aspect, std::max(10.f, camera.distance * 0.01f),
                                         camera.distance * 20 + 20000),
                             lookAt(eye, camera.target));
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDisable(GL_CULL_FACE);
  // the model
  if (viewer.hasModel() && !viewer.positions().empty()) {
    glUseProgram(meshProgram_);
    glUniformMatrix4fv(glGetUniformLocation(meshProgram_, "viewProjection"), 1, GL_FALSE, viewProjection_.data());
    // the light from over the viewer's shoulder
    std::array<float, 3> l{eye[0] - camera.target[0] + 600, eye[1] - camera.target[1] + 1500,
                           eye[2] - camera.target[2]};
    const float ll = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    glUniform3f(glGetUniformLocation(meshProgram_, "light"), l[0] / ll, l[1] / ll, l[2] / ll);
    glUniform1i(glGetUniformLocation(meshProgram_, "image"), 0);
    glBindVertexArray(meshVao_);
    glBindBuffer(GL_ARRAY_BUFFER, positionBuffer_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(viewer.positions().size() * sizeof(float)), viewer.positions().data(),
                 GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, normalBuffer_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(viewer.normals().size() * sizeof(float)), viewer.normals().data(),
                 GL_STREAM_DRAW);
    glActiveTexture(GL_TEXTURE0);
    const GLint texturedAt = glGetUniformLocation(meshProgram_, "textured"),
                alphaAt = glGetUniformLocation(meshProgram_, "alphaTest"),
                highlightAt = glGetUniformLocation(meshProgram_, "highlight");
    for (const auto &d : viewer.draws()) {
      const bool tex = options.textured && d.texture >= 0 && std::size_t(d.texture) < textures_.size() &&
                       textures_[std::size_t(d.texture)];
      glUniform1i(texturedAt, tex ? 1 : 0);
      glUniform1i(alphaAt, d.alphaTest ? 1 : 0);
      glUniform1i(highlightAt, d.piece == viewer.highlight() ? 1 : 0);
      if (tex) glBindTexture(GL_TEXTURE_2D, textures_[std::size_t(d.texture)]);
      if ((d.first + d.count) * 3 <= viewer.positions().size() / 3)
        glDrawArrays(GL_TRIANGLES, GLint(d.first * 3), GLsizei(d.count * 3));
    }
  }
  // the grid, the axes, the skeleton
  std::vector<float> lines;
  const float floorY = viewer.hasModel() ? viewer.restMinimum()[1] : 0.f;
  if (options.grid) {
    const float step = 250.f, half = 3000.f;
    for (float v = -half; v <= half + 1; v += step) {
      const bool major = std::fmod(std::fabs(v) + 1, 1000.f) < 2.f;
      const std::array<float, 4> c = major ? std::array<float, 4>{0.42f, 0.44f, 0.5f, 1}
                                           : std::array<float, 4>{0.24f, 0.25f, 0.29f, 1};
      pushLine(lines, {v, floorY, -half}, {v, floorY, half}, c);
      pushLine(lines, {-half, floorY, v}, {half, floorY, v}, c);
    }
  }
  if (options.axes) {
    pushLine(lines, {0, floorY + 1, 0}, {1000, floorY + 1, 0}, {0.95f, 0.3f, 0.3f, 1});   // X red
    pushLine(lines, {0, floorY + 1, 0}, {0, floorY + 1000, 0}, {0.4f, 0.9f, 0.4f, 1});   // Y green
    pushLine(lines, {0, floorY + 1, 0}, {0, floorY + 1, 1000}, {0.35f, 0.55f, 1.f, 1});  // Z blue
  }
  const std::size_t worldLines = lines.size();
  if (options.skeleton && viewer.hasModel()) {
    const auto &j = viewer.joints();
    const auto &p = viewer.parents();
    for (std::size_t i = 0; i < j.size() && i < p.size(); ++i)
      if (p[i] != 255 && p[i] < j.size()) pushLine(lines, j[p[i]], j[i], {1.f, 0.78f, 0.25f, 1});
  }
  // the editor's handles and gizmo (over the model as the bones)
  if (!options.handles.empty()) {
    std::array<float, 3> f{camera.target[0] - eye[0], camera.target[1] - eye[1], camera.target[2] - eye[2]};
    const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (auto &v : f) v /= fl;
    std::array<float, 3> r{-f[2], 0, f[0]};
    const float rl = std::max(1e-6f, std::sqrt(r[0] * r[0] + r[2] * r[2]));
    r = {r[0] / rl, 0, r[2] / rl};
    const std::array<float, 3> u{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    auto ring = [&](const std::array<float, 3> &c, const std::array<float, 3> &a, const std::array<float, 3> &b,
                    float radius, const std::array<float, 4> &col) {
      const int n = 24;
      for (int k = 0; k < n; ++k) {
        const float t0 = 6.2831853f * float(k) / n, t1 = 6.2831853f * float(k + 1) / n;
        std::array<float, 3> p0, p1;
        for (int i = 0; i < 3; ++i) {
          p0[i] = c[i] + radius * (std::cos(t0) * a[i] + std::sin(t0) * b[i]);
          p1[i] = c[i] + radius * (std::cos(t1) * a[i] + std::sin(t1) * b[i]);
        }
        pushLine(lines, p0, p1, col);
      }
    };
    const float small = camera.distance * 0.009f;
    for (std::size_t i = 0; i < options.handles.size(); ++i) {
      const bool on = i < options.handleParts.size() && options.handleParts[i] == options.selected;
      ring(options.handles[i], r, u, on ? small * 1.8f : small,
           on ? std::array<float, 4>{1, 1, 1, 1} : std::array<float, 4>{0.35f, 0.85f, 1.f, 1});
      if (on && options.gizmo) {
        const auto &c = options.handles[i];
        const float g = GizmoSize(camera.distance);
        const std::array<float, 4> colours[3] = {{0.95f, 0.3f, 0.3f, 1}, {0.4f, 0.9f, 0.4f, 1}, {0.35f, 0.55f, 1.f, 1}};
        for (int a = 0; a < 3; ++a) {
          const auto &ax = options.gizmoAxes[std::size_t(a)];
          if (options.gizmo == 1) {
            const std::array<float, 3> tip{c[0] + ax[0] * g, c[1] + ax[1] * g, c[2] + ax[2] * g};
            pushLine(lines, c, tip, colours[a]);
            ring(tip, r, u, small * 0.9f, colours[a]);
          } else {
            // the ring around axis a: two directions across it
            const auto &b0 = options.gizmoAxes[std::size_t((a + 1) % 3)], &b1 = options.gizmoAxes[std::size_t((a + 2) % 3)];
            ring(c, b0, b1, g, colours[a]);
          }
        }
      }
    }
  }
  if (!lines.empty()) {
    glUseProgram(lineProgram_);
    glUniformMatrix4fv(glGetUniformLocation(lineProgram_, "viewProjection"), 1, GL_FALSE, viewProjection_.data());
    glBindVertexArray(lineVao_);
    glBindBuffer(GL_ARRAY_BUFFER, lineBuffer_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(lines.size() * sizeof(float)), lines.data(), GL_STREAM_DRAW);
    glLineWidth(1.f);
    glDrawArrays(GL_LINES, 0, GLsizei(worldLines / 7));
    if (lines.size() > worldLines) {
      glDisable(GL_DEPTH_TEST);  // the bones over the model
      glDrawArrays(GL_LINES, GLsizei(worldLines / 7), GLsizei((lines.size() - worldLines) / 7));
      glEnable(GL_DEPTH_TEST);
    }
  }
  glBindVertexArray(0);
}

}  // namespace re4::studio
