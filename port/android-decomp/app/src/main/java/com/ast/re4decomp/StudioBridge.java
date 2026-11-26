package com.ast.re4decomp;

/**
 * CubeEvil4 Studio's native viewer (cpp/studio_jni.cpp, the library libre4studio: the Studio's own APK loads only it,
 * without the game core): archives, models, animations, the GL picture.
 */
final class StudioBridge {
  private StudioBridge() {}

  private static boolean loaded;

  static synchronized void load() {
    if (!loaded) {
      System.loadLibrary("re4studio");
      loaded = true;
    }
  }

  /** "" or why not; `name` the file's own name (the path may be /proc/self/fd/N). */
  static native String openArchive(String path, String name);
  /** "index\tkind\tbytes" per entry of the archive opened. */
  static native String[] entries();
  static native String loadModel(int bin, int tpl);
  /** The whole model as the game puts it together (or the archive's first model): "" or why not. */
  static native String loadComplete();
  /** The pieces on the view, the main model first: "entry\tname". */
  static native String[] parts();
  /** The names the game gives the archive's models: "entry\tname" (none for an archive it does not know). */
  static native String[] partNames();
  /** The piece drawn tinted (its place in parts()), -1 none. */
  static native void highlight(int piece);
  static native String togglePiece(int bin);
  static native int useMotions(String path, String name);
  static native String lastError();
  static native String[] motionNames();
  static native String setMotion(int clip);
  static native float motionLength();
  static native void setInPlace(boolean on);
  static native float[] frameModel();
  static native String info();
  /** An .lfs file of the PC release unpacked to `out` (copied when it is not packed): "" or why not. */
  static native String unpackLfs(String in, String out);
  /**
   * A PC skin (an unpacked em/plXX.udas) converted into the disc's archive `gc` (its name `gcName`), the textures
   * from the unpacked .pack files `packs`, `tex` the largest texture side: "ok\n" + report or "error\n" + why.
   */
  static native String convertSkin(String gc, String gcName, String pc, String[] packs, int tex, String out);
  /** A GTA San Andreas ped skin (.dff, its .txd or "") on Leon's disc archive: "ok\n" + report or "error\n" + why. */
  static native String convertDff(String gc, String gcName, String dff, String txd, int tex, boolean keepHead,
      boolean fitLeon, String out);
  /** A rigged model as a binary .fbx (its pictures, .png / .tga, in the same folder) on the character's disc archive as
   *  a GTA skin: "ok\n" + report or "error\n" + why. */
  static native String convertFbx(String gc, String gcName, String fbx, int tex, boolean keepHead, String out);
  /** An XNALara model (the classic binary generic_item.mesh, its pictures in the same folder), as convertFbx. */
  static native String convertXps(String gc, String gcName, String mesh, int tex, boolean keepHead, String out);
  /**
   * An RE4 Remake (PC) character mod on Leon's disc archive: `dirs` its folders (natives/STM/... inside; the base,
   * then a colour variant laid over it): "ok\n" + report or "error\n" + why.
   */
  static native String convertReMod(String gc, String gcName, String[] dirs, int tex, boolean keepHead,
      boolean fitLeon, String out);
  /**
   * An RE4 Remake (PC) weapon mod into the disc's weapon archive (em/wepNN.drs): `dirs` its folders (natives/STM/...
   * inside; the base, then a colour variant), `wp` the weapon's folder (wp4000 ...): "ok\n" + report or "error\n" + why.
   */
  static native String convertReWeapon(String gc, String gcName, String[] dirs, String wp, int tex, String out);
  /** The disc's weapon archive (wep04 ...) an RE4 Remake weapon (wp4000 ...) replaces; "" none known. */
  static native String reWeaponTarget(String wp);
  static native void surfaceCreated();
  static native void draw(int width, int height, float yaw, float pitch, float distance, float tx, float ty, float tz,
      int flags, float frame);
}
