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
    for (File f : all) n += f.isDirectory() ? countFiles(f) : 1;
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

  static boolean delete(File f) {
    File[] all = f.listFiles();
    if (all != null) for (File c : all) delete(c);
    return !f.exists() || f.delete();
  }

  /** What a copy brought: the pack's folder name and its files. */
  static final class Result {
    final String name;
    final int files;
    Result(String name, int files) { this.name = name; this.files = files; }
  }

  /**
   * Copies the picked folder (a document tree) to game/mods/<its name>, replacing a pack of that name. It goes to a
   * hidden folder first, so a copy that fails leaves no half pack the core would read.
   */
  static Result copyTree(ContentResolver resolver, Uri tree, File gameRoot) throws IOException {
    String rootId = DocumentsContract.getTreeDocumentId(tree);
    String name = safeName(displayName(resolver, DocumentsContract.buildDocumentUriUsingTree(tree, rootId)));
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
    return new Result(name, files);
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
