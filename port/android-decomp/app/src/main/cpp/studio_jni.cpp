// CubeEvil4 Studio: the Java side (StudioBridge / StudioActivity) and the viewer (studio_model.cpp, studio_gl.cpp).
// The UI thread opens archives, loads models and picks animations; the GL thread poses and draws. One lock for both.
#include "studio_convert.h"
#include "studio_gl.h"
#include "studio_motion_edit.h"
#include <dirent.h>
#include <fstream>
#include <sys/stat.h>
#include <android/log.h>
#include <jni.h>
#include <mutex>

namespace {

using namespace re4::studio;

std::mutex gLock;
Archive gArchive;      // the archive of the models list
Viewer gViewer;
GlView gView;
bool gViewReady = false;
bool gUpload = false;  // a new model: its textures and uvs to the GPU
std::string gError;
motionedit::Editor gEdit;  // the Edit tab's animations (studio_motion_edit.cpp)

std::string text(JNIEnv *env, jstring s) {
  if (!s) return {};
  const char *c = env->GetStringUTFChars(s, nullptr);
  std::string out = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return out;
}

jobjectArray strings(JNIEnv *env, const std::vector<std::string> &list) {
  jobjectArray out = env->NewObjectArray(jsize(list.size()), env->FindClass("java/lang/String"), nullptr);
  for (std::size_t i = 0; i < list.size(); ++i) {
    jstring s = env->NewStringUTF(list[i].c_str());
    env->SetObjectArrayElement(out, jsize(i), s);
    env->DeleteLocalRef(s);
  }
  return out;
}

void log(const std::string &what) { __android_log_print(ANDROID_LOG_INFO, "RE4Studio", "%s", what.c_str()); }

// the game's own part for a BIN of the open archive (its palette and name), if it is one
PartSpec specFor(unsigned bin) {
  for (const auto &p : GameParts(gArchive.name(), gArchive))
    if (p.bin == bin) return p;
  return {bin, -1, ""};
}

}  // namespace

#define STUDIO(name) Java_com_ast_re4decomp_StudioBridge_##name

extern "C" {

// "" or why the archive cannot be shown
JNIEXPORT jstring JNICALL STUDIO(openArchive)(JNIEnv *env, jclass, jstring path, jstring name) {
  std::lock_guard<std::mutex> lock(gLock);
  std::string why;
  const bool ok = gArchive.open(text(env, path), &why, text(env, name));
  log("archive " + text(env, path) + (ok ? ": " + std::to_string(gArchive.entries().size()) + " entries" : ": " + why));
  return env->NewStringUTF(ok ? "" : why.c_str());
}

// the archive's entries: "index\tkind\tbytes"
JNIEXPORT jobjectArray JNICALL STUDIO(entries)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  std::vector<std::string> out;
  for (const auto &e : gArchive.entries())
    out.push_back(std::to_string(e.index) + "\t" + e.kind + "\t" + std::to_string(e.bytes));
  return strings(env, out);
}

JNIEXPORT jstring JNICALL STUDIO(loadModel)(JNIEnv *env, jclass, jint bin, jint tpl) {
  std::lock_guard<std::mutex> lock(gLock);
  std::string why;
  const auto spec = specFor(unsigned(bin));
  const bool ok = gViewer.loadModel(gArchive, unsigned(bin), tpl >= 0 ? tpl : spec.tpl, &why);
  if (ok) {
    gViewer.clearMotions();
    gViewer.addMotions(gArchive, nullptr);  // the model's own archive's animations first
    gUpload = true;
  }
  log("model #" + std::to_string(bin) + (ok ? ": " + std::to_string(gViewer.triangles()) + " triangles" : ": " + why));
  return env->NewStringUTF(ok ? "" : why.c_str());
}

