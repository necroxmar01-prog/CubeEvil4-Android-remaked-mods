package com.ast.re4decomp;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * Mods switched on and off in the menu (wish of the project owner, 09.11.2026: several skins of one character kept,
 * one used). A pack holding the file {@value #OFF} is switched off; the core leaves it out
 * (port/whole/runtime/dvd.cpp applyMods). Only one pack replaces a file: switching one on switches off the others
 * that replace any of its files. Plain java.io, so the tests run without Android (tests/ModSwitchTests.java).
 */
final class ModSwitch {
  static final String OFF = ".off";

  private ModSwitch() {}

  static boolean isOn(File pack) { return !new File(pack, OFF).exists(); }

  /** The disc paths a pack replaces, lower case ("em/pl08.das"); a pack may hold them in files/. */
  static Set<String> files(File pack) {
    File files = new File(pack, "files");
    Set<String> out = new HashSet<>();
    collect(files.isDirectory() ? files : pack, "", out, 0);
    return out;
  }

  private static void collect(File dir, String rel, Set<String> out, int depth) {
    File[] all = dir.listFiles();
    if (all == null || depth > 16) return;
    for (File f : all) {
      if (f.getName().startsWith(".")) continue;
      String name = rel + f.getName().toLowerCase(Locale.ROOT);
      if (f.isDirectory()) collect(f, name + "/", out, depth + 1);
      else out.add(name);
    }
  }

  /**
   * Switches the pack `name` in `mods` on or off; on also switches off every other pack that is on and replaces one
   * of its files. Returns those, in name order.
   */
  static List<String> set(File mods, String name, boolean on) throws IOException {
    File pack = new File(mods, name), marker = new File(pack, OFF);
    List<String> off = new ArrayList<>();
    if (!pack.isDirectory()) throw new IOException("no mod " + name);
    if (!on) {
      if (!marker.exists() && !marker.createNewFile()) throw new IOException("cannot switch off " + name);
      return off;
    }
    if (marker.exists() && !marker.delete()) throw new IOException("cannot switch on " + name);
    Set<String> mine = files(pack);
    File[] all = mods.listFiles();
    if (all == null) return off;
    Arrays.sort(all);
    for (File other : all) {
      if (!other.isDirectory() || other.getName().startsWith(".") || other.getName().equals(name) || !isOn(other)) {
        continue;
      }
      Set<String> theirs = files(other);
      theirs.retainAll(mine);
      if (theirs.isEmpty()) continue;
      if (!new File(other, OFF).createNewFile()) throw new IOException("cannot switch off " + other.getName());
      off.add(other.getName());
    }
    return off;
  }
}
