package com.ast.re4decomp;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.util.Arrays;
import java.util.List;

/** ModSwitch without Android: packs on and off, one pack per file. javac + java (see .github/workflows/build-apk.yml). */
public final class ModSwitchTests {
  private static int checks, failed;

  private static void check(boolean ok, String what) {
    checks++;
    if (!ok) {
      failed++;
      System.out.println("FAIL: " + what);
    }
  }

  private static void file(File f) throws IOException {
    f.getParentFile().mkdirs();
    try (FileOutputStream o = new FileOutputStream(f)) { o.write(1); }
  }

  private static void delete(File f) {
    File[] all = f.listFiles();
    if (all != null) for (File c : all) delete(c);
    f.delete();
  }

  public static void main(String[] args) throws IOException {
    File mods = Files.createTempDirectory("modswitch").toFile();
    try {
      file(new File(mods, "Leon Asia/em/pl08.das"));
      file(new File(mods, "Leon Asia/em/pl00.das"));
      file(new File(mods, "Leon Requiem/files/EM/PL08.das"));   // files/ and other case: the same disc file
      file(new File(mods, "Leon Requiem/em/wep16.das"));
      file(new File(mods, "Ashley/em/pl01.das"));
      file(new File(mods, ".incoming/em/pl08.das"));             // a copy under way: no pack

      check(ModSwitch.files(new File(mods, "Leon Requiem")).equals(new java.util.HashSet<>(Arrays.asList("em/pl08.das"))),
          "files/ of a pack, lower case");
      check(ModSwitch.isOn(new File(mods, "Leon Asia")) && ModSwitch.isOn(new File(mods, "Leon Requiem")), "on at first");

      List<String> off = ModSwitch.set(mods, "Leon Requiem", true);
      check(off.equals(Arrays.asList("Leon Asia")), "switching on switches off the pack with the same file: " + off);
      check(!ModSwitch.isOn(new File(mods, "Leon Asia")) && ModSwitch.isOn(new File(mods, "Ashley")), "Ashley stays on");
      check(new File(mods, "Leon Asia/" + ModSwitch.OFF).isFile() && ModSwitch.files(new File(mods, "Leon Asia")).size() == 2,
          "the marker is a hidden file, not one of the pack's");

      off = ModSwitch.set(mods, "Leon Asia", true);
      check(off.equals(Arrays.asList("Leon Requiem")) && ModSwitch.isOn(new File(mods, "Leon Asia")), "and back: " + off);

      off = ModSwitch.set(mods, "Ashley", false);
      check(off.isEmpty() && !ModSwitch.isOn(new File(mods, "Ashley")), "off");
      off = ModSwitch.set(mods, "Ashley", false);
      check(off.isEmpty() && !ModSwitch.isOn(new File(mods, "Ashley")), "off twice");
      off = ModSwitch.set(mods, "Ashley", true);
      check(off.isEmpty() && ModSwitch.isOn(new File(mods, "Ashley")), "on without others");

      boolean threw = false;
      try {
        ModSwitch.set(mods, "missing", true);
      } catch (IOException e) {
        threw = true;
      }
      check(threw, "a missing pack");
    } finally {
      delete(mods);
    }
    System.out.println("ModSwitchTests: " + checks + " checks, " + failed + " failed");
    if (failed > 0) System.exit(1);
  }
}