// the whole model as the game puts it together (the archive's character: GameParts), else its first model; "" or why
JNIEXPORT jstring JNICALL STUDIO(loadComplete)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  std::string why;
  auto specs = GameParts(gArchive.name(), gArchive);
  if (specs.empty()) {
    const auto bins = gArchive.of("BIN");
    if (bins.empty()) return env->NewStringUTF("this archive holds no models");
    specs.push_back({bins[0], -1, "Model #" + std::to_string(bins[0])});
  }
  const bool ok = gViewer.loadParts(gArchive, specs, &why);
  if (ok) {
    gViewer.clearMotions();
    gViewer.addMotions(gArchive, nullptr);
    gUpload = true;
  }
  log("complete model of " + gArchive.name() + (ok ? ": " + std::to_string(gViewer.pieces().size()) + " pieces, " +
      std::to_string(gViewer.triangles()) + " triangles" : ": " + why));
  return env->NewStringUTF(ok ? "" : why.c_str());
}

// the pieces shown: "entry\tname" (the main model first)
JNIEXPORT jobjectArray JNICALL STUDIO(parts)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  std::vector<std::string> out;
  const auto entries = gViewer.pieceEntries();
  const auto names = gViewer.pieces();
  for (std::size_t i = 0; i < entries.size() && i < names.size(); ++i)
    out.push_back(std::to_string(entries[i]) + "\t" + names[i]);
  return strings(env, out);
}

// the names the game gives the open archive's models: "entry\tname" (none for an archive it does not know)
JNIEXPORT jobjectArray JNICALL STUDIO(partNames)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  std::vector<std::string> out;
  for (const auto &p : GameParts(gArchive.name(), gArchive)) out.push_back(std::to_string(p.bin) + "\t" + p.label);
  return strings(env, out);
}

// the piece drawn tinted (its place in parts()), -1 none
JNIEXPORT void JNICALL STUDIO(highlight)(JNIEnv *, jclass, jint piece) {
  std::lock_guard<std::mutex> lock(gLock);
  gViewer.setHighlight(piece);
}

// "added", "removed" or why not
JNIEXPORT jstring JNICALL STUDIO(togglePiece)(JNIEnv *env, jclass, jint bin) {
  std::lock_guard<std::mutex> lock(gLock);
  std::string why;
  bool added = false;
  const auto spec = specFor(unsigned(bin));
  const bool ok = gViewer.togglePiece(gArchive, unsigned(bin), spec.tpl, &added, &why, spec.label);
  if (ok) gUpload = true;
  return env->NewStringUTF(ok ? (added ? "added" : "removed") : why.c_str());
}

// the animations of another archive (a disc archive or a mod pack's .dat) in place of the list: how many, -1 none
JNIEXPORT jint JNICALL STUDIO(useMotions)(JNIEnv *env, jclass, jstring path, jstring name) {
  std::lock_guard<std::mutex> lock(gLock);
  Archive a;
  std::string why;
  if (!a.open(text(env, path), &why, text(env, name))) {
    gError = why;
    return -1;
  }
  gViewer.clearMotions();
  const int n = gViewer.addMotions(a, &why);
  if (!n) gError = why;
  return n ? n : -1;
}

// ---- the converter (studio_convert.cpp): UHD skins of the PC release
namespace {
bool readAll(const std::string &path, convert::Bytes &out) { return ReadWhole(path, out, 512u * 1024 * 1024); }
bool writeAll(const std::string &path, const convert::Bytes &data) {
  std::ofstream o(path, std::ios::binary | std::ios::trunc);
  return o && o.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
}
}  // namespace

// An .lfs file of the PC release unpacked to `out`: "" or why not
JNIEXPORT jstring JNICALL STUDIO(unpackLfs)(JNIEnv *env, jclass, jstring in, jstring out) {
  convert::Bytes data;
  if (!readAll(text(env, in), data)) return env->NewStringUTF("cannot be read");
  try {
    const auto unpacked = convert::IsLfs(data) ? convert::UnpackLfs(data) : data;
    if (!writeAll(text(env, out), unpacked)) return env->NewStringUTF("cannot be written");
  } catch (const std::exception &e) {
    return env->NewStringUTF(e.what());
  }
  return env->NewStringUTF("");
}

