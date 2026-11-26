package com.ast.re4decomp;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.graphics.drawable.GradientDrawable;
import android.opengl.GLSurfaceView;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.opengles.GL10;

/**
 * CubeEvil4 Studio (Mods, 19.11.2026; port/docs/studio-plan.md). DevJunior's choice: an app screen of its own, without
 * the game running underneath; the viewer and the animations first. Left: the tabs (Models, Animations, Convert, Edit)
 * and their lists; right: the 3D view (cpp/studio_gl.cpp) with the floor grid on X/Z and the axes (X red, Y green,
 * Z blue). One finger turns the camera, two fingers move it, a pinch zooms. The models come straight from the
 * imported discs (game/disc1, game/disc2: files/em/*.drs) and from the mod packs (game/mods).
 */
public final class StudioActivity extends Activity {
  private static final String TAG = "RE4Studio";
  private static final int BG = 0xFF101216, PANEL = 0xFF171A20, CARD = 0xFF20242C, ACCENT = 0xFFE0A23A,
      TEXT = 0xFFE8EAED, DIM = 0xFF9AA0A8;

  private final Handler main = new Handler(Looper.getMainLooper());
  private LinearLayout content;
  private final List<Button> tabs = new ArrayList<>();
  private int tab;
  private TextView title, info, frameLabel;
  private SeekBar seek;
  private Button play;
  private GLSurfaceView view;

  // the view's state (the GL thread reads it)
  private volatile float yaw = 0.6f, pitch = 0.25f, distance = 3500f, tx, ty = 900f, tz;
  private volatile int flags = 1 | 2 | 8;  // grid, axes, textures
  private volatile boolean playing = true;
  private volatile float speed = 1f, frame, length;
  private long lastNanos;
  private boolean seeking;

  private StudioFiles files;
  private String archivePath;      // the archive whose models are listed, below game/ (null: the archives)
  private String archiveName = "";
  private int highlighted = -1;    // the piece tinted in the view (its place in StudioBridge.parts())
  private String source = "disc1"; // disc1, disc2, mods
  private String kind = "all";     // all, pl, em, wep, other
  private boolean modelLoaded;

