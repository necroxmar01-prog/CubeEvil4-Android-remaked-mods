package com.ast.re4decomp;

import android.content.ContentResolver;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Mods (wish of the project owner, 08.11.2026: character skins): every folder in game/mods mirrors the disc's files/
 * folder, and the core reads its files instead of the disc's (port/whole/runtime/dvd.cpp applyMods, at the start of
 * the game). The app copies a folder the player picks there, lists the packs and removes them. The files themselves
 * are the player's own and never ship with the app.
 */
final class ModStore {
  /** Larger mods than this are refused (a phone's storage; the largest disc file is far smaller). */
  static final long MAX_BYTES = 2L * 1024 * 1024 * 1024;

  private ModStore() {}

  static File dir(File gameRoot) { return new File(gameRoot, "mods"); }

  /** The packs in name order (the order the core applies them; a later one wins), hidden folders left out. */
  static String[] packs(File gameRoot) {
    File[] all = dir(gameRoot).listFiles();
    if (all == null) return new String[0];
    List<String> names = new ArrayList<>();
    for (File f : all) if (f.isDirectory() && !f.getName().startsWith(".")) names.add(f.getName());
    String[] out = names.toArray(new String[0]);
    Arrays.sort(out);
    return out;
  }

  /** Files below a folder (any depth). */
  static int countFiles(File folder) {
    File[] all = folder.listFiles();
    if (all == null) return 0;
    int n = 0;
    for (File f : all) if (!f.getName().startsWith(".")) n += f.isDirectory() ? countFiles(f) : 1;
    return n;
  }

  /** A folder name the core and every file system take: no separators or control characters, not hidden. */
  static String safeName(String name) {
    StringBuilder b = new StringBuilder();
    if (name != null) {
      for (char c : name.trim().toCharArray()) {
        b.append(c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<'
            || c == '>' || c == '|' ? '_' : c);
      }
    }
    while (b.length() > 0 && b.charAt(0) == '.') b.deleteCharAt(0);
    if (b.length() > 64) b.setLength(64);
    return b.length() == 0 ? "mod" : b.toString();
  }

  static boolean removeAll(File gameRoot) { return delete(dir(gameRoot)); }

  /** One pack goes (Mods by DevParalax, 26.11.2026: "delete mods one by one"); false when it is not there. */
  static boolean remove(File gameRoot, String name) {
    File pack = new File(dir(gameRoot), safeName(name));
    if (!pack.isDirectory() || !pack.getName().equals(name)) return false;
    return delete(pack);
  }

  /** What copyAll brought: the packs added (in order) and "name: why" of the ones that failed. */
  static final class AllResult {
    final List<Result> added = new ArrayList<>();
    final List<String> failed = new ArrayList<>();
  }