// A PC skin (`pc`: em/plXX.udas or .udas.lfs) into the disc's archive (`gc`, its name `gcName`), the textures from the
// .pack files `packs`, written to `out`: "ok\n" + the report, or "error\n" + why (+ the report so far)
JNIEXPORT jstring JNICALL STUDIO(convertSkin)(JNIEnv *env, jclass, jstring gc, jstring gcName, jstring pc,
                                             jobjectArray packs, jint tex, jstring out) {
  convert::Bytes g, p, result;
  if (!readAll(text(env, gc), g)) return env->NewStringUTF("error\nthe disc's archive cannot be read");
  if (!readAll(text(env, pc), p)) return env->NewStringUTF("error\nthe mod's archive cannot be read");
  convert::Packs pk;
  const jsize n = packs ? env->GetArrayLength(packs) : 0;
  for (jsize i = 0; i < n; ++i) {
    auto s = static_cast<jstring>(env->GetObjectArrayElement(packs, i));
    pk.add(text(env, s));
    env->DeleteLocalRef(s);
  }
  convert::Options o;
  if (tex >= 8) o.tex = tex;
  std::string log, why;
  if (!convert::ConvertSkin(g, text(env, gcName), p, pk, o, result, log, &why))
    return env->NewStringUTF(("error\n" + why + "\n" + log).c_str());
  if (!writeAll(text(env, out), result)) return env->NewStringUTF("error\nthe result cannot be written");
  __android_log_print(ANDROID_LOG_INFO, "RE4Studio", "converted %s: %zu bytes", text(env, gcName).c_str(), result.size());
  return env->NewStringUTF(("ok\n" + log).c_str());
}

// A GTA San Andreas ped skin (`dff`, its textures `txd`: "" none) on Leon's archive of the disc (`gc`, its name
// `gcName`), written to `out`: "ok\n" + the report, or "error\n" + why (+ the report so far)
JNIEXPORT jstring JNICALL STUDIO(convertDff)(JNIEnv *env, jclass, jstring gc, jstring gcName, jstring dff, jstring txd,
                                            jint tex, jboolean keepHead, jboolean fitLeon, jstring out) {
  convert::Bytes g, d, t, result;
  if (!readAll(text(env, gc), g)) return env->NewStringUTF("error\nthe disc's archive cannot be read");
  if (!readAll(text(env, dff), d)) return env->NewStringUTF("error\nthe .dff cannot be read");
  const std::string txdPath = text(env, txd);
  if (!txdPath.empty() && !readAll(txdPath, t)) return env->NewStringUTF("error\nthe .txd cannot be read");
  convert::Options o;
  if (tex >= 8) o.tex = tex;
  o.keepHead = keepHead;
  (void) fitLeon;  // GTA skins keep their own build (DevJunior, 20.11.2026: theirs looked best)
  std::string log, why;
  if (!convert::ConvertDff(g, text(env, gcName), d, t, o, result, log, &why))
    return env->NewStringUTF(("error\n" + why + "\n" + log).c_str());
  if (!writeAll(text(env, out), result)) return env->NewStringUTF("error\nthe result cannot be written");
  return env->NewStringUTF(("ok\n" + log).c_str());
}