  @Override protected void onCreate(Bundle state) {
    super.onCreate(state);
    getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    StudioBridge.load();
    files = StudioFiles.open(this);
    LinearLayout root = new LinearLayout(this);
    root.setOrientation(LinearLayout.HORIZONTAL);
    root.setBackgroundColor(BG);

    // ---- the left panel: title, tabs, lists
    LinearLayout panel = new LinearLayout(this);
    panel.setOrientation(LinearLayout.VERTICAL);
    panel.setBackgroundColor(PANEL);
    panel.setPadding(dp(14), dp(12), dp(14), dp(8));
    root.addView(panel, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.36f));
    TextView name = text("CubeEvil4 Studio", 20, TEXT, true);
    panel.addView(name);
    TextView sub = text("Models and animations of your discs and mods", 12, DIM, false);
    panel.addView(sub);
    LinearLayout tabRow = new LinearLayout(this);
    tabRow.setOrientation(LinearLayout.HORIZONTAL);
    tabRow.setPadding(0, dp(10), 0, dp(8));
    String[] names = {"Models", "Animations", "Convert", "Edit"};
    for (int i = 0; i < names.length; i++) {
      final int index = i;
      Button b = pill(names[i], false);
      // one line each (DevJunior's screenshot 26.11.2026: "Model s", "Animat ions" broken in two)
      b.setSingleLine(true);
      b.setPadding(dp(4), 0, dp(4), 0);
      b.setTextSize(12);
      b.setOnClickListener(v -> showTab(index));
      tabs.add(b);
      LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(38), 1f);
      lp.setMargins(i == 0 ? 0 : dp(4), 0, 0, 0);
      tabRow.addView(b, lp);
    }
    panel.addView(tabRow);
    ScrollView scroll = new ScrollView(this);
    content = new LinearLayout(this);
    content.setOrientation(LinearLayout.VERTICAL);
    scroll.addView(content);
    panel.addView(scroll, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

    // ---- the right side: the 3D view with its bars
    FrameLayout stage = new FrameLayout(this);
    root.addView(stage, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.64f));
    view = new GLSurfaceView(this);
    view.setEGLContextClientVersion(3);
    view.setEGLConfigChooser(8, 8, 8, 8, 24, 0);
    view.setPreserveEGLContextOnPause(true);
    view.setRenderer(new GLSurfaceView.Renderer() {
      @Override public void onSurfaceCreated(GL10 gl, EGLConfig config) { StudioBridge.surfaceCreated(); }
      @Override public void onSurfaceChanged(GL10 gl, int w, int h) { width = w; height = h; }
      @Override public void onDrawFrame(GL10 gl) { drawFrame(); }
    });
    stage.addView(view, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.MATCH_PARENT));
    touch();

    LinearLayout top = new LinearLayout(this);
    top.setOrientation(LinearLayout.VERTICAL);
    top.setPadding(dp(14), dp(10), dp(14), dp(10));
    title = text("Choose a model on the left", 15, TEXT, true);
    info = text("", 12, DIM, false);
    top.addView(title);
    top.addView(info);
    stage.addView(top, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP));

    LinearLayout bar = new LinearLayout(this);
    bar.setOrientation(LinearLayout.VERTICAL);
    bar.setPadding(dp(12), dp(6), dp(12), dp(10));
    bar.setBackground(round(0xCC14161B, 0));
    LinearLayout timeline = new LinearLayout(this);
    timeline.setGravity(Gravity.CENTER_VERTICAL);
    play = pill("Pause", false);
    play.setOnClickListener(v -> { playing = !playing; play.setText(playing ? "Pause" : "Play"); });
    timeline.addView(play, new LinearLayout.LayoutParams(dp(78), dp(36)));
    seek = new SeekBar(this);
    seek.setMax(1000);
    seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
      @Override public void onProgressChanged(SeekBar s, int p, boolean user) {
        if (user && length > 0) frame = length * p / 1000f;
      }
      @Override public void onStartTrackingTouch(SeekBar s) { seeking = true; }
      @Override public void onStopTrackingTouch(SeekBar s) { seeking = false; }
    });
    timeline.addView(seek, new LinearLayout.LayoutParams(0, dp(36), 1f));
    frameLabel = text("0 / 0", 12, DIM, false);
    frameLabel.setMinWidth(dp(70));
    frameLabel.setGravity(Gravity.END);
    timeline.addView(frameLabel);
    bar.addView(timeline);
    HorizontalScrollView optionsScroll = new HorizontalScrollView(this);
    LinearLayout options = new LinearLayout(this);
    options.setPadding(0, dp(6), 0, 0);
    optionsScroll.addView(options);
    toggle(options, "Grid", 1);
    toggle(options, "Axes", 2);
    toggle(options, "Bones", 4);
    toggle(options, "Textures", 8);
    Button inPlace = pill("In place ✓", true);
    inPlace.setOnClickListener(v -> {
      boolean on = !inPlace.isSelected();
      inPlace.setSelected(on);
      style(inPlace, on);
      inPlace.setText(on ? "In place ✓" : "In place");
      StudioBridge.setInPlace(on);
    });
    options.addView(inPlace, chip());
    Button speedButton = pill("Speed 1×", false);
    speedButton.setOnClickListener(v -> {
      speed = speed >= 2f ? 0.25f : speed * 2f;
      speedButton.setText(String.format(Locale.ROOT, "Speed %s×", speed == 0.25f ? "¼" : speed == 0.5f ? "½"
          : String.valueOf((int) speed)));
    });
    options.addView(speedButton, chip());
    Button reset = pill("Frame model", false);
    reset.setOnClickListener(v -> frameModel());
    options.addView(reset, chip());
    bar.addView(optionsScroll);
    FrameLayout.LayoutParams barLp = new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM);
    stage.addView(bar, barLp);

    setContentView(root);
    showTab(0);
    main.postDelayed(ticker, 100);
  }

  private volatile int width = 1, height = 1;

  // ---- the GL thread: the clock of the animation and the picture
  private void drawFrame() {
    long now = System.nanoTime();
    float dt = lastNanos == 0 ? 0 : (now - lastNanos) / 1e9f;
    lastNanos = now;
    if (dt > 0.1f) dt = 0.1f;
    if (playing && !seeking && length > 0) {
      frame += dt * 30f * speed;  // the game's motions: 30 frames a second
      if (frame > length) frame = 0;
    }
    StudioBridge.draw(width, height, yaw, pitch, distance, tx, ty, tz, flags, frame);
  }

  private final Runnable ticker = new Runnable() {
    @Override public void run() {
      if (length > 0) {
        if (!seeking) seek.setProgress((int) (frame / length * 1000f));
        frameLabel.setText(String.format(Locale.ROOT, "%d / %d", (int) frame, (int) length));
      } else {
        frameLabel.setText("rest pose");
      }
      main.postDelayed(this, 100);
    }
  };

  // ---- touch: one finger turns, two fingers move, a pinch zooms
  private float lastX, lastY;
  private int lastCount;

  @SuppressLint("ClickableViewAccessibility")
  private void touch() {
    ScaleGestureDetector scale = new ScaleGestureDetector(this, new ScaleGestureDetector.SimpleOnScaleGestureListener() {
      @Override public boolean onScale(ScaleGestureDetector d) {
        float f = d.getScaleFactor();
        if (f > 0.01f) distance = Math.max(150f, Math.min(40000f, distance / f));
        return true;
      }
    });
    view.setOnTouchListener((v, e) -> {
      scale.onTouchEvent(e);
      int n = e.getPointerCount();
      float x = 0, y = 0;
      for (int i = 0; i < n; i++) { x += e.getX(i); y += e.getY(i); }
      x /= n;
      y /= n;
      if (e.getActionMasked() == MotionEvent.ACTION_DOWN || n != lastCount
          || e.getActionMasked() == MotionEvent.ACTION_POINTER_UP) {
        lastX = x;
        lastY = y;
        lastCount = n;
        return true;
      }
      if (e.getActionMasked() == MotionEvent.ACTION_MOVE) {
        float dx = x - lastX, dy = y - lastY;
        if (n == 1) {
          yaw -= dx * 0.008f;
          pitch = Math.max(-1.45f, Math.min(1.45f, pitch + dy * 0.006f));
        } else if (!scale.isInProgress() || n >= 2) {
          // move the target in the view's plane: right = (cos yaw, 0, -sin yaw), up = y
          float k = distance / Math.max(1, v.getHeight()) * 1.1f;
          tx -= dx * k * (float) Math.cos(yaw);
          tz += dx * k * (float) Math.sin(yaw);
          ty += dy * k;
        }
        lastX = x;
        lastY = y;
      }
      return true;
    });
  }

  private void frameModel() {
    float[] f = StudioBridge.frameModel();
    tx = f[0];
    ty = f[1];
    tz = f[2];
    distance = f[3];
    yaw = 0.6f;
    pitch = 0.2f;
  }

  // ---- the tabs
  private void showTab(int index) {
    tab = index;
    for (int i = 0; i < tabs.size(); i++) style(tabs.get(i), i == index);
    content.removeAllViews();
    if (files == null) {
      content.addView(note("CubeEvil4 Studio reads the discs and mod packs of CubeEvil4 Mods (or CubeEvil4). Install it, "
          + "import your discs there, then open the Studio again."));
      return;
    }
    if (index == 0) showModels();
    else if (index == 1) showAnimations();
    else if (index == 2) showConvert();
    else showEdit();
  }

  private void showModels() {
    LinearLayout sources = row();
    for (String s : new String[] {"disc1", "disc2", "mods"}) {
      Button b = pill(s.equals("disc1") ? "Disc 1" : s.equals("disc2") ? "Disc 2" : "Mod packs", s.equals(source));
      b.setOnClickListener(v -> { source = s; archivePath = null; showTab(0); });
      sources.addView(b, chip());
    }
    content.addView(sources);
    if (archivePath == null) {
      LinearLayout kinds = row();
      String[][] k = {{"all", "All"}, {"pl", "Characters"}, {"em", "Enemies"}, {"wep", "Weapons"}, {"other", "Other"}};
      for (String[] e : k) {
        Button b = pill(e[1], e[0].equals(kind));
        b.setOnClickListener(v -> { kind = e[0]; showTab(0); });
        kinds.addView(b, chip());
      }
      HorizontalScrollView hs = new HorizontalScrollView(this);
      hs.addView(kinds);
      content.addView(hs);
      List<String[]> found = archives();
      if (found.isEmpty()) {
        content.addView(note(!files.lastError.isEmpty()
            ? "The files of " + files.from() + " cannot be read: " + files.lastError
            : source.equals("mods") ? "No mod pack holds models (em/*.drs)."
            : "No archives here: import your discs in CubeEvil4 first."));
      }
      for (String[] f : found) {
        // f: rel, file name, pack name ("" none), size
        String parent = f[2].isEmpty() ? "" : f[2] + " · ";
        boolean model = f[0].contains("/em/");
        Button b = card(parent + f[1], String.format(Locale.ROOT, "%.1f MB", Long.parseLong(f[3]) / 1048576.0)
            + (model ? "" : " · tap to save a copy"));
        // a copy of the file for other tools (DevJunior, 20.11.2026: the core for the Remake HUD); hold a model's card
        b.setOnClickListener(v -> { if (model) openArchive(f[0], f[1]); else saveCopy(f[0], f[1]); });
        b.setOnLongClickListener(v -> { saveCopy(f[0], f[1]); return true; });
        content.addView(b, item());
      }
      return;
    }
    Button back = pill("‹ All archives", false);
    back.setOnClickListener(v -> { archivePath = null; showTab(0); });
    content.addView(back, item());
    Button whole = pill("Whole model again", false);
    whole.setOnClickListener(v -> loadComplete());
    content.addView(whole, item());
    // the pieces on the model: tap one to see it tinted, "Off" takes it off (DevJunior, 19.11.2026: "it is hard to
    // know what you are changing")
    TextView onHead = text("On the model", 14, TEXT, true);
    onHead.setPadding(0, dp(8), 0, dp(2));
    content.addView(onHead);
    content.addView(note("Tap a part to see it highlighted in the view."));
    String[] shownParts = StudioBridge.parts();
    java.util.Set<Integer> onModel = new java.util.HashSet<>();
    for (int k = 0; k < shownParts.length; k++) {
      String[] p = shownParts[k].split("\t", 2);
      final int piece = k;
      final int entry = Integer.parseInt(p[0]);
      onModel.add(entry);
      LinearLayout line = row();
      Button b = card(p.length > 1 ? p[1] : "#" + entry, k == 0 ? "main model · #" + entry : "#" + entry);
      if (highlighted == k) b.setBackground(round(0xFF3A2F1C, ACCENT));
      b.setOnClickListener(v -> {
        highlighted = highlighted == piece ? -1 : piece;
        StudioBridge.highlight(highlighted);
        showTab(0);
      });
      line.addView(b, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
      if (k > 0) {
        Button off = pill("Off", false);
        off.setOnClickListener(v -> {
          String r = StudioBridge.togglePiece(entry);
          if (!r.equals("removed")) toast("#" + entry + ": " + r);
          highlighted = -1;
          StudioBridge.highlight(-1);
          updateInfo();
          showTab(0);
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(64), dp(48));
        lp.setMargins(dp(6), 0, 0, 0);
        line.addView(off, lp);
      }
      content.addView(line, item());
    }
    // the archive's other models: on the model, or alone
    java.util.Map<Integer, String> names = new java.util.HashMap<>();
    for (String n : StudioBridge.partNames()) {
      String[] p = n.split("\t", 2);
      if (p.length == 2) names.put(Integer.parseInt(p[0]), p[1]);
    }
    TextView moreHead = text("More in this archive", 14, TEXT, true);
    moreHead.setPadding(0, dp(12), 0, dp(2));
    content.addView(moreHead);
    content.addView(note("Weapons, other hands, faces and outfits: “Add” puts one on the model (when it fits its "
        + "skeleton), “Alone” shows it by itself."));
    String[] entries = StudioBridge.entries();
    int shown = 0;
    for (String e : entries) {
      String[] p = e.split("\t");
      if (p.length < 3 || !p[1].startsWith("BIN") || p[2].equals("0")) continue;
      final int index = Integer.parseInt(p[0]);
      if (onModel.contains(index)) continue;
      LinearLayout line = row();
      String label = names.containsKey(index) ? names.get(index) : "Model #" + index;
      Button b = card(label, String.format(Locale.ROOT, "#%d · %d KB", index, Long.parseLong(p[2]) / 1024));
      b.setClickable(false);
      line.addView(b, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
      Button add = pill("Add", false);
      add.setOnClickListener(v -> {
        if (!modelLoaded) { toast("Open a model first"); return; }
        String r = StudioBridge.togglePiece(index);
        if (!r.equals("added")) toast(label + ": " + r);
        updateInfo();
        showTab(0);
      });
      Button alone = pill("Alone", false);
      alone.setOnClickListener(v -> loadModel(index, label));
      LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(58), dp(48));
      lp.setMargins(dp(6), 0, 0, 0);
      line.addView(add, lp);
      LinearLayout.LayoutParams lp2 = new LinearLayout.LayoutParams(dp(64), dp(48));
      lp2.setMargins(dp(4), 0, 0, 0);
      line.addView(alone, lp2);
      content.addView(line, item());
      shown++;
    }
    if (shown == 0) content.addView(note("Nothing else: every model of this archive is on the view."));
  }

  private void showAnimations() {
    if (!modelLoaded) {
      content.addView(note("Choose a model first (Models tab)."));
      return;
    }
    content.addView(note("Sources: the model's own archive is in use. Others (Leon, a Ganado, your mod packs) play "
        + "on this model when they fit its skeleton."));
    LinearLayout sources = new LinearLayout(this);
    sources.setOrientation(LinearLayout.VERTICAL);
    addSource(sources, "This model's archive", archivePath);
    addSource(sources, "Leon (pl00)", discFile("em/pl00.drs"));
    addSource(sources, "Ganado (em10)", discFile("em/em10.drs"));
    for (String[] f : packMotions()) addSource(sources, f[1], f[0]);
    content.addView(sources);
    TextView head = text("Animations", 14, TEXT, true);
    head.setPadding(0, dp(10), 0, dp(4));
    content.addView(head);
    String[] names = StudioBridge.motionNames();
    if (names.length == 0) content.addView(note("No animations in this source."));
    Button rest = card("Rest pose", "the model as it is built");
    rest.setOnClickListener(v -> pickMotion(-1, "Rest pose"));
    content.addView(rest, item());
    for (int i = 0; i < names.length; i++) {
      final int clip = i;
      final String label = names[i];
      Button b = card(label, "");
      b.setOnClickListener(v -> pickMotion(clip, label));
      content.addView(b, item());
    }
  }

  private void addSource(LinearLayout list, String label, String rel) {
    if (rel == null || files.size(rel) < 0) return;
    Button b = pill(label, false);
    b.setOnClickListener(v -> {
      String path = files.path(rel);
      if (path == null) { toast(label + ": cannot be read"); return; }
      int n = StudioBridge.useMotions(path, rel.substring(rel.lastIndexOf('/') + 1));
      if (n < 0) toast(label + ": " + StudioBridge.lastError());
      else toast(label + ": " + n + " animations");
      length = 0;
      frame = 0;
      showTab(1);
    });
    list.addView(b, item());
  }

  // ---- Convert (Mods, 19.11.2026; DevJunior's choice: UHD skins of the PC release first, picked as a .zip, saved as a
  // .zip the game imports like any mod pack). The mod is unpacked into the cache (em/plXX.udas, ImagePackHD/*.pack,
  // .lfs unpacked), each skin is turned into the disc's own archive (cpp/studio_convert.cpp, the port of
  // port/tools/uhd_skin_convert.py), shown here, and the results go into <mod>/em/plXX.drs (or .das) of the .zip.
  private static final int PICK_MOD = 41, SAVE_ZIP = 42, SAVE_COPY = 43;
  private String copyRel;  // the file "save a copy" writes (SAVE_COPY)

  // a disc or mod pack file as it is, to a place the user picks
  private void saveCopy(String rel, String name) {
    copyRel = rel;
    android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_CREATE_DOCUMENT);
    i.addCategory(android.content.Intent.CATEGORY_OPENABLE);
    i.setType("application/octet-stream");
    i.putExtra(android.content.Intent.EXTRA_TITLE, name);
    try {
      startActivityForResult(i, SAVE_COPY);
    } catch (Exception e) {
      toast("No file picker: " + e.getMessage());
    }
  }
  private static final String[][] CHARACTERS = {
      {"pl00", "Leon"}, {"pl08", "Leon (jacket)"}, {"pl09", "Leon (armour)"}, {"pl10", "Leon (outfit)"},
      {"pl01", "Ashley"}, {"pl05", "Ashley (armour)"}, {"pl11", "Ashley (outfit)"}, {"pl0b", "Ada"},
      {"pl0c", "Ada (2nd outfit)"}, {"pl06", "HUNK"}, {"pl0a", "Krauser"}, {"pl0d", "Wesker"}};
  // PC archives the discs do not have, and the disc's archives they go into (uhd_skin_convert.py: Separate Ways' Ada,
  // pl16, over Ada's pl0b; DevJunior's "Ada remake by kreed", 19.11.2026)
  private static final String[][] PC_ONLY = {{"pl16", "pl0b", "pl0c"}};
  // GTA San Andreas ped skins (.dff + .txd; DevJunior, 19.11.2026: "this model over Leon"): Leon's outfits
  private static final String[] GTA_ON = {"pl00", "pl08", "pl09", "pl10"};
  // ... or Ashley's or Ada's (DevJunior, 20.11.2026: "skins prepared for Ashley and for Ada Wong")
  private static final String[] ASHLEY_ON = {"pl01", "pl05", "pl11"};
  private static final String[] ADA_ON = {"pl0b", "pl0c"};
  private static final String[][] SKIN_FOR = {{"", "Auto"}, {"leon", "Leon"}, {"ashley", "Ashley"}, {"ada", "Ada"}};
  // the disc's handguns (DevJunior, 20.11.2026: "replace weapons or make weapon packs"): the choice for an RE4 Remake
  // weapon the converter does not know (the Sentinel Nine ...); the known ones go where StudioBridge.reWeaponTarget says
  private static final String[][] WEAPONS = {
      {"wep04", "Handgun"}, {"wep01", "Blacktail"}, {"wep02", "Punisher"}, {"wep03", "Red9"}, {"wep17", "Matilda"}};
  private java.io.File convertDir;          // cache/convert: udas/, packs/, out/
  private String modName = "";
  private final List<String[]> skins = new ArrayList<>();  // PC stem, udas path, disc rel ("" none), disc file, disc stem
  private final java.util.Map<String, String> results = new java.util.HashMap<>();  // stem -> converted file
  private final List<String[]> udasFiles = new ArrayList<>();  // the mod's skins: PC stem, unpacked .udas
  private final StringBuilder convertLog = new StringBuilder();
  // the largest texture side; 0 = Best: 512, else 256, else 128, the largest whose archive fits the mods' place
  // (DevJunior, 25.11.2026: "we use high-quality GTA skins and I want them to work optimally")
  private int convertTex = 0;
  private boolean keepHead;  // GTA skins: Leon's own head, face and hair stay (a skin without a head)
  // RE4 Remake outfits: "Slim", sized as the GTA skins come out (trunk to the head joint, a little slimmer); on at
  // first (DevJunior, 20.11.2026, the Master Chief so: "this one came out perfect, it would be a good measure")
  private boolean fitLeon = true;
  // whom GTA skins and RE4 Remake outfits go on: "" by the outfit's folder (cha1.. Ashley, cha2.. Ada, else Leon)
  private String skinFor = "";
  private final java.util.Map<String, String> remakeWho = new java.util.HashMap<>();  // mod folder -> its character
  private volatile boolean busy;

  private void showConvert() {
    if (busy) {  // the worker thread fills the lists; they are shown once it is done
      content.addView(card("Working…", "the mod's files are read and converted on the phone"), item());
      return;
    }
    content.addView(note("UHD skins of the PC release (Nexus mods: em/plXX.udas with BIO4/ImagePackHD/*.pack), GTA San "
        + "Andreas ped skins (.dff with its .txd), rigged models (binary .fbx or XNALara's generic_item.mesh, with their .png / .tga pictures), outfits for Leon, Ashley or Ada and handguns of the RE4 Remake (natives/STM/..., "
        + ".mesh with its .mdf2 and .tex) become a mod pack for CubeEvil4. Pick the mod's .zip (or its files)."));
    Button pick = card("Pick a mod (.zip)", modName.isEmpty() ? "" : "Now: " + modName);
    pick.setOnClickListener(v -> {
      android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
      i.addCategory(android.content.Intent.CATEGORY_OPENABLE);
      i.setType("*/*");
      i.putExtra(android.content.Intent.EXTRA_ALLOW_MULTIPLE, true);
      try {
        startActivityForResult(i, PICK_MOD);
      } catch (Exception e) {
        toast("No file picker: " + e.getMessage());
      }
    });
    content.addView(pick, item());
    if (!skins.isEmpty()) {
      LinearLayout sizes = row();
      sizes.addView(text("Textures ", 12, DIM, false));
      for (int t : new int[] {0, 128, 256, 512}) {
        Button b = pill(t == 0 ? "Best" : String.valueOf(t), t == convertTex);
        b.setOnClickListener(v -> { convertTex = t; results.clear(); showTab(2); });
        sizes.addView(b, chip());
      }
      content.addView(sizes);
      boolean gta = false;
      for (String[] k : skins) gta |= k[0].endsWith(".dff") || k[0].endsWith(".fbx") || k[0].endsWith(".mesh") || k[0].endsWith(REMAKE);
      if (gta) {
        LinearLayout who = row();
        who.addView(text("For ", 12, DIM, false));
        for (String[] f : SKIN_FOR) {
          Button b = pill(f[1], skinFor.equals(f[0]));
          b.setOnClickListener(v -> { skinFor = f[0]; results.clear(); listSkins(); showTab(2); });
          who.addView(b, chip());
        }
        content.addView(who);
        content.addView(note("Auto: RE4 Remake outfits go on whom they were made for (Leon, Ashley or Ada), GTA skins by "
            + "their name (Ashley, Ada, else Leon)."));
        LinearLayout head = row();
        head.addView(text("The character's head ", 12, DIM, false));
        for (boolean on : new boolean[] {false, true}) {
          Button b = pill(on ? "Shown" : "Hidden", keepHead == on);
          b.setOnClickListener(v -> { keepHead = on; results.clear(); showTab(2); });
          head.addView(b, chip());
        }
        content.addView(head);
        content.addView(note("Hidden for a skin with its own head or helmet; shown for one without a head."));
      }
      boolean remakeOutfit = false;
      for (String[] k : skins) remakeOutfit |= k[0].endsWith(REMAKE);
      if (remakeOutfit) {
        LinearLayout build = row();
        build.addView(text("Build ", 12, DIM, false));
        for (boolean on : new boolean[] {true, false}) {
          Button b = pill(on ? "Slim" : "Its own", fitLeon == on);
          b.setOnClickListener(v -> { fitLeon = on; results.clear(); showTab(2); });
          build.addView(b, chip());
        }
        content.addView(build);
        content.addView(note("RE4 Remake outfits. Slim: sized as the GTA skins come out (smaller trunk and head, a "
            + "little slimmer). Its own: the outfit's own build."));
      }
      content.addView(note(convertTex == 0
          ? "Best: the sharpest textures (512) that fit the game's place for the character, else 256, else 128."
          : "Larger textures look sharper but need more of the game's memory."));
      for (String[] k : skins) {
        String done = results.get(k[4]);
        String detail = k[2].isEmpty()
            ? (files.lastError.isEmpty() ? k[4] + " is not on your discs: import them in CubeEvil4 first"
                : "the discs cannot be read: " + files.lastError)
            : done != null ? "converted ✓ · tap to view" : "tap to convert into " + k[3];
        String head = (k[0].equals(k[4]) ? k[0] : k[0] + " → " + k[4]) + " · " + character(k[4]);
        Button b = card(head, detail);
        b.setEnabled(!busy && !k[2].isEmpty());
        b.setOnClickListener(v -> {
          if (results.containsKey(k[4]) && k[0].endsWith(REMAKE_WEAPON)) toast(k[3] + " is converted; the view shows characters only");
          else if (results.containsKey(k[4])) preview(k[4], k[3]);
          else {
            // Ashley (DevJunior, 25.11.2026: "Ashley stops using the skin in the campaign"): the player's Ashley (pl01,
            // pl05) and the partner at Leon's side in the story (pl11) are three archives; one tap converts the model
            // into each of them that is listed
            List<String[]> rows = new ArrayList<>();
            boolean ashley = k[4].equals("pl01") || k[4].equals("pl05") || k[4].equals("pl11");
            for (String[] o : skins)
              if (o == k || (ashley && o[0].equals(k[0]) && o[1].equals(k[1]) && !o[2].isEmpty()
                  && (o[4].equals("pl01") || o[4].equals("pl05") || o[4].equals("pl11")) && !results.containsKey(o[4])))
                if (!rows.contains(o)) rows.add(o);
            if (!rows.contains(k)) rows.add(0, k);
            convert(rows, k[4]);
          }
        });
        content.addView(b, item());
      }
      Button all = pill("Convert all", false);
      all.setEnabled(!busy);
      all.setOnClickListener(v -> convert(new ArrayList<>(skins), null));
      Button save = pill("Save as .zip", !results.isEmpty());
      save.setEnabled(!busy && !results.isEmpty());
      save.setOnClickListener(v -> {
        android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_CREATE_DOCUMENT);
        i.addCategory(android.content.Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        i.putExtra(android.content.Intent.EXTRA_TITLE, modName + ".zip");
        try {
          startActivityForResult(i, SAVE_ZIP);
        } catch (Exception e) {
          toast("No file picker: " + e.getMessage());
        }
      });
      LinearLayout actions = row();
      actions.addView(all, chip());
      actions.addView(save, chip());
      content.addView(actions);
      if (!results.isEmpty()) {
        content.addView(note("In CubeEvil4: menu (SELECT) › Mods › import the .zip, then switch the pack on."));
      }
    }
    if (convertLog.length() > 0) {
      TextView log = text(convertLog.toString(), 11, DIM, false);
      log.setTypeface(android.graphics.Typeface.MONOSPACE);
      log.setTextIsSelectable(true);
      content.addView(log);
    }
  }

  private static String character(String stem) {
    for (String[] c : CHARACTERS) if (c[0].equals(stem)) return c[1];
    for (String[] w : WEAPONS) if (w[0].equals(stem)) return w[1];
    return stem.startsWith("em") ? "enemy" : "character";
  }

  private void log(String s) {
    convertLog.append(s).append('\n');
    if (convertLog.length() > 12000) convertLog.delete(0, convertLog.length() - 12000);
  }

  @Override protected void onActivityResult(int request, int result, android.content.Intent data) {
    super.onActivityResult(request, result, data);
    if (result != RESULT_OK || data == null) return;
    if (request == PICK_MOD) {
      List<android.net.Uri> picked = new ArrayList<>();
      if (data.getClipData() != null) {
        for (int i = 0; i < data.getClipData().getItemCount(); i++) picked.add(data.getClipData().getItemAt(i).getUri());
      } else if (data.getData() != null) {
        picked.add(data.getData());
      }
      importMod(picked);
    } else if (request == SAVE_ZIP && data.getData() != null) {
      saveZip(data.getData());
    } else if (request == PICK_ANIM && data.getData() != null) {
      android.net.Uri u = data.getData();
      java.io.File tmp = new java.io.File(getCacheDir(), "edit_source.dat");
      try (java.io.InputStream in = getContentResolver().openInputStream(u);
           java.io.OutputStream out = new java.io.FileOutputStream(tmp)) {
        byte[] buf = new byte[1 << 16];
        for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
      } catch (Exception e) {
        toast("Cannot be read: " + e.getMessage());
        return;
      }
      openEdit(tmp.getPath(), displayName(u));
    } else if (request == SAVE_ANIM && data.getData() != null) {
      java.io.File tmp = new java.io.File(getCacheDir(), "edit_save.dat");
      String why = StudioBridge.editSave(tmp.getPath());
      if (why.isEmpty()) {
        try (java.io.InputStream in = new java.io.FileInputStream(tmp);
             java.io.OutputStream out = getContentResolver().openOutputStream(data.getData())) {
          byte[] buf = new byte[1 << 16];
          for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
        } catch (Exception e) {
          why = String.valueOf(e.getMessage());
        }
      }
      tmp.delete();
      toast(why.isEmpty() ? "Saved: " + editName : "Not saved: " + why);
    } else if (request == SAVE_COPY && data.getData() != null && copyRel != null) {
      final android.net.Uri to = data.getData();
      final String rel = copyRel;
      new Thread(() -> {
        java.io.File tmp = new java.io.File(getCacheDir(), "copy.tmp");
        String why = files.copy(rel, tmp);
        if (why.isEmpty()) {
          try (java.io.InputStream in = new java.io.FileInputStream(tmp);
               java.io.OutputStream out = getContentResolver().openOutputStream(to)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
          } catch (Exception e) {
            why = String.valueOf(e.getMessage());
          }
        }
        tmp.delete();
        final String w = why;
        main.post(() -> toast(w.isEmpty() ? "Saved: " + rel : "Not saved: " + w));
      }, "studio-copy").start();
    }
  }

  private void work(String what, Runnable job) {
    busy = true;
    showTab(2);
    toast(what);
    new Thread(() -> {
      try {
        job.run();
      } catch (Throwable t) {
        Log.e(TAG, what, t);
        log("error: " + t);
      }
      main.post(() -> { busy = false; showTab(2); });
    }, "studio-convert").start();
  }

  // the picked files (a .zip, or .udas / .pack files, packed with .lfs or not) into cache/convert
  private void importMod(List<android.net.Uri> picked) {
    work("Reading the mod…", () -> {
      convertDir = new java.io.File(getCacheDir(), "convert");
      deleteTree(convertDir);
      java.io.File udas = new java.io.File(convertDir, "udas"), packs = new java.io.File(convertDir, "packs");
      new java.io.File(convertDir, "gta").mkdirs();
      new java.io.File(convertDir, "fbx").mkdirs();
      java.io.File remake = new java.io.File(convertDir, "remake");
      remake.mkdirs();
      udas.mkdirs();
      packs.mkdirs();
      new java.io.File(convertDir, "out").mkdirs();
      skins.clear();
      udasFiles.clear();
      remakeWho.clear();
      results.clear();
      convertLog.setLength(0);
      modName = "";
      for (android.net.Uri u : picked) {
        String name = displayName(u);
        String low = name.toLowerCase(Locale.ROOT);
        if (modName.isEmpty()) modName = cleanName(name);
        if (low.endsWith(".7z") || low.endsWith(".rar")) {
          log(name + ": .7z and .rar cannot be read here yet; unpack it and pick the .udas and .pack files, or "
              + "repack it as .zip");
          continue;
        }
        try (java.io.InputStream in = getContentResolver().openInputStream(u)) {
          if (in == null) continue;
          if (low.endsWith(".zip")) {
            java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(new java.io.BufferedInputStream(in));
            int found = 0;
            for (java.util.zip.ZipEntry e; (e = zip.getNextEntry()) != null;) {
              if (e.isDirectory()) continue;
              String n = e.getName().replace('\\', '/');
              // an RE4 Remake mod: its folders (the base, the colour variants) with natives/STM/... kept as they are
              int nat = n.toLowerCase(Locale.ROOT).indexOf("natives/stm/");
              if (nat >= 0) {
                String low2 = n.toLowerCase(Locale.ROOT);
                if (!low2.contains(".mesh.") && !low2.contains(".mdf2.") && !low2.contains(".tex.")) continue;
                String top = nat == 0 ? "mod" : n.substring(0, nat - 1).replace('/', '_');
                if (top.isEmpty() || top.contains("..")) continue;
                java.io.File dest = new java.io.File(new java.io.File(remake, top), n.substring(nat));
                if (!dest.getCanonicalPath().startsWith(remake.getCanonicalPath())) continue;
                dest.getParentFile().mkdirs();
                store(zip, dest);
                found++;
                continue;
              }
              // a rigged model (.fbx, XNALara's .mesh) and its pictures: each zip folder in a folder of its own (two
              // XNALara models are both generic_item.mesh; DevJunior, 26.11.2026)
              String lowN = n.toLowerCase(Locale.ROOT);
              if (lowN.endsWith(".fbx") || lowN.endsWith(".mesh") || lowN.endsWith(".png") || lowN.endsWith(".tga")
                  || lowN.endsWith(".jpg") || lowN.endsWith(".jpeg")) {
                int cut = n.lastIndexOf('/');
                String dir = cut <= 0 ? "model" : n.substring(0, cut).replace('/', '_');
                if (dir.contains("..")) continue;
                java.io.File rig = new java.io.File(new java.io.File(new java.io.File(convertDir, "fbx"), dir),
                    lowN.substring(lowN.lastIndexOf('/') + 1));
                rig.getParentFile().mkdirs();
                store(zip, rig);
                found++;
                continue;
              }
              java.io.File dest = destination(n.substring(n.lastIndexOf('/') + 1), udas, packs);
              if (dest == null) continue;
              store(zip, dest);
              found++;
            }
            log(name + ": " + found + " files to convert");
          } else {
            java.io.File dest = destination(name, udas, packs);
            if (dest == null) log(name + ": not a .udas or .pack file, left out");
            else store(in, dest);
          }
        } catch (Exception e) {
          log(name + ": " + e.getMessage());
        }
      }
      if (modName.isEmpty()) modName = "UHD skin";
      // .lfs (the PC release's packing) unpacked, then the skins listed against the discs
      java.io.File[] all = concat(udas.listFiles(), packs.listFiles());
      for (java.io.File f : all) {
        if (!f.getName().toLowerCase(Locale.ROOT).endsWith(".lfs")) continue;
        java.io.File out = new java.io.File(f.getParentFile(), f.getName().substring(0, f.getName().length() - 4));
        String why = StudioBridge.unpackLfs(f.getAbsolutePath(), out.getAbsolutePath());
        if (!why.isEmpty()) log(f.getName() + ": " + why);
        f.delete();
      }
      udasFiles.clear();
      remakeWho.clear();
      java.io.File[] found = udas.listFiles();
      if (found != null) {
        java.util.Arrays.sort(found);
        for (java.io.File f : found) {
          udasFiles.add(new String[] {f.getName().toLowerCase(Locale.ROOT).replace(".udas", ""), f.getAbsolutePath()});
        }
      }
      java.io.File[] gta = new java.io.File(convertDir, "gta").listFiles();
      if (gta != null) {
        java.util.Arrays.sort(gta);
        for (java.io.File f : gta) if (f.getName().endsWith(".dff")) udasFiles.add(new String[] {f.getName(), f.getAbsolutePath()});
      }
      // rigged models: .fbx, and XNALara's .mesh (listed by its folder's name: they are all generic_item.mesh)
      java.util.ArrayDeque<java.io.File> rigDirs = new java.util.ArrayDeque<>();
      rigDirs.add(new java.io.File(convertDir, "fbx"));
      while (!rigDirs.isEmpty()) {
        java.io.File[] in = rigDirs.poll().listFiles();
        if (in == null) continue;
        java.util.Arrays.sort(in);
        for (java.io.File f : in) {
          if (f.isDirectory()) rigDirs.add(f);
          else if (f.getName().endsWith(".fbx")) udasFiles.add(new String[] {f.getName(), f.getAbsolutePath()});
          else if (f.getName().endsWith(".mesh")) {
            String folder = f.getParentFile().getName();
            udasFiles.add(new String[] {(folder.equals("fbx") ? f.getName().replace(".mesh", "") : folder) + ".mesh",
                f.getAbsolutePath()});
          }
        }
      }
      // RE4 Remake mods: the folder with the character meshes, and each colour variant laid over it; weapons (meshes
      // under Character/wp/wpNN/wpNNNN) each on its own, a folder with only textures of a weapon as its variant
      java.io.File[] tops = remake.listFiles();
      if (tops != null) {
        java.util.Arrays.sort(tops);
        java.io.File base = null;
        java.util.Map<String, java.io.File> weaponBase = new java.util.TreeMap<>();
        java.util.Map<java.io.File, java.util.Set<String>> weaponTex = new java.util.LinkedHashMap<>();
        for (java.io.File t : tops) {
          if (!t.isDirectory()) continue;
          java.util.Set<String> meshes = new java.util.TreeSet<>(), any = new java.util.TreeSet<>();
          weaponIds(t, meshes, any);
          if (!meshes.isEmpty()) {
            for (String wp : meshes) {
              udasFiles.add(new String[] {t.getName() + " · " + wp + REMAKE_WEAPON, t.getAbsolutePath(), wp});
              if (!weaponBase.containsKey(wp)) weaponBase.put(wp, t);
            }
          } else if (!any.isEmpty()) {
            weaponTex.put(t, any);
          } else if (base == null && hasMesh(t)) {
            base = t;
          }
        }
        for (java.util.Map.Entry<java.io.File, java.util.Set<String>> v : weaponTex.entrySet()) {
          for (String wp : v.getValue()) {
            java.io.File b = weaponBase.get(wp);
            if (b == null) continue;
            udasFiles.add(new String[] {v.getKey().getName() + " · " + wp + REMAKE_WEAPON,
                b.getAbsolutePath() + "\n" + v.getKey().getAbsolutePath(), wp});
          }
        }
        if (base != null) {
          udasFiles.add(new String[] {base.getName() + REMAKE, base.getAbsolutePath()});
          for (java.io.File t : tops) {
            if (t.equals(base) || !t.isDirectory() || weaponTex.containsKey(t) || weaponBase.containsValue(t)) continue;
            udasFiles.add(new String[] {t.getName() + REMAKE, base.getAbsolutePath() + "\n" + t.getAbsolutePath()});
          }
        } else if (tops.length > 0 && weaponBase.isEmpty()) {
          log("RE4 Remake files without a .mesh: only outfits and weapons with their meshes are converted here");
        }
      }
      listSkins();
      String[] packNames = packs.list();
      log(skins.size() + " skins, " + (packNames == null ? 0 : packNames.length) + " texture packs");
      if (skins.isEmpty()) {
        log("No em/plXX.udas, .dff, .fbx or RE4 Remake .mesh in what was picked: this converts UHD skins of the PC release, "
            + "GTA San Andreas ped skins, rigged .fbx models and outfits of the RE4 Remake.");
      }
    });
  }

  // each PC skin against the disc archives it goes into (several for PC_ONLY); again when the discs could not be read
  private void listSkins() {
    files.lastError = "";
    List<String[]> out = new ArrayList<>();
    for (String[] u : udasFiles) {
      List<String> targets = new ArrayList<>();
      if (u[0].endsWith(".dff") || u[0].endsWith(".fbx") || u[0].endsWith(".mesh") || u[0].endsWith(REMAKE)) {
        String who = !skinFor.isEmpty() ? skinFor
            : u[0].endsWith(REMAKE) ? whoFor(u[1].split("\n")[0]) : whoByName(u[0] + " " + u[1]);
        targets.addAll(java.util.Arrays.asList(
            who.equals("ashley") ? ASHLEY_ON : who.equals("ada") ? ADA_ON : GTA_ON));
      }
      else if (u[0].endsWith(REMAKE_WEAPON)) {
        String known = StudioBridge.reWeaponTarget(u[2]);
        if (!known.isEmpty()) targets.add(known);
        else for (String[] w : WEAPONS) targets.add(w[0]);
      } else targets.add(u[0]);
      for (String[] alt : PC_ONLY) {
        if (alt[0].equals(u[0])) {
          targets.clear();
          for (int t = 1; t < alt.length; t++) targets.add(alt[t]);
        }
      }
      for (String target : targets) {
        String rel = "", file = "";
        for (String disc : new String[] {"disc1", "disc2"}) {
          for (String ext : new String[] {".drs", ".das"}) {
            String r = disc + "/files/em/" + target + ext;
            if (rel.isEmpty() && files.size(r) >= 0) {
              rel = r;
              file = target + ext;
            }
          }
        }
        out.add(new String[] {u[0], u[1], rel, file, target, u.length > 2 ? u[2] : ""});
      }
    }
    skins.clear();
    skins.addAll(out);
  }

  // whom an RE4 Remake outfit was made for: its meshes' folder (character/ch/cha1NN Ashley, cha2NN Ada, else Leon)
  // Auto for a GTA skin (DevJunior, 25.11.2026: an Ashley skin went on Leon): by its file and folder names, "ashley"
  // -> Ashley, "ada" as a word -> Ada, else Leon
  static String whoByName(String names) {
    String n = names.toLowerCase(Locale.ROOT);
    if (n.contains("ashley")) return "ashley";
    if (n.matches("(?s).*(^|[^a-z])ada([^a-z]|$).*") || n.contains("adawong")) return "ada";
    return "";
  }

  private String whoFor(String base) {
    String known = remakeWho.get(base);
    if (known != null) return known;
    String who = "";
    java.util.ArrayDeque<java.io.File> todo = new java.util.ArrayDeque<>();
    todo.add(new java.io.File(base));
    while (!todo.isEmpty() && who.isEmpty()) {
      java.io.File d = todo.poll();
      java.io.File[] in = d.listFiles();
      if (in == null) continue;
      for (java.io.File f : in) {
        if (f.isDirectory()) todo.add(f);
        else if (f.getName().toLowerCase(Locale.ROOT).contains(".mesh")) {
          String path = f.getAbsolutePath().toLowerCase(Locale.ROOT).replace('\\', '/');
          if (path.contains("/ch/cha1/")) who = "ashley";
          else if (path.contains("/ch/cha2/")) who = "ada";
          else if (path.contains("/ch/cha0/")) who = "leon";
        }
      }
    }
    remakeWho.put(base, who);
    return who;
  }

  private static java.io.File destination(String name, java.io.File udas, java.io.File packs) {
    String low = name.toLowerCase(Locale.ROOT);
    if (low.matches("(pl|em)[0-9a-f]{2}\\.udas(\\.lfs)?")) return new java.io.File(udas, low);
    if (low.endsWith(".dff") || low.endsWith(".txd")) return new java.io.File(new java.io.File(udas.getParentFile(), "gta"), low);
    // a rigged model as .fbx with its pictures beside it (DevJunior, 25.11.2026)
    if (low.endsWith(".fbx") || low.endsWith(".mesh") || low.endsWith(".png") || low.endsWith(".tga") || low.endsWith(".jpg")
        || low.endsWith(".jpeg"))
      return new java.io.File(new java.io.File(udas.getParentFile(), "fbx"), low);
    if (low.endsWith(".pack") || low.endsWith(".pack.lfs")) return new java.io.File(packs, low);
    return null;
  }

  // RE4 Remake mods (Leon outfits of the PC release, RE Engine): an entry's name ends with this
  private static final String REMAKE = " (RE4 Remake)";
  // RE4 Remake weapon mods (natives/STM/_Chainsaw/Character/wp/wpNN/wpNNNN): an entry's name ends with this
  private static final String REMAKE_WEAPON = " (RE4 Remake weapon)";

  // the weapons (wpNNNN) a folder of an RE4 Remake mod holds files of: with a .mesh in `meshes`, any file in `any`
  private static void weaponIds(java.io.File dir, java.util.Set<String> meshes, java.util.Set<String> any) {
    java.io.File[] in = dir.listFiles();
    if (in == null) return;
    for (java.io.File f : in) {
      if (f.isDirectory()) {
        weaponIds(f, meshes, any);
        continue;
      }
      String p = f.getAbsolutePath().replace('\\', '/').toLowerCase(Locale.ROOT);
      java.util.regex.Matcher m = java.util.regex.Pattern.compile("/character/wp/wp[0-9]{2}/(wp[0-9]{4})/").matcher(p);
      if (!m.find()) continue;
      any.add(m.group(1));
      if (f.getName().toLowerCase(Locale.ROOT).contains(".mesh.")) meshes.add(m.group(1));
    }
  }

  private static boolean hasMesh(java.io.File dir) {
    java.io.File[] in = dir.listFiles();
    if (in == null) return false;
    for (java.io.File f : in) {
      if (f.isDirectory() ? hasMesh(f) : f.getName().toLowerCase(Locale.ROOT).contains(".mesh.")) return true;
    }
    return false;
  }

  // the .txd of a .dff: the one of the same name, else the only one picked ("" none)
  private static String txdFor(String dffPath) {
    java.io.File dff = new java.io.File(dffPath);
    java.io.File same = new java.io.File(dff.getParentFile(), dff.getName().replaceAll("\\.dff$", ".txd"));
    if (same.isFile()) return same.getAbsolutePath();
    String[] all = dff.getParentFile().list();
    String only = "";
    int n = 0;
    if (all != null) for (String a : all) if (a.endsWith(".txd")) { only = a; n++; }
    return n == 1 ? new java.io.File(dff.getParentFile(), only).getAbsolutePath() : "";
  }

  private static void store(java.io.InputStream in, java.io.File dest) throws java.io.IOException {
    try (java.io.OutputStream out = new java.io.FileOutputStream(dest)) {
      byte[] buf = new byte[1 << 16];
      for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
    }
  }

  private void convert(List<String[]> which, String preview) {
    work("Converting…", () -> {
      java.io.File packDir = new java.io.File(convertDir, "packs"), out = new java.io.File(convertDir, "out");
      String[] packNames = packDir.list();
      List<String> packs = new ArrayList<>();
      if (packNames != null) for (String n : packNames) if (n.endsWith(".pack")) packs.add(new java.io.File(packDir, n).getAbsolutePath());
      for (String[] k : which) {
        if (k[2].isEmpty()) continue;
        java.io.File disc = new java.io.File(convertDir, "disc_" + k[3]);
        String why = files.copy(k[2], disc);
        if (!why.isEmpty()) {
          log(k[0] + " → " + k[3] + ": the disc's file cannot be read: " + why);
          continue;
        }
        java.io.File dest = new java.io.File(out, k[3]);
        String r = "";
        for (int tex : convertTex == 0 ? new int[] {512, 256, 128} : new int[] {convertTex}) {
          r = convertOne(k, disc, dest, packs, tex);
          if (r.startsWith("ok\n") || !r.contains("too large")) {
            if (convertTex == 0) r = r.replaceFirst("\n", "\ntextures up to " + tex + " (Best)\n");
            break;
          }
        }
        disc.delete();
        boolean ok = r.startsWith("ok\n");
        log("── " + k[0] + " → " + k[3] + " · " + character(k[4]) + (ok ? ": converted" : ": failed"));
        log(r.substring(r.indexOf('\n') + 1).trim());
        if (ok) results.put(k[4], dest.getAbsolutePath());
      }
      if (preview != null && results.containsKey(preview)) {
        String file = null;
        for (String[] k : which) if (k[4].equals(preview)) file = k[3];
        final String f = file;
        main.post(() -> preview(preview, f));
      }
    });
  }

  // one conversion with textures up to `convertTex` (a side)
  private String convertOne(String[] k, java.io.File disc, java.io.File dest, List<String> packs, int convertTex) {
    return k[0].endsWith(REMAKE_WEAPON)
        ? StudioBridge.convertReWeapon(disc.getAbsolutePath(), k[3], k[1].split("\n"), k[5], convertTex,
            dest.getAbsolutePath())
        : k[0].endsWith(REMAKE)
        ? StudioBridge.convertReMod(disc.getAbsolutePath(), k[3], k[1].split("\n"), convertTex, keepHead, fitLeon,
            dest.getAbsolutePath())
        : k[0].endsWith(".mesh")
        ? StudioBridge.convertXps(disc.getAbsolutePath(), k[3], k[1], convertTex, keepHead, dest.getAbsolutePath())
        : k[0].endsWith(".fbx")
        ? StudioBridge.convertFbx(disc.getAbsolutePath(), k[3], k[1], convertTex, keepHead, dest.getAbsolutePath())
        : k[0].endsWith(".dff")
        ? StudioBridge.convertDff(disc.getAbsolutePath(), k[3], k[1], txdFor(k[1]), convertTex, keepHead, fitLeon,
            dest.getAbsolutePath())
        : StudioBridge.convertSkin(disc.getAbsolutePath(), k[3], k[1], packs.toArray(new String[0]), convertTex,
            dest.getAbsolutePath());
  }

  // the converted archive on the view, as the game will put it together
  private void preview(String stem, String file) {
    String path = results.get(stem);
    if (path == null) return;
    String why = StudioBridge.openArchive(path, file);
    if (!why.isEmpty()) {
      toast(file + ": " + why);
      return;
    }
    archivePath = null;
    archiveName = modName + " · " + file;
    String w = StudioBridge.loadComplete();
    if (!w.isEmpty()) {
      toast(file + ": " + w);
      return;
    }
    modelLoaded = true;
    length = 0;
    frame = 0;
    title.setText(archiveName);
    updateInfo();
    frameModel();
  }

  // the pack the game imports: <mod>/em/plXX.drs (Mods › import)
  private void saveZip(android.net.Uri uri) {
    work("Saving…", () -> {
      String folder = modName.replaceAll("[^A-Za-z0-9 _.-]", "").trim();
      if (folder.isEmpty()) folder = "UHD skin";
      try (java.io.OutputStream os = getContentResolver().openOutputStream(uri, "w");
           java.util.zip.ZipOutputStream zip = new java.util.zip.ZipOutputStream(new java.io.BufferedOutputStream(os))) {
        for (java.util.Map.Entry<String, String> r : results.entrySet()) {
          java.io.File f = new java.io.File(r.getValue());
          zip.putNextEntry(new java.util.zip.ZipEntry(folder + "/em/" + f.getName()));
          try (java.io.InputStream in = new java.io.FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0;) zip.write(buf, 0, n);
          }
          zip.closeEntry();
        }
        log("saved: " + folder + ".zip (" + results.size() + " archives)");
      } catch (Exception e) {
        log("cannot be saved: " + e.getMessage());
      }
    });
  }

  private String displayName(android.net.Uri u) {
    try (android.database.Cursor c = getContentResolver().query(u,
             new String[] {android.provider.OpenableColumns.DISPLAY_NAME}, null, null, null)) {
      if (c != null && c.moveToFirst() && c.getString(0) != null) return c.getString(0);
    } catch (Exception ignored) {
    }
    String p = u.getLastPathSegment();
    return p == null ? "mod" : p.substring(p.lastIndexOf('/') + 1);
  }

  // "Jill as Ashley-1234-1-0-1700000000.zip" -> "Jill as Ashley" (the Nexus download's numbers dropped)
  static String cleanName(String file) {
    String n = file.replaceAll("(?i)\\.(zip|7z|rar|udas|lfs|pack)$", "");
    n = n.replaceAll("(?i)\\.(udas|pack)$", "");
    n = n.replaceAll("(-\\d+)+$", "").replace('_', ' ').replaceAll("[^A-Za-z0-9 .-]", "").trim();
    return n.length() > 40 ? n.substring(0, 40).trim() : n;
  }

  private static java.io.File[] concat(java.io.File[] a, java.io.File[] b) {
    List<java.io.File> out = new ArrayList<>();
    if (a != null) out.addAll(java.util.Arrays.asList(a));
    if (b != null) out.addAll(java.util.Arrays.asList(b));
    return out.toArray(new java.io.File[0]);
  }

  private static void deleteTree(java.io.File f) {
    java.io.File[] children = f.listFiles();
    if (children != null) for (java.io.File c : children) deleteTree(c);
    f.delete();
  }

  // ---- Edit: the animations (Mods, 26.11.2026, DevJunior: "I want to edit the animations I have and take them to the
  // game myself"; "choose and adjust", good controls). A motion archive (a mod pack's melee/move.dat, finish.dat, or a
  // .dat from the phone) is opened in cpp/studio_motion_edit.cpp; one animation of it plays on the model; a bone is
  // turned on X, Y, Z (or the body moved) over a range of frames with a soft edge; the speed changes; undo; the result
  // is saved as a .dat for the mod pack's melee folder.
  private static final int PICK_ANIM = 44, SAVE_ANIM = 45;
  // Leon's bones by their part number (the game's skeleton of pl00 .. pl10)
  private static final String[] LEON_BONES = {
      "Root", "Waist", "Spine", "Chest", "Head", "R clavicle", "", "R upper arm", "R forearm", "R twist", "R hand",
      "L clavicle", "", "L upper arm", "L forearm", "L twist", "L hand", "Hips", "R thigh", "R calf", "R foot",
      "R toe", "L thigh", "L calf", "L foot", "L toe", "", "Jaw"};
  private String editName = "";      // the open archive's file name ("" none)
  private String[] editClips = {};
  private int editClip = -1;          // index into editClips
  private int editPart = 3;           // the bone (part number); -1 the body's position (root, mm); -10 - k IK target k
  private static final String[] IK_ENDS = {"IK: right hand", "IK: left hand", "IK: right foot", "IK: left foot"};
  private final float[] editDelta = new float[3];  // degrees, or mm for the body
  private int editFrom = 0, editTo = -1, editFade = 4, editSpeedPct = 100;

  private static String boneName(int part) {
    if (part <= -10) return IK_ENDS[-10 - part];
    if (part < 0) return "Body position";
    String n = part < LEON_BONES.length ? LEON_BONES[part] : "";
    return n.isEmpty() ? "Bone " + part : n + " (" + part + ")";
  }

  private int clipEntry() { return editClip < 0 ? -1 : Integer.parseInt(editClips[editClip].split("\\|", 3)[0]); }
  private int clipLast() { return editClip < 0 ? 0 : Integer.parseInt(editClips[editClip].split("\\|", 3)[1]); }
  private String clipLabel(int i) {
    String[] c = editClips[i].split("\\|", 3);
    return c[2] + " · " + c[1] + " frames";
  }

  private void showEdit() {
    content.addView(note("Animations: open a motion archive (your mod pack's melee/move.dat, or a .dat on the phone), "
        + "pick an animation and a bone, turn it on X, Y, Z over a range of frames, then save it as a .dat and put it "
        + "in your mod pack's melee folder. IK: pick \"IK: right hand\" (or a foot) as the bone and move it in mm. "
        + "Model colours and textures come next."));
    if (!modelLoaded) {
      String leon = discFile("em/pl00.drs");
      Button load = card("Load Leon (pl00)", leon == null ? "import your discs in CubeEvil4 first" : "the model the "
          + "changes play on");
      load.setEnabled(leon != null);
      load.setOnClickListener(v -> { openArchive(leon, "pl00.drs"); if (modelLoaded) showTab(3); });
      content.addView(load, item());
    }
    TextView head = text("Open", 14, TEXT, true);
    content.addView(head);
    for (String[] f : packMotions()) {
      Button b = pill(f[1], f[1].equals(editName));
      b.setOnClickListener(v -> openEdit(files.path(f[0]), f[1]));
      content.addView(b, item());
    }
    Button pick = pill("A .dat on the phone…", false);
    pick.setOnClickListener(v -> {
      android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_OPEN_DOCUMENT);
      i.addCategory(android.content.Intent.CATEGORY_OPENABLE);
      i.setType("*/*");
      try { startActivityForResult(i, PICK_ANIM); } catch (Exception e) { toast("No file picker: " + e.getMessage()); }
    });
    content.addView(pick, item());
    if (editName.isEmpty()) return;

    TextView open = text(editName, 14, ACCENT, true);
    open.setPadding(0, dp(10), 0, dp(2));
    content.addView(open);
    Button clip = card("Animation", editClip < 0 ? "tap to choose" : clipLabel(editClip));
    clip.setOnClickListener(v -> chooseClip());
    content.addView(clip, item());
    if (editClip < 0) return;
    final int last = clipLast();
    if (editTo < 0 || editTo > last) editTo = last;

    Button bone = card("Bone", boneName(editPart));
    bone.setOnClickListener(v -> chooseBone());
    content.addView(bone, item());

    boolean body = editPart < 0, ik = editPart <= -10;
    if (ik) content.addView(note("Move the hand or foot; the arm or leg bends to reach it, the hand or foot keeps its "
        + "angle."));
    String unit = body ? " mm" : "°";
    int range = body ? 300 : 90;
    String[] axes = body ? new String[] {"X (side)", "Y (up)", "Z (forward)"} : new String[] {"X", "Y", "Z"};
    for (int a = 0; a < 3; a++) content.addView(slider(axes[a], -range, range, Math.round(editDelta[a]), unit, a));

    TextView frames = text("Frames", 13, TEXT, true);
    frames.setPadding(0, dp(8), 0, 0);
    content.addView(frames);
    content.addView(note("Pause, drag the timeline above to a frame, then tap From or To."));
    LinearLayout fr = row();
    Button from = pill("From " + editFrom, false);
    from.setOnClickListener(v -> { editFrom = Math.min((int) frame, last); if (editFrom > editTo) editTo = editFrom; showTab(3); });
    Button to = pill("To " + editTo, false);
    to.setOnClickListener(v -> { editTo = Math.min((int) frame, last); if (editTo < editFrom) editFrom = editTo; showTab(3); });
    Button all = pill("Whole", editFrom == 0 && editTo == last);
    all.setOnClickListener(v -> { editFrom = 0; editTo = last; showTab(3); });
    fr.addView(from, chip());
    fr.addView(to, chip());
    fr.addView(all, chip());
    content.addView(fr);
    Button cut = pill("Cut: keep " + editFrom + "–" + editTo, false);
    cut.setOnClickListener(v -> {
      String why = StudioBridge.editTrim(clipEntry(), editFrom, editTo);
      if (!why.isEmpty()) { toast(why); return; }
      editClips = StudioBridge.editClips();
      editFrom = 0;
      editTo = -1;
      frame = 0;
      preview();
      showTab(3);
    });
    content.addView(cut, item());
    content.addView(slider("Soft edge", 0, 15, editFade, " frames", 3));

    LinearLayout act = row();
    Button apply = pill("Apply", true);
    apply.setOnClickListener(v -> {
      float k = body ? 1f : (float) (Math.PI / 180.0);
      String why = ik
          ? StudioBridge.editReach(clipEntry(), -10 - editPart, editDelta[0], editDelta[1], editDelta[2], editFrom,
              editTo, editFade)
          : StudioBridge.editAdjust(clipEntry(), body ? 0 : editPart, body, editDelta[0] * k, editDelta[1] * k,
              editDelta[2] * k, editFrom, editTo, editFade);
      if (why.startsWith("!")) toast(why.substring(1));
      else if (!why.isEmpty()) { toast(why); return; }
      editDelta[0] = editDelta[1] = editDelta[2] = 0;
      preview();
      showTab(3);
    });
    Button undo = pill("Undo", false);
    undo.setOnClickListener(v -> {
      if (StudioBridge.editUndo() < 0) { toast("Nothing to undo"); return; }
      preview();
      showTab(3);
    });
    Button zero = pill("Reset sliders", false);
    zero.setOnClickListener(v -> { editDelta[0] = editDelta[1] = editDelta[2] = 0; showTab(3); });
    act.addView(apply, chip());
    act.addView(undo, chip());
    act.addView(zero, chip());
    content.addView(act);

    TextView sp = text("Speed", 13, TEXT, true);
    sp.setPadding(0, dp(8), 0, 0);
    content.addView(sp);
    content.addView(slider("Speed", 50, 200, editSpeedPct, " %", 4));
    Button speedApply = pill("Apply speed", false);
    speedApply.setOnClickListener(v -> {
      if (editSpeedPct == 100) { toast("The speed is 100 %"); return; }
      String why = StudioBridge.editSpeed(clipEntry(), editSpeedPct / 100f);
      if (!why.isEmpty()) { toast(why); return; }
      editSpeedPct = 100;
      editClips = StudioBridge.editClips();
      preview();
      showTab(3);
    });
    content.addView(speedApply, item());

    Button save = card("Save as…", "a .dat for your mod pack's melee folder (" + editName + ")");
    save.setOnClickListener(v -> {
      android.content.Intent i = new android.content.Intent(android.content.Intent.ACTION_CREATE_DOCUMENT);
      i.addCategory(android.content.Intent.CATEGORY_OPENABLE);
      i.setType("application/octet-stream");
      i.putExtra(android.content.Intent.EXTRA_TITLE, editName);
      try { startActivityForResult(i, SAVE_ANIM); } catch (Exception e) { toast("No file picker: " + e.getMessage()); }
    });
    content.addView(save, item());
  }

  // a labelled slider: which 0..2 the bone's X, Y, Z, 3 the soft edge, 4 the speed
  private LinearLayout slider(String label, int min, int max, int value, String unit, int which) {
    LinearLayout box = new LinearLayout(this);
    box.setOrientation(LinearLayout.VERTICAL);
    TextView t = text(label + ": " + value + unit, 12, DIM, false);
    SeekBar s = new SeekBar(this);
    s.setMax(max - min);
    s.setProgress(value - min);
    s.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
      @Override public void onProgressChanged(SeekBar b, int p, boolean user) {
        int v = p + min;
        t.setText(label + ": " + v + unit);
        if (which < 3) editDelta[which] = v;
        else if (which == 3) editFade = v;
        else editSpeedPct = v;
      }
      @Override public void onStartTrackingTouch(SeekBar b) {}
      @Override public void onStopTrackingTouch(SeekBar b) {}
    });
    box.addView(t);
    box.addView(s);
    return box;
  }

  private void chooseClip() {
    String[] names = new String[editClips.length];
    for (int i = 0; i < names.length; i++) names[i] = clipLabel(i);
    new android.app.AlertDialog.Builder(this).setTitle("Animation").setItems(names, (d, i) -> {
      editClip = i;
      editFrom = 0;
      editTo = clipLast();
      editDelta[0] = editDelta[1] = editDelta[2] = 0;
      preview();
      showTab(3);
    }).show();
  }

  private void chooseBone() {
    int[] parts = StudioBridge.editParts(clipEntry(), false);
    int[] moves = StudioBridge.editParts(clipEntry(), true);
    boolean root = false;
    for (int p : moves) root |= p == 0;
    List<Integer> list = new ArrayList<>();
    for (int k = 0; k < IK_ENDS.length; k++) list.add(-10 - k);
    if (root) list.add(-1);
    for (int p : parts) list.add(p);
    String[] names = new String[list.size()];
    for (int i = 0; i < names.length; i++) names[i] = boneName(list.get(i));
    new android.app.AlertDialog.Builder(this).setTitle("Bone").setItems(names, (d, i) -> {
      editPart = list.get(i);
      editDelta[0] = editDelta[1] = editDelta[2] = 0;
      showTab(3);
    }).show();
  }

  private void openEdit(String path, String name) {
    String why = path == null ? "cannot be read" : StudioBridge.editOpen(path);
    if (!why.isEmpty()) { toast(name + ": " + why); return; }
    editName = name.contains(" · ") ? name.substring(name.lastIndexOf(" · ") + 3) : name;
    editClips = StudioBridge.editClips();
    editClip = editClips.length > 0 ? 0 : -1;
    editFrom = 0;
    editTo = -1;
    editDelta[0] = editDelta[1] = editDelta[2] = 0;
    toast(name + ": " + editClips.length + " animations");
    preview();
    showTab(3);
  }

  // the edited archive on the model (cache/edit_preview.dat), the chosen animation playing
  private void preview() {
    if (editClip < 0 || !modelLoaded) return;
    String why = StudioBridge.editPreview(new java.io.File(getCacheDir(), "edit_preview.dat").getPath(), clipEntry());
    if (!why.isEmpty()) { toast(why); return; }
    length = StudioBridge.motionLength();
    if (frame > length) frame = 0;
    info.setText(StudioBridge.info() + " · " + clipLabel(editClip));
  }

  // ---- actions
  // an archive opened shows its whole model at once, as the game puts it together (DevJunior, 19.11.2026: "the models
  // should open complete, easily")
  private void openArchive(String rel, String name) {
    String path = files.path(rel);
    String why = path == null ? "cannot be read" : StudioBridge.openArchive(path, name);
    if (!why.isEmpty()) {
      toast(name + ": " + why);
      return;
    }
    archivePath = rel;
    archiveName = name;
    loadComplete();
  }

  private void loadComplete() {
    String why = StudioBridge.loadComplete();
    highlighted = -1;
    if (!why.isEmpty()) {
      toast(archiveName + ": " + why);
      modelLoaded = false;
      showTab(0);
      return;
    }
    modelLoaded = true;
    length = 0;
    frame = 0;
    title.setText(archiveName);
    updateInfo();
    frameModel();
    showTab(0);
  }

  private void loadModel(int index, String label) {
    String why = StudioBridge.loadModel(index, -1);
    highlighted = -1;
    if (!why.isEmpty()) {
      toast(label + ": " + why);
      return;
    }
    modelLoaded = true;
    length = 0;
    frame = 0;
    title.setText(archiveName + " · " + label + " alone");
    updateInfo();
    frameModel();
    showTab(0);
  }

  private void pickMotion(int clip, String label) {
    String why = StudioBridge.setMotion(clip);
    if (!why.isEmpty()) {
      toast(label + ": " + why);
      return;
    }
    frame = 0;
    length = StudioBridge.motionLength();
    playing = true;
    play.setText("Pause");
    updateInfo();
    info.setText(info.getText() + (clip >= 0 ? " · " + label : ""));
  }

  private void updateInfo() { info.setText(StudioBridge.info()); }

  // ---- files (below the game's folder game/, StudioFiles)
  // the archives of the source: rel, file name, pack name ("" none), size
  private List<String[]> archives() {
    List<String[]> out = new ArrayList<>();
    files.lastError = "";
    if (source.equals("mods")) {
      for (StudioFiles.Item p : files.list("mods")) {
        if (!p.dir) continue;
        collect("mods/" + p.name + "/em", p.name, out);
        collect("mods/" + p.name + "/files/em", p.name, out);
      }
    } else {
      collect(source + "/files/em", "", out);
      collect(source + "/files/etc", "", out);  // the core (etc/core.das: the HUD's textures), under "Other"
      // the game's fonts (DevJunior, 26.11.2026: the inventory in the game's own letters), under "Other" to save a copy
      collect(source + "/files/Font", "", out);
      collect(source + "/files/font", "", out);
    }
    List<String[]> kept = new ArrayList<>();
    for (String[] f : out) {
      String n = f[1].toLowerCase(Locale.ROOT);
      boolean pl = n.startsWith("pl"), em = n.startsWith("em"), wep = n.startsWith("wep");
      if (kind.equals("all") || (kind.equals("pl") && pl) || (kind.equals("em") && em) || (kind.equals("wep") && wep)
          || (kind.equals("other") && !pl && !em && !wep)) kept.add(f);
    }
    return kept;
  }

  private void collect(String dir, String pack, List<String[]> out) {
    for (StudioFiles.Item f : files.list(dir)) {
      String n = f.name.toLowerCase(Locale.ROOT);
      if (!f.dir && (n.endsWith(".drs") || n.endsWith(".das") || n.endsWith(".fnt"))) {
        out.add(new String[] {dir + "/" + f.name, f.name, pack, String.valueOf(f.size)});
      }
    }
  }

  private String discFile(String rel) {
    for (String disc : new String[] {"disc1", "disc2"}) {
      String r = disc + "/files/" + rel;
      if (files.size(r) >= 0) return r;
    }
    return null;
  }

  // the mod packs' animation archives (melee/*.dat: moves, reactions, finishers, kicks): rel, label
  private List<String[]> packMotions() {
    List<String[]> out = new ArrayList<>();
    for (StudioFiles.Item p : files.list("mods")) {
      if (!p.dir) continue;
      for (StudioFiles.Item f : files.list("mods/" + p.name + "/melee")) {
        if (!f.dir && f.name.toLowerCase(Locale.ROOT).endsWith(".dat")) {
          out.add(new String[] {"mods/" + p.name + "/melee/" + f.name, p.name + " · " + f.name});
        }
      }
    }
    return out;
  }

  // ---- the look
  private int dp(float v) {
    return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics()));
  }

  private TextView text(String s, float sp, int color, boolean bold) {
    TextView t = new TextView(this);
    t.setText(s);
    t.setTextSize(sp);
    t.setTextColor(color);
    if (bold) t.setTypeface(t.getTypeface(), android.graphics.Typeface.BOLD);
    return t;
  }

  private TextView note(String s) {
    TextView t = text(s, 12, DIM, false);
    t.setPadding(dp(2), dp(6), dp(2), dp(8));
    return t;
  }

  private GradientDrawable round(int color, int stroke) {
    GradientDrawable g = new GradientDrawable();
    g.setColor(color);
    g.setCornerRadius(dp(12));
    if (stroke != 0) g.setStroke(dp(1), stroke);
    return g;
  }

  private Button pill(String s, boolean on) {
    Button b = new Button(this);
    b.setText(s);
    b.setAllCaps(false);
    b.setTextSize(13);
    b.setPadding(dp(12), 0, dp(12), 0);
    b.setMinHeight(0);
    b.setMinimumHeight(0);
    b.setMinWidth(0);
    b.setMinimumWidth(0);
    style(b, on);
    return b;
  }

  private void style(Button b, boolean on) {
    b.setSelected(on);
    b.setTextColor(on ? 0xFF15171B : TEXT);
    b.setBackground(round(on ? ACCENT : CARD, on ? 0 : 0xFF2C313A));
  }

  private Button card(String head, String detail) {
    Button b = new Button(this);
    b.setAllCaps(false);
    b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
    b.setText(detail.isEmpty() ? head : head + "\n" + detail);
    b.setTextSize(13);
    b.setTextColor(TEXT);
    b.setPadding(dp(14), dp(10), dp(14), dp(10));
    b.setBackground(round(CARD, 0xFF2C313A));
    return b;
  }

  private void toggle(LinearLayout row, String label, int bit) {
    Button b = pill(label, (flags & bit) != 0);
    b.setOnClickListener(v -> {
      flags ^= bit;
      style(b, (flags & bit) != 0);
    });
    row.addView(b, chip());
  }

  private LinearLayout row() {
    LinearLayout r = new LinearLayout(this);
    r.setOrientation(LinearLayout.HORIZONTAL);
    r.setGravity(Gravity.CENTER_VERTICAL);
    r.setPadding(0, dp(2), 0, dp(4));
    return r;
  }

  private LinearLayout.LayoutParams chip() {
    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(34));
    lp.setMargins(0, 0, dp(6), 0);
    return lp;
  }

  private LinearLayout.LayoutParams item() {
    LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
        ViewGroup.LayoutParams.WRAP_CONTENT);
    lp.setMargins(0, dp(3), 0, dp(3));
    return lp;
  }

  private void toast(String s) {
    Log.i(TAG, s);
    Toast.makeText(this, s, Toast.LENGTH_SHORT).show();
  }

  @Override protected void onResume() {
    super.onResume();
    view.onResume();
    lastNanos = 0;
    // back from the game app's StudioWakeActivity (StudioFiles.wake): its files now
    if (files != null && !files.lastError.isEmpty() && !busy) {
      if (!udasFiles.isEmpty()) listSkins();
      showTab(tab);
    }
  }

  @Override protected void onPause() {
    view.onPause();
    super.onPause();
  }

  @Override protected void onDestroy() {
    main.removeCallbacks(ticker);
    if (files != null) files.close();
    super.onDestroy();
  }
}