  /**
   * Every mod in the picked folder (Mods by DevParalax, 26.11.2026: "install all the mods it finds in a folder"):
   * each subfolder a pack (copied as copyTree copies a picked folder), each .zip one (copyZip); a picked folder that
   * is itself laid out like the disc (em/, files/ ... at its top) is one pack. In name order, each switched on, so
   * of two that replace the same file the later one stays on.
   */
  static AllResult copyAll(ContentResolver resolver, Uri tree, File gameRoot) {
    AllResult out = new AllResult();
    String rootId = DocumentsContract.getTreeDocumentId(tree);
    List<String[]> kids = new ArrayList<>();  // id, name, mime
    try (Cursor c = resolver.query(DocumentsContract.buildChildDocumentsUriUsingTree(tree, rootId),
        new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
      if (c == null) { out.failed.add("?: the folder cannot be read"); return out; }
      while (c.moveToNext()) {
        String name = c.getString(1);
        if (name == null || name.startsWith(".")) continue;
        kids.add(new String[] {c.getString(0), name, c.getString(2)});
      }
    }
    for (String[] k : kids) {
      if (DocumentsContract.Document.MIME_TYPE_DIR.equals(k[2]) && looksLikeDiscFolder(k[1])) {
        try {
          out.added.add(copyTree(resolver, tree, gameRoot));
        } catch (IOException | RuntimeException e) {
          out.failed.add(k[1] + ": " + e.getMessage());
        }
        return out;
      }
    }
    kids.sort((a, b) -> a[1].compareToIgnoreCase(b[1]));
    for (String[] k : kids) {
      try {
        if (DocumentsContract.Document.MIME_TYPE_DIR.equals(k[2])) {
          out.added.add(copyDir(resolver, tree, k[0], safeName(k[1]), gameRoot));
        } else if (k[1].toLowerCase(java.util.Locale.ROOT).endsWith(".zip")) {
          out.added.add(copyZip(resolver, DocumentsContract.buildDocumentUriUsingTree(tree, k[0]), gameRoot));
        }
      } catch (IOException | RuntimeException e) {
        out.failed.add(k[1] + ": " + e.getMessage());
      }
    }
    return out;
  }

  static boolean delete(File f) {
    File[] all = f.listFiles();
    if (all != null) for (File c : all) delete(c);
    return !f.exists() || f.delete();
  }

  /** What a copy brought: the pack's folder name, its files and the packs it switched off (ModSwitch). */
  static final class Result {
    final String name;
    final int files;
    final List<String> switchedOff;
    Result(String name, int files, List<String> switchedOff) {
      this.name = name; this.files = files; this.switchedOff = switchedOff;
    }
  }

  /**
   * Copies the picked folder (a document tree) to game/mods/<its name>, replacing a pack of that name. It goes to a
   * hidden folder first, so a copy that fails leaves no half pack the core would read. The new pack is switched on.
   */
  static Result copyTree(ContentResolver resolver, Uri tree, File gameRoot) throws IOException {
    String rootId = DocumentsContract.getTreeDocumentId(tree);
    String name = safeName(displayName(resolver, DocumentsContract.buildDocumentUriUsingTree(tree, rootId)));
    return copyDir(resolver, tree, rootId, name, gameRoot);
  }

  /** The folder `docId` of the picked tree as the pack `name` (copyTree, copyAll). */
  private static Result copyDir(ContentResolver resolver, Uri tree, String rootId, String name, File gameRoot)
      throws IOException {
    File mods = dir(gameRoot), staging = new File(mods, ".incoming"), target = new File(mods, name);
    delete(staging);
    if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
    long[] bytes = {0};
    int files;
    try {
      files = copyChildren(resolver, tree, rootId, staging, bytes, 0);
    } catch (IOException | RuntimeException e) {
      delete(staging);
      throw e;
    }
    if (files == 0) {
      delete(staging);
      throw new IOException("the folder holds no files");
    }
    delete(target);
    if (!staging.renameTo(target)) {
      delete(staging);
      throw new IOException("cannot create " + target);
    }
    // the new pack is on; the others that replace its files go off (one pack per file)
    return new Result(name, files, ModSwitch.set(mods, name, true));
  }

  /**
   * A .zip laid out the same way (CubeEvil4 Studio saves "<name>/em/pl01.das"), to game/mods/<its folder>, or to the
   * .zip's own name when the files are not all in one folder. Like copyTree: hidden folder first, then switched on.
   */
  static Result copyZip(ContentResolver resolver, Uri zip, File gameRoot) throws IOException {
    String zipName = displayName(resolver, zip);
    if (zipName == null) zipName = "mod";
    if (zipName.toLowerCase(java.util.Locale.ROOT).endsWith(".zip")) zipName = zipName.substring(0, zipName.length() - 4);
    File mods = dir(gameRoot), staging = new File(mods, ".incoming");
    delete(staging);
    if (!staging.mkdirs()) throw new IOException("cannot create " + staging);
    long bytes = 0;
    int files = 0;
    try (InputStream raw = resolver.openInputStream(zip)) {
      if (raw == null) throw new IOException("the file cannot be read");
      java.util.zip.ZipInputStream in = new java.util.zip.ZipInputStream(new java.io.BufferedInputStream(raw));
      for (java.util.zip.ZipEntry e; (e = in.getNextEntry()) != null;) {
        if (e.isDirectory()) continue;
        // every part a safe name, nothing hidden, nothing outside the pack
        StringBuilder rel = new StringBuilder();
        boolean skip = false;
        for (String part : e.getName().replace('\\', '/').split("/")) {
          if (part.isEmpty() || part.equals(".")) continue;
          if (part.equals("..") || part.startsWith(".") || part.equals("__MACOSX")) { skip = true; break; }
          if (rel.length() > 0) rel.append('/');
          rel.append(safeName(part));
        }
        if (skip || rel.length() == 0) continue;
        File out = new File(staging, rel.toString());
        File parent = out.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("cannot create " + parent);
        try (OutputStream o = new FileOutputStream(out)) {
          byte[] buf = new byte[1 << 16];
          for (int n; (n = in.read(buf)) > 0;) {
            bytes += n;
            if (bytes > MAX_BYTES) throw new IOException("the mod is larger than 2 GB");
            o.write(buf, 0, n);
          }
        }
        files++;
      }
    } catch (IOException | RuntimeException e) {
      delete(staging);
      throw e;
    }
    if (files == 0) {
      delete(staging);
      throw new IOException("the .zip holds no files");
    }
    // one folder at the top that is not one of the disc's own (em, etc, ...): that is the pack
    File source = staging;
    String name = safeName(zipName);
    File[] top = staging.listFiles();
    if (top != null && top.length == 1 && top[0].isDirectory() && !looksLikeDiscFolder(top[0].getName())) {
      source = top[0];
      name = safeName(top[0].getName());
    }
    File target = new File(mods, name);
    delete(target);
    File moved = source == staging ? staging : new File(mods, ".incoming-pack");
    if (source != staging) {
      delete(moved);
      if (!source.renameTo(moved)) {
        delete(staging);
        throw new IOException("cannot create " + target);
      }
      delete(staging);
    }
    if (!moved.renameTo(target)) {
      delete(moved);
      throw new IOException("cannot create " + target);
    }
    return new Result(name, files, ModSwitch.set(mods, name, true));
  }

  // the disc's own folders (files/...): a .zip with just em/ at the top is a pack itself
  private static boolean looksLikeDiscFolder(String n) {
    String l = n.toLowerCase(java.util.Locale.ROOT);
    return l.equals("em") || l.equals("files") || l.equals("etc") || l.equals("bio4") || l.matches("st\\d|r\\d{3}")
        || l.equals("melee") || l.equals("music") || l.equals("sound") || l.equals("event") || l.equals("ss")
        || l.equals("voice");
  }

  private static String displayName(ContentResolver resolver, Uri doc) {
    try (Cursor c = resolver.query(doc, new String[] {DocumentsContract.Document.COLUMN_DISPLAY_NAME}, null, null,
        null)) {
      if (c != null && c.moveToFirst()) return c.getString(0);
    }
    return null;
  }

  private static int copyChildren(ContentResolver resolver, Uri tree, String parentId, File dest, long[] bytes,
      int depth) throws IOException {
    if (depth > 16) return 0;
    Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId);
    int files = 0;
    try (Cursor c = resolver.query(children, new String[] {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
        DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE}, null, null, null)) {
      if (c == null) throw new IOException("the folder cannot be read");
      while (c.moveToNext()) {
        String id = c.getString(0), name = c.getString(1), mime = c.getString(2);
        if (name == null || name.startsWith(".")) continue;
        File out = new File(dest, safeName(name));
        if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
          if (!out.isDirectory() && !out.mkdirs()) throw new IOException("cannot create " + out);
          files += copyChildren(resolver, tree, id, out, bytes, depth + 1);
          continue;
        }
        try (InputStream in = resolver.openInputStream(DocumentsContract.buildDocumentUriUsingTree(tree, id));
             OutputStream o = new FileOutputStream(out)) {
          if (in == null) throw new IOException(name);
          byte[] buf = new byte[1 << 16];
          for (int n; (n = in.read(buf)) > 0;) {
            bytes[0] += n;
            if (bytes[0] > MAX_BYTES) throw new IOException("the mod is larger than 2 GB");
            o.write(buf, 0, n);
          }
        }
        files++;
      }
    }
    return files;
  }
}