// the pictures (.png, .tga, .jpg) in the folder of `path`, by their lower-case names
static std::map<std::string, convert::Bytes> picturesBeside(const std::string &path) {
  std::map<std::string, convert::Bytes> files;
  const std::string dir = path.substr(0, path.find_last_of('/'));
  if (DIR *d = opendir(dir.c_str())) {
    while (dirent *e = readdir(d)) {
      std::string low = e->d_name;
      for (auto &c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
      const auto dot = low.find_last_of('.');
      const std::string ext = dot == std::string::npos ? "" : low.substr(dot);
      if (ext != ".png" && ext != ".tga" && ext != ".jpg" && ext != ".jpeg") continue;
      convert::Bytes b;
      if (readAll(dir + "/" + e->d_name, b)) files[low] = std::move(b);
    }
    closedir(d);
  }
  return files;
}

// A rigged model (`model`: a binary .fbx, or XNALara's generic_item.mesh when `xps`; its pictures in its folder) on the
// character's archive of the disc (`gc`, its name `gcName`) as a GTA skin, written to `out`: "ok\n" + the report, or
// "error\n" + why
static jstring convertRig(JNIEnv *env, jstring gc, jstring gcName, jstring model, jint tex, jboolean keepHead, jstring out,
                          bool xps) {
  convert::Bytes g, f, result;
  if (!readAll(text(env, gc), g)) return env->NewStringUTF("error\nthe disc's archive cannot be read");
  const std::string path = text(env, model);
  if (!readAll(path, f)) return env->NewStringUTF("error\nthe model cannot be read");
  const auto files = picturesBeside(path);
  convert::Options o;
  if (tex >= 8) o.tex = tex;
  o.keepHead = keepHead;
  std::string log, why;
  const bool ok = xps ? convert::ConvertXps(g, text(env, gcName), f, files, o, result, log, &why)
                      : convert::ConvertFbx(g, text(env, gcName), f, files, o, result, log, &why);
  if (!ok) return env->NewStringUTF(("error\n" + why + "\n" + log).c_str());
  if (!writeAll(text(env, out), result)) return env->NewStringUTF("error\nthe result cannot be written");
  return env->NewStringUTF(("ok\n" + log).c_str());
}

JNIEXPORT jstring JNICALL STUDIO(convertFbx)(JNIEnv *env, jclass, jstring gc, jstring gcName, jstring fbx, jint tex,
                                            jboolean keepHead, jstring out) {
  return convertRig(env, gc, gcName, fbx, tex, keepHead, out, false);
}

JNIEXPORT jstring JNICALL STUDIO(convertXps)(JNIEnv *env, jclass, jstring gc, jstring gcName, jstring mesh, jint tex,
                                            jboolean keepHead, jstring out) {
  return convertRig(env, gc, gcName, mesh, tex, keepHead, out, true);
}

// the RE4 Remake mod's files below `dir` (natives/STM/... inside): their path after "natives/stm/" in lower case;
// only meshes, materials and textures
static void readReFolder(const std::string &dir, const std::string &rel, std::map<std::string, convert::Bytes> &files) {
  DIR *d = opendir(dir.c_str());
  if (!d) return;
  while (dirent *e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    const std::string path = dir + "/" + name;
    std::string low = rel.empty() ? name : rel + "/" + name;
    for (auto &c : low) c = char(std::tolower(static_cast<unsigned char>(c)));
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) continue;
    if (S_ISDIR(st.st_mode)) {
      readReFolder(path, low, files);
      continue;
    }
    const auto at = low.find("natives/stm/");
    if (at == std::string::npos) continue;
    if (low.find(".mesh.") == std::string::npos && low.find(".mdf2.") == std::string::npos &&
        low.find(".tex.") == std::string::npos)
      continue;
    convert::Bytes b;
    if (readAll(path, b)) files[low.substr(at + 12)] = std::move(b);
  }
  closedir(d);
}

// An RE4 Remake mod (its folders `dirs`: the base, then a variant laid over it) on Leon's archive of the disc
// (`gc`, its name `gcName`), written to `out`: "ok\n" + the report, or "error\n" + why (+ the report so far)
JNIEXPORT jstring JNICALL STUDIO(convertReMod)(JNIEnv *env, jclass, jstring gc, jstring gcName, jobjectArray dirs,
                                              jint tex, jboolean keepHead, jboolean fitLeon, jstring out) {
  convert::Bytes g, result;
  if (!readAll(text(env, gc), g)) return env->NewStringUTF("error\nthe disc's archive cannot be read");
  std::map<std::string, convert::Bytes> files;
  for (jsize i = 0; i < env->GetArrayLength(dirs); ++i) {
    auto *s = static_cast<jstring>(env->GetObjectArrayElement(dirs, i));
    readReFolder(text(env, s), "", files);
    env->DeleteLocalRef(s);
  }
  convert::Options o;
  if (tex >= 8) o.tex = tex;
  o.keepHead = keepHead;
  o.fitBody = fitLeon ? 0.6 : 0.0;
  o.trunkToHead = fitLeon;  // with the slimmer build: the GTA skins' smaller trunk and head
  std::string log, why;
  if (!convert::ConvertReMod(g, text(env, gcName), files, o, result, log, &why))
    return env->NewStringUTF(("error\n" + why + "\n" + log).c_str());
  if (!writeAll(text(env, out), result)) return env->NewStringUTF("error\nthe result cannot be written");
  return env->NewStringUTF(("ok\n" + log).c_str());
}

// An RE4 Remake weapon mod (its folders `dirs`, the weapon `wp`: wp4000 ...; "" the first) into the disc's weapon
// archive (`gc`, em/wepNN.drs), written to `out`: "ok\n" + the report, or "error\n" + why
JNIEXPORT jstring JNICALL STUDIO(convertReWeapon)(JNIEnv *env, jclass, jstring gc, jstring gcName, jobjectArray dirs,
                                                 jstring wp, jint tex, jstring out) {
  convert::Bytes g, result;
  if (!readAll(text(env, gc), g)) return env->NewStringUTF("error\nthe disc's archive cannot be read");
  std::map<std::string, convert::Bytes> files;
  for (jsize i = 0; i < env->GetArrayLength(dirs); ++i) {
    auto *s = static_cast<jstring>(env->GetObjectArrayElement(dirs, i));
    readReFolder(text(env, s), "", files);
    env->DeleteLocalRef(s);
  }
  convert::Options o;
  if (tex >= 8) o.tex = tex;
  std::string log, why;
  if (!convert::ConvertReWeapon(g, text(env, gcName), files, text(env, wp), o, result, log, &why))
    return env->NewStringUTF(("error\n" + why + "\n" + log).c_str());
  if (!writeAll(text(env, out), result)) return env->NewStringUTF("error\nthe result cannot be written");
  return env->NewStringUTF(("ok\n" + log).c_str());
}

// The disc's weapon archive (wep04 ...) an RE4 Remake weapon replaces (its folder wp4000 ...); "" none known
JNIEXPORT jstring JNICALL STUDIO(reWeaponTarget)(JNIEnv *env, jclass, jstring wp) {
  return env->NewStringUTF(convert::ReWeaponTarget(text(env, wp)));
}

// ---- the animation editor (Edit tab; studio_motion_edit.cpp)
// a motion archive's body (move.dat, finish.dat ...) to edit: "" or why not
JNIEXPORT jstring JNICALL STUDIO(editOpen)(JNIEnv *env, jclass, jstring path) {
  std::lock_guard<std::mutex> lock(gLock);
  convert::Bytes data;
  std::string why;
  if (!ReadWhole(text(env, path), data, 16u * 1024 * 1024)) return env->NewStringUTF("cannot be read");
  return env->NewStringUTF(gEdit.open(data, &why) ? "" : why.c_str());
}

// its animations: "entry|last frame|name"
JNIEXPORT jobjectArray JNICALL STUDIO(editClips)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  std::vector<std::string> out;
  for (const auto &c : gEdit.clips()) out.push_back(std::to_string(c.entry) + "|" + std::to_string(c.frames) + "|" + c.name);
  return strings(env, out);
}

// the bones with a rotation (pos false) or a position track in the animation
JNIEXPORT jintArray JNICALL STUDIO(editParts)(JNIEnv *env, jclass, jint entry, jboolean pos) {
  std::lock_guard<std::mutex> lock(gLock);
  const auto p = gEdit.parts(unsigned(entry), pos);
  jintArray out = env->NewIntArray(jsize(p.size()));
  if (!p.empty()) env->SetIntArrayRegion(out, 0, jsize(p.size()), p.data());
  return out;
}

// turns a bone (radians) or moves it (mm) from frame f0 to f1, easing over `fade` frames: "" or why not
JNIEXPORT jstring JNICALL STUDIO(editAdjust)(JNIEnv *env, jclass, jint entry, jint part, jboolean pos, jfloat x,
                                             jfloat y, jfloat z, jint f0, jint f1, jint fade) {
  std::lock_guard<std::mutex> lock(gLock);
  const float d[3] = {x, y, z};
  std::string why;
  return env->NewStringUTF(gEdit.adjust(unsigned(entry), part, pos, d, f0, f1, fade, &why) ? "" : why.c_str());
}

// IK: the hand or foot `end` (0 right hand, 1 left hand, 2 right foot, 3 left foot) moved by x, y, z mm (model space)
// over frames f0..f1 on the model's skeleton: "" done, "!..." done but out of reach, else why not
JNIEXPORT jstring JNICALL STUDIO(editReach)(JNIEnv *env, jclass, jint entry, jint end, jfloat x, jfloat y, jfloat z,
                                            jint f0, jint f1, jint fade) {
  std::lock_guard<std::mutex> lock(gLock);
  const auto *joints = gViewer.bindJoints();
  if (!joints) return env->NewStringUTF("load a model first (Models tab)");
  motionedit::Skeleton s;
  for (const auto &j : *joints) {
    s.parent.push_back(j.parent == 255 ? -1 : int(j.parent));
    s.bind.push_back(j.center);
  }
  const float d[3] = {x, y, z};
  std::string why;
  if (!gEdit.reach(unsigned(entry), s, end, d, f0, f1, fade, &why)) return env->NewStringUTF(why.c_str());
  if (gEdit.lastReachMiss() > 5.0f) {
    char line[96];
    std::snprintf(line, sizeof(line), "!out of reach: up to %.0f mm short", double(gEdit.lastReachMiss()));
    return env->NewStringUTF(line);
  }
  return env->NewStringUTF("");
}

// cut: keeps frames f0..f1 of the animation ("" or why not)
JNIEXPORT jstring JNICALL STUDIO(editTrim)(JNIEnv *env, jclass, jint entry, jint f0, jint f1) {
  std::lock_guard<std::mutex> lock(gLock);
  std::string why;
  return env->NewStringUTF(gEdit.trim(unsigned(entry), f0, f1, &why) ? "" : why.c_str());
}

JNIEXPORT jstring JNICALL STUDIO(editSpeed)(JNIEnv *env, jclass, jint entry, jfloat speed) {
  std::lock_guard<std::mutex> lock(gLock);
  std::string why;
  return env->NewStringUTF(gEdit.retime(unsigned(entry), speed, &why) ? "" : why.c_str());
}

// how many steps can be undone after this one (-1: nothing was undone)
JNIEXPORT jint JNICALL STUDIO(editUndo)(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  return gEdit.undo() ? gEdit.undoCount() : -1;
}

// the edited archive written to `path` ("" or why not)
JNIEXPORT jstring JNICALL STUDIO(editSave)(JNIEnv *env, jclass, jstring path) {
  std::lock_guard<std::mutex> lock(gLock);
  if (!gEdit.isOpen()) return env->NewStringUTF("nothing open");
  std::ofstream o(text(env, path), std::ios::binary | std::ios::trunc);
  const auto b = gEdit.save();
  if (!o || !o.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size()))) return env->NewStringUTF("cannot be written");
  return env->NewStringUTF("");
}

// the edited archive written to `path` and the animation `entry` of it on the model: "" or why not
JNIEXPORT jstring JNICALL STUDIO(editPreview)(JNIEnv *env, jclass, jstring path, jint entry) {
  std::lock_guard<std::mutex> lock(gLock);
  if (!gEdit.isOpen()) return env->NewStringUTF("nothing open");
  const std::string file = text(env, path);
  {
    std::ofstream o(file, std::ios::binary | std::ios::trunc);
    const auto b = gEdit.save();
    if (!o || !o.write(reinterpret_cast<const char *>(b.data()), std::streamsize(b.size())))
      return env->NewStringUTF("cannot be written");
  }
  Archive a;
  std::string why;
  if (!a.open(file, &why, "edit.dat")) return env->NewStringUTF(why.c_str());
  gViewer.clearMotions();
  gViewer.addMotions(a, &why);
  const std::string tail = " #" + std::to_string(entry);
  for (int i = 0; i < gViewer.motionCount(); ++i) {
    const std::string n = gViewer.motionName(i);
    if (n.size() >= tail.size() && n.compare(n.size() - tail.size(), tail.size(), tail) == 0) {
      if (!gViewer.setMotion(i, &why)) return env->NewStringUTF(why.empty() ? "it does not fit this skeleton" : why.c_str());
      return env->NewStringUTF("");
    }
  }
  return env->NewStringUTF("the animation is not in the written file");
}

JNIEXPORT jstring JNICALL STUDIO(lastError)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  return env->NewStringUTF(gError.c_str());
}

JNIEXPORT jobjectArray JNICALL STUDIO(motionNames)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  std::vector<std::string> out;
  for (int i = 0; i < gViewer.motionCount(); ++i) out.push_back(gViewer.motionName(i));
  return strings(env, out);
}

// "" or why the animation does not fit the model
JNIEXPORT jstring JNICALL STUDIO(setMotion)(JNIEnv *env, jclass, jint clip) {
  std::lock_guard<std::mutex> lock(gLock);
  std::string why;
  const bool ok = gViewer.setMotion(clip, &why);
  return env->NewStringUTF(ok ? "" : (why.empty() ? "it does not fit this skeleton" : why.c_str()));
}

JNIEXPORT jfloat JNICALL STUDIO(motionLength)(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  return gViewer.motionLength();
}

JNIEXPORT void JNICALL STUDIO(setInPlace)(JNIEnv *, jclass, jboolean on) {
  std::lock_guard<std::mutex> lock(gLock);
  gViewer.setInPlace(on);
}

// the camera that frames the model: target x, y, z and the distance
JNIEXPORT jfloatArray JNICALL STUDIO(frameModel)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  const Camera c = gViewer.hasModel() ? FrameBounds(gViewer.restMinimum(), gViewer.restMaximum()) : Camera{};
  const float v[4] = {c.target[0], c.target[1], c.target[2], c.distance};
  jfloatArray out = env->NewFloatArray(4);
  env->SetFloatArrayRegion(out, 0, 4, v);
  return out;
}

// "parts · triangles · height"
JNIEXPORT jstring JNICALL STUDIO(info)(JNIEnv *env, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  if (!gViewer.hasModel()) return env->NewStringUTF("");
  char line[160];
  std::snprintf(line, sizeof(line), "%u bones · %zu triangles · %zu pieces · %.0f mm tall", gViewer.parts(),
                gViewer.triangles(), gViewer.pieces().size(), double(gViewer.restMaximum()[1] - gViewer.restMinimum()[1]));
  return env->NewStringUTF(line);
}

// GL thread: a new surface (a new context: everything on the GPU again)
JNIEXPORT void JNICALL STUDIO(surfaceCreated)(JNIEnv *, jclass) {
  std::lock_guard<std::mutex> lock(gLock);
  std::string why;
  gViewReady = gView.create(&why);
  if (!gViewReady) log("GL: " + why);
  gUpload = true;
}

// GL thread: the pose at `frame` and the picture. flags: 1 grid, 2 axes, 4 skeleton, 8 textures
JNIEXPORT void JNICALL STUDIO(draw)(JNIEnv *, jclass, jint width, jint height, jfloat yaw, jfloat pitch,
                                    jfloat distance, jfloat tx, jfloat ty, jfloat tz, jint flags, jfloat frame) {
  std::lock_guard<std::mutex> lock(gLock);
  if (!gViewReady) return;
  if (gUpload) {
    gView.upload(gViewer);
    gUpload = false;
  }
  gViewer.pose(frame);
  Camera c;
  c.yaw = yaw;
  c.pitch = pitch;
  c.distance = distance;
  c.target = {tx, ty, tz};
  ViewOptions o;
  o.grid = flags & 1;
  o.axes = flags & 2;
  o.skeleton = flags & 4;
  o.textured = flags & 8;
  gView.draw(gViewer, width, height, c, o);
}

}  // extern "C"
