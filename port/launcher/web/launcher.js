/* Hercules launcher stage: title-menu look of the 1997 game (design.md).
 * Screens: main menu, PASSWÖRTER (start level, Meg's room), OPTIONEN,
 * SPIEL LADEN. Talks to hercules_launcher.py through the WebKit message
 * handler "herc"; without it (plain browser) it runs on demo data.
 * Layout is in PAL pixels (320 x 256); the art is SC times larger (HD). */
"use strict";

const A = window.HERC_ASSETS;
const W = 320, H = 256;
const SC = (A && A.scale) || 1;
if (SC > 1) document.body.classList.add("hd");
const scene = document.getElementById("scene");
const stage = document.getElementById("stage");

/* ------------------------------------------------------------------ bridge */
const bridge = (() => {
  const native = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.herc;
  let next = 1;
  const waiting = new Map();
  const listeners = {};
  window.hercBridge = {
    reply(id, data) { const f = waiting.get(id); waiting.delete(id); if (f) f(data); },
    event(name, data) { (listeners[name] || []).forEach(f => f(data)); },
  };
  const demo = {
    cfg: { start_level: "off", internal_resolution: "1080p", aspect_ratio: "16:9", fullscreen: 0,
           vsync: "on", hd_sprites: true, hd_levels: true, hd_hud: true, texture_filtering: "nearest",
           antialiasing: false, perspective_texturing: false, geometry_correction: false,
           crt_sim: false, crt_mask: 0.5, crt_bloom: 0.35, scanlines: false, scanline_strength: 0.35,
           crt_filter: "raw", frequency: 48000, spu_hq: true, auto_skip_fmv: false, rewind: true,
           skip_intro: true, skip_memcard: true,
           language: (navigator.language || "de").slice(0, 2) },
    states: [{ slot: 2, name: "", mtime: 1791830880, thumb: "" },
             { slot: 1, name: "LEVEL 2 ANFANG", mtime: 1791830820, thumb: "" }],
    selected_slot: null,
    hd: { pack: false, running: false, progress: null, missing: [] },
  };
  function call(cmd, args = {}) {
    if (!native) {
      if (cmd === "state") return Promise.resolve(JSON.parse(JSON.stringify(demo)));
      if (cmd === "hd_status") return Promise.resolve(JSON.parse(JSON.stringify(demo.hd)));
      if (cmd === "hd_start") {
        demo.hd = { pack: false, running: true, missing: [],
                    progress: { fraction: 0.37, eta: 9000, state: "running", step: "upscale-sprites" } };
        return Promise.resolve({ ok: true });
      }
      if (cmd === "hd_cancel") { demo.hd.running = false; return Promise.resolve({ ok: true }); }
      if (cmd === "set") { demo.cfg[args.key] = args.value; return Promise.resolve({ ok: true }); }
      if (cmd === "delete") { demo.states = demo.states.filter(s => s.slot !== args.slot); return Promise.resolve({ ok: true }); }
      return Promise.resolve({ ok: true, demo: true });
    }
    const id = next++;
    return new Promise(res => {
      waiting.set(id, res);
      native.postMessage(JSON.stringify({ id, cmd, args }));
    });
  }
  return { call, on(name, f) { (listeners[name] = listeners[name] || []).push(f); } };
})();

/* ---------------------------------------------------------------- drawing */
function img(src, x, y, cls) {
  const el = document.createElement("img");
  el.src = "assets/" + src;
  el.alt = "";
  // HD art: lay out at PAL size unless the caller sets a size
  el.addEventListener("load", () => { if (!el.style.width) el.style.width = el.naturalWidth / SC + "px"; });
  el.style.left = x + "px";
  el.style.top = y + "px";
  if (cls) el.className = cls;
  scene.appendChild(el);
  return el;
}
function bg(key, cls = "bg") { return img(A.bg[key], 0, 0, cls); }

const fontImg = {};
for (const [k, f] of Object.entries(A.font.files)) { const i = new Image(); i.src = "assets/" + f; fontImg[k] = i; }

/* Text in the game's pause-menu font; color: yellow | orange | blue.
 * The glyph cell has room for accents above (y is the cell top). */
function glyph(ch) { return A.font.glyphs[ch] || A.font.glyphs[" "]; }
function textWidth(s) {
  let w = 0;
  for (const ch of s.toUpperCase()) w += glyph(ch)[1] + 1;
  return Math.max(0, w - 1);
}
function text(s, x, y, color = "yellow", align = "left") {
  s = s.toUpperCase();
  const w = textWidth(s), h = A.font.height, top = A.font.top;
  const c = document.createElement("canvas");
  c.width = Math.max(1, w * SC); c.height = h * SC;
  c.style.width = Math.max(1, w) + "px"; c.style.height = h + "px";
  const draw = () => {
    const g2 = c.getContext("2d");
    g2.imageSmoothingEnabled = false;
    let cx = 0;
    for (const ch of s) {
      const g = glyph(ch);
      g2.drawImage(fontImg[color], g[0] * SC, top * SC, g[1] * SC, h * SC, cx * SC, 0, g[1] * SC, h * SC);
      cx += g[1] + 1;
    }
  };
  fontImg[color].complete ? draw() : fontImg[color].addEventListener("load", draw, { once: true });
  c.style.left = (align === "center" ? Math.round(x - w / 2) : align === "right" ? x - w : x) + "px";
  c.style.top = y + "px";
  c.setAttribute("aria-label", s);
  scene.appendChild(c);
  return c;
}
function hit(x, y, w, h, onHover, onClick, label) {
  const d = document.createElement("div");
  d.className = "hit";
  Object.assign(d.style, { left: x + "px", top: y + "px", width: w + "px", height: h + "px" });
  if (label) { d.setAttribute("role", "button"); d.setAttribute("aria-label", label); }
  d.addEventListener("mouseenter", onHover);
  d.addEventListener("click", onClick);
  scene.appendChild(d);
  return d;
}
/* Lines of text, word-wrapped to maxW and centered on x; returns the height. */
const LINE = 15;
function wrap(s, maxW) {
  s = s.toUpperCase();
  if (s.includes("\n")) return s.split("\n").flatMap(part => wrap(part, maxW));
  if (textWidth(s) <= maxW) return [s];        // keeps runs of spaces (help lines)
  const lines = [];
  let line = "";
  for (const w of s.toUpperCase().split(/\s+/)) {
    if (line && textWidth(line + " " + w) > maxW) { lines.push(line); line = w; }
    else line = line ? line + " " + w : w;
  }
  if (line) lines.push(line);
  return lines;
}
function textBlock(s, x, y, color = "yellow", maxW = 280) {
  const lines = wrap(s, maxW);
  lines.forEach((l, i) => text(l, x, y + i * LINE, color, "center"));
  return lines.length * LINE + (A.font.height - LINE);
}
function centered(src, y, onPlaced) {
  const i = img(src, 0, y);
  const place = () => {
    i.style.left = Math.round((W - i.naturalWidth / SC) / 2) + "px";
    if (onPlaced) onPlaced(i.naturalHeight / SC);
  };
  i.complete && i.naturalWidth ? place() : i.addEventListener("load", place, { once: true });
  return i;
}

/* ------------------------------------------------------------- language */
function lang() { const l = S && S.cfg.language; return STR[l] ? l : "en"; }
function T(key, vars) {
  let v = STR[lang()][key];
  if (vars) for (const [k, x] of Object.entries(vars)) v = v.replace("{" + k + "}", x);
  return v;
}
/* a caption: the game's German sprite, or the same words in the font */
function caption(sprite, key, y, color = "yellow") {
  if (lang() === "de" && sprite) centered(sprite, y);
  else textBlock(T(key), W / 2, y, color, 300);
}
function levelName(lv) { return STR[lang()].levels[LEVEL_KEYS.indexOf(lv === "off" || lv === "1" ? "2" : lv)]; }
function fmtTime(t) {
  const d = new Date(t * 1000), p = n => String(n).padStart(2, "0");
  const date = lang() === "de" ? `${p(d.getDate())}.${p(d.getMonth() + 1)}.${d.getFullYear()}`
    : lang() === "en" ? `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}`
    : `${p(d.getDate())}/${p(d.getMonth() + 1)}/${d.getFullYear()}`;
  return `${date} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

/* sprite loops at a low, period frame rate */
let timers = [];
function animate(el, frames, fps = 8) {
  let n = 0;
  const t = setInterval(() => { n = (n + 1) % frames.length; el.src = "assets/" + frames[n]; }, 1000 / fps);
  timers.push(t);
}
function clear() {
  timers.forEach(clearInterval); timers = [];
  scene.textContent = "";
}

/* zoom (not a transform) so the HD art is drawn at screen resolution; the
   remaining horizontal stretch to 4:3 (PAL pixels are wide) is a scaleX */
function fit() {
  const r = stage.getBoundingClientRect();
  const z = r.height / H;
  scene.style.zoom = z;
  scene.style.transform = `scaleX(${r.width / (W * z)})`;
}
window.addEventListener("resize", fit);

/* ------------------------------------------------------------------ state */
let S = null;                 // {cfg, states, selected_slot, ...} from Python
let screen = null;            // current screen object {render, key}
const history = [];

function go(next, push = true) {
  if (push && screen) history.push(screen);
  screen = next;
  render();
}
function back() { if (history.length) { screen = history.pop(); render(); } }
function render() { clear(); screen.render(); fit(); }
async function set(key, value) { S.cfg[key] = value; await bridge.call("set", { key, value }); }

/* ------------------------------------------------------------- main menu */
const mainMenu = {
  sel: 0,
  render() {
    bg("menu");
    const left = img(A.sprite.muse_a[0], 6, 100), left2 = img(A.sprite.muse_b[0], 40, 104);
    const right = img(A.sprite.muse_c[0], 258, 100), right2 = img(A.sprite.muse_a[3], 224, 104);
    right2.style.transform = "scaleX(-1)";
    animate(left, A.sprite.muse_a, 7); animate(left2, A.sprite.muse_b, 6);
    animate(right, A.sprite.muse_c, 7); animate(right2, A.sprite.muse_a, 5);
    const labels = T("menu");
    if (lang() === "de") img(A.sprite.menu[this.sel], 52, 98);
    else labels.forEach((label, i) => text(label, W / 2, 101 + i * 26, i === this.sel ? "blue" : "orange", "center"));
    labels.forEach((label, i) => hit(70, 98 + i * 26, 180, 25,
      () => { if (this.sel !== i) { this.sel = i; render(); } }, () => this.activate(), label));
    const lv = S.cfg.start_level;
    if (lv !== "off") {
      if (lang() === "de" && A.sprite.level_name[lv]) centered(A.sprite.level_name[lv], 6);
      else textBlock(levelName(lv), W / 2, -5, "yellow", 300);
    }
    hdBar();
  },
  key(k) {
    if (k === "up") { this.sel = (this.sel + 3) % 4; render(); }
    else if (k === "down") { this.sel = (this.sel + 1) % 4; render(); }
    else if (k === "ok") this.activate();
  },
  activate() {
    if (this.sel === 0) play(null);
    else if (this.sel === 1) go(levelScreen);
    else if (this.sel === 2) go(optionsScreen);
    else go(loadScreen);
  },
};

/* --------------------------------------------- PASSWÖRTER: start level */
const LEVELS = ["off", "0", "2", "3", "5", "6", "7", "8", "9", "10", "11"];
const FRESCO = { off: "6", 0: "9", 2: "5", 3: "4", 5: "1", 6: "11", 7: "2", 8: "3", 9: "10", 10: "8", 11: "7" };
const levelScreen = {
  render() {
    bg("room");
    bg("beams", "bg beams");
    const lv = S.cfg.start_level;
    text(T("level_title"), W / 2, 2, "blue", "center");
    const f = A.fresco[FRESCO[lv]];
    if (f) {
      const fw = 128, fh = 102, fx = (W - fw) / 2, fy = 28;
      const frame = document.createElement("div");
      frame.className = "fresco-frame";
      Object.assign(frame.style, { left: fx + "px", top: fy + "px", width: fw + "px", height: fh + "px" });
      scene.appendChild(frame);
      const p = img(f, fx, fy); p.style.width = fw + "px"; p.style.height = fh + "px";
    }
    // level name (one or two lines), then the level number below it
    const label = lv === "off" ? T("level_normal") : T("level_n", { n: lv });
    const sprite = lang() === "de" && A.sprite.level_name[lv === "off" ? "1" : lv];
    if (sprite) {
      const num = text(label, W / 2, 152, "yellow", "center");
      centered(sprite, 134, h => { num.style.top = Math.round(134 + h - A.font.height + LINE + 6) + "px"; });
    } else {
      const h = textBlock(levelName(lv), W / 2, 130, "yellow", 200);
      text(label, W / 2, 130 + h + 2, "yellow", "center");
    }
    const meg = img(A.sprite.meg[0], 22, 96);
    animate(meg, A.sprite.meg, 6);
    text("<", 84, 68, "blue"); text(">", 228, 68, "blue");
    hit(70, 28, 30, 102, () => {}, () => this.key("left"), T("prev_level"));
    hit(222, 28, 30, 102, () => {}, () => this.key("right"), T("next_level"));
    hit(96, 28, 128, 102, () => {}, () => this.key("ok"), T("start_here"));
    caption(A.sprite.caption.back_help, "help_back", 234);
  },
  key(k) {
    const i = LEVELS.indexOf(S.cfg.start_level);
    if (k === "left" || k === "right") {
      const n = (i + (k === "left" ? -1 : 1) + LEVELS.length) % LEVELS.length;
      set("start_level", LEVELS[n]).then(render);
    } else if (k === "ok") play(null);
    else if (k === "back") back();
  },
};

/* --------------------------------------------------------------- options */
/* rows: [key, choices]; a choice label is a val.* key, "on"/"off", or a literal
   (numbers, "720P") when it starts with "=" */
const onoff = [[true, "on"], [false, "off"]];
const pct = vs => vs.map(v => [v, "=" + Math.round(v * 100)]);
const PAGES = [
  ["general", [
    ["language", LANGS.map(([k, n]) => [k, "=" + n])],
    ["hd_build", null],
  ]],
  ["video", [
    ["internal_resolution", [["native", "native"], ["720p", "=720P"], ["1080p", "=1080P"], ["1440p", "=1440P"], ["4k", "=4K"]]],
    ["aspect_ratio", [["4:3", "ratio43"], ["16:9", "ratio169"], ["21:9", "ratio219"]]],
    ["fullscreen", [[0, "window"], [1, "full"], [2, "exclusive"]]],
    ["vsync", [["on", "on"], ["adaptive", "adaptive"], ["vrr", "=VRR"], ["off", "off"]]],
    ["preset", [["original", "p_original"], ["hd", "p_hd"], ["custom", "p_custom"]]],
  ]],
  ["gfx", [
    ["hd_sprites", onoff],
    ["hd_levels", onoff],
    ["hd_hud", onoff],
    ["texture_filtering", [["nearest", "sharp"], ["bilinear", "soft"], ["xbr", "=XBR"]]],
    ["antialiasing", onoff],
    ["perspective_texturing", onoff],
    ["geometry_correction", onoff],
  ]],
  ["filter", [
    ["crt_sim", onoff],
    ["crt_mask", pct([0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1])],
    ["crt_bloom", pct([0, 0.1, 0.2, 0.3, 0.35, 0.4, 0.5, 0.6, 0.8, 1])],
    ["scanlines", onoff],
    ["scanline_strength", pct([0.1, 0.2, 0.3, 0.35, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1])],
    ["crt_filter", [["raw", "raw"], ["crt", "tube"], ["composite", "=COMPOSITE"], ["trinitron", "=TRINITRON"]]],
  ]],
  ["sound", [
    ["frequency", [[48000, "=48000"], [44100, "=44100"], [32000, "=32000"]]],
    ["spu_hq", [[true, "high"], [false, "normal"]]],
    ["auto_skip_fmv", [[false, "show"], [true, "skip"]]],
    ["rewind", onoff],
  ]],
];
function choiceLabel(c) {
  const k = c[1], L = STR[lang()];
  return k[0] === "=" ? k.slice(1) : k === "on" || k === "off" ? L[k] : L.val[k];
}
const optionsScreen = {
  page: 0, row: 0,
  render() {
    bg("plaster");
    const [pageKey, rows] = PAGES[this.page];
    const title = STR[lang()].pages[pageKey];
    text(T("options"), W / 2, 4, "yellow", "center");
    text("< " + title + " >", W / 2, 28, this.row === 0 ? "blue" : "orange", "center");
    hit(60, 30, 200, 18, () => { this.row = 0; render(); }, () => this.step(1), T("page") + " " + title);
    rows.forEach(([key, choices], i) => {
      const y = 54 + i * 20, active = this.row === i + 1, label = STR[lang()].opt[key];
      text(label, 20, y, active ? "blue" : "yellow");
      text(this.valueLabel(key, choices), 300, y, active ? "blue" : "orange", "right");
      hit(14, y + 4, 292, 18, () => { if (!active) { this.row = i + 1; render(); } },
          () => { this.row = i + 1; this.step(1); }, label);
    });
    caption(A.sprite.caption.back_help, "help_back", 234);
  },
  current(key) {
    if (key === "preset") return S.preset || "custom";
    return S.cfg[key];
  },
  valueLabel(key, choices) {
    if (key === "hd_build") return hdLabel();
    const v = this.current(key);
    let best = choices[0];
    for (const c of choices) if (c[0] === v) best = c;
    if (typeof v === "number" && !choices.some(c => c[0] === v))
      best = choices.reduce((a, c) => Math.abs(c[0] - v) < Math.abs(a[0] - v) ? c : a);
    return choiceLabel(best);
  },
  async step(d) {
    if (this.row === 0) {
      this.page = (this.page + d + PAGES.length) % PAGES.length;
      return render();
    }
    const [key, choices] = PAGES[this.page][1][this.row - 1];
    if (key === "hd_build") return hdToggle();
    const v = this.current(key);
    let i = choices.findIndex(c => c[0] === v);
    if (i < 0) i = 0;
    const nv = choices[(i + d + choices.length) % choices.length][0];
    if (key === "preset") {
      if (nv !== "custom") { const r = await bridge.call("preset", { name: nv }); S = r.state || S; }
    } else {
      await set(key, nv);
      S = await bridge.call("state");
    }
    render();
  },
  key(k) {
    const n = PAGES[this.page][1].length + 1;
    if (k === "up") { this.row = (this.row + n - 1) % n; render(); }
    else if (k === "down") { this.row = (this.row + 1) % n; render(); }
    else if (k === "left") this.step(-1);
    else if (k === "right" || k === "ok") this.step(1);
    else if (k === "back") back();
  },
};

/* ------------------------------------------------------------ SPIEL LADEN */
const loadScreen = {
  i: 0, confirm: false, yes: false,
  render() {
    bg("plaster2");
    caption(A.sprite.caption.load, "load_title", 6);
    const st = S.states;
    if (!st.length) {
      caption(A.sprite.caption.empty, "empty", 110);
      caption(A.sprite.caption.back_help, "help_back", 234);
      return;
    }
    this.i = Math.min(this.i, st.length - 1);
    const show = (k, x, y, w, h, on) => {
      const s = st[k];
      const f = document.createElement("div");
      f.className = "thumb-frame" + (on ? "" : " off");
      Object.assign(f.style, { left: x + "px", top: y + "px", width: w + "px", height: h + "px" });
      scene.appendChild(f);
      if (s.thumb) { const t = img("", x, y); t.src = s.thumb; t.style.width = w + "px"; t.style.height = h + "px"; if (!on) t.style.opacity = 0.55; }
      hit(x, y, w, h, () => {}, () => { if (k === this.i) this.key("ok"); else { this.i = k; render(); } },
          T("save_label") + " " + (s.name || T("slot", { n: s.slot + 1 })));
    };
    if (this.i > 0) show(this.i - 1, 12, 74, 64, 48, false);
    if (this.i < st.length - 1) show(this.i + 1, 244, 74, 64, 48, false);
    show(this.i, 96, 50, 128, 96, true);
    const s = st[this.i];
    text(s.name || T("slot", { n: s.slot + 1 }), W / 2, 148, "blue", "center");
    text(fmtTime(s.mtime), W / 2, 166, "yellow", "center");
    text(T("of", { i: this.i + 1, n: st.length }), W / 2, 184, "orange", "center");
    textBlock(T("help_load"), W / 2, 204, "yellow", 300);
    if (this.confirm) {
      const d = document.createElement("div"); d.className = "dim"; scene.appendChild(d);
      text(T("delete_q"), W / 2, 92, "yellow", "center");
      text(T("yes"), 120, 120, this.yes ? "blue" : "yellow", "center");
      text(T("no"), 200, 120, this.yes ? "yellow" : "blue", "center");
      hit(96, 124, 48, 20, () => { this.yes = true; render(); }, () => this.answer(true), T("yes"));
      hit(172, 124, 56, 20, () => { this.yes = false; render(); }, () => this.answer(false), T("no"));
    }
  },
  async answer(yes) {
    this.confirm = false;
    if (yes) { await bridge.call("delete", { slot: S.states[this.i].slot }); S = await bridge.call("state"); }
    render();
  },
  key(k) {
    if (this.confirm) {
      if (k === "left" || k === "right") { this.yes = !this.yes; render(); }
      else if (k === "ok") this.answer(this.yes);
      else if (k === "back") { this.confirm = false; render(); }
      return;
    }
    const n = S.states.length;
    if (k === "left" && n) { this.i = (this.i + n - 1) % n; render(); }
    else if (k === "right" && n) { this.i = (this.i + 1) % n; render(); }
    else if (k === "ok" && n) play(S.states[this.i].slot);
    else if (k === "delete" && n) { this.confirm = true; this.yes = false; render(); }
    else if (k === "back") back();
  },
};

/* ----------------------------------------------------------- play / wait */
let running = false;
async function play(slot) {
  if (running) return;
  running = true;
  const r = await bridge.call("play", { slot });
  if (!r || !r.ok) {
    running = false;
    const e = (r && r.error) || "start_failed";
    showMessage(STR[lang()].err[e] || e);
  }
}
bridge.on("game_ended", async d => {
  running = false;
  S = await bridge.call("state");
  if (d && d.code) showMessage(STR[lang()].err.exit.replace("{n}", d.code));
  else render();
});
function showMessage(msg) {
  render();
  const d = document.createElement("div"); d.className = "dim"; scene.appendChild(d);
  textBlock(msg, W / 2, 96, "yellow", 280);
}

/* ------------------------------------------------------------------ input */
const KEYS = {
  ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right",
  Enter: "ok", " ": "ok", x: "ok", X: "ok",
  Escape: "back", Backspace: "back",
  Delete: "delete", q: "delete", Q: "delete",
};
document.addEventListener("keydown", e => {
  const k = KEYS[e.key];
  if (!k || running) return;
  e.preventDefault();
  dispatch(k);
});
function dispatch(k) { screen.key(k); }
/* gamepad: d-pad / left stick, Cross or Start = ok, Triangle / Circle = back, Square = delete */
let padPrev = {};
function pollPad() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  for (const p of pads) {
    if (!p) continue;
    const now = {
      up: p.buttons[12]?.pressed || p.axes[1] < -0.6, down: p.buttons[13]?.pressed || p.axes[1] > 0.6,
      left: p.buttons[14]?.pressed || p.axes[0] < -0.6, right: p.buttons[15]?.pressed || p.axes[0] > 0.6,
      ok: p.buttons[0]?.pressed || p.buttons[9]?.pressed, back: p.buttons[3]?.pressed || p.buttons[1]?.pressed,
      delete: p.buttons[2]?.pressed,
    };
    for (const [k, v] of Object.entries(now)) if (v && !padPrev[k] && !running) dispatch(k);
    padPrev = now;
  }
  requestAnimationFrame(pollPad);
}
requestAnimationFrame(pollPad);

/* ------------------------------------------- HD pack built from the disc */
function hdPhase() {
  const h = S.hd || {};
  if (h.pack && !h.running) return "ready";
  if (h.running) return "running";
  if (h.progress && h.progress.state !== "done") return "paused";
  return "none";
}
function fmtEta(sec) {
  if (sec == null) return "…";
  const m = Math.max(1, Math.round(sec / 60));
  return m < 60 ? `${m} MIN` : `${Math.floor(m / 60)}:${String(m % 60).padStart(2, "0")} H`;
}
function hdPercent() { return Math.floor(((S.hd.progress || {}).fraction || 0) * 100); }
function hdLabel() {
  const ph = hdPhase(), V = STR[lang()].val;
  if (ph === "ready") return V.hd_ready;
  if (ph === "running") return hdPercent() + " %";
  if (ph === "paused") return V.hd_paused.replace("{p}", hdPercent());
  return (S.hd.missing || []).length ? V.hd_unavailable : V.hd_make;
}
async function hdToggle() {
  const ph = hdPhase();
  if (ph === "running") await bridge.call("hd_cancel");
  else if (ph !== "ready") {
    const r = await bridge.call("hd_start");
    if (r && !r.ok) showMessage(T("hd_missing") + " " + (r.missing || []).join(", ").toUpperCase());
  }
  S.hd = await bridge.call("hd_status");
  render();
}
/* bottom line on the main menu while the pack is being made */
function hdBar() {
  const ph = hdPhase();
  if (ph !== "running" && ph !== "paused") return;
  const p = hdPercent(), w = 200, x = (W - w) / 2, y = 249;
  const track = document.createElement("div");
  Object.assign(track.style, { position: "absolute", left: x + "px", top: y + "px", width: w + "px",
    height: "5px", background: "rgba(20,8,4,0.75)", boxShadow: "0 0 0 1px #6b3a1a" });
  const fill = document.createElement("div");
  Object.assign(fill.style, { width: p + "%", height: "100%", background: "#ffd038" });
  track.appendChild(fill);
  scene.appendChild(track);
  const msg = ph === "running"
    ? T("hd_progress", { p, eta: fmtEta((S.hd.progress || {}).eta) })
    : STR[lang()].val.hd_paused.replace("{p}", p);
  text(msg, W / 2, y - 24, "yellow", "center");
}
/* refresh the progress every few seconds without restarting the animations */
setInterval(async () => {
  if (!S || running || hdPhase() === "ready" && !(S.hd || {}).running) return;
  const before = JSON.stringify(S.hd);
  S.hd = await bridge.call("hd_status");
  if (JSON.stringify(S.hd) !== before && (screen === mainMenu || screen === optionsScreen)) render();
}, 5000);

const hdPrompt = {
  yes: true,
  render() {
    bg("plaster");
    text(T("hd_title"), W / 2, 30, "yellow", "center");
    textBlock(T("hd_body"), W / 2, 64, "orange", 280);
    text(T("hd_yes"), 110, 186, this.yes ? "blue" : "yellow", "center");
    text(T("hd_later"), 210, 186, this.yes ? "yellow" : "blue", "center");
    hit(70, 190, 80, 20, () => { this.yes = true; render(); }, () => this.answer(true), T("hd_yes"));
    hit(170, 190, 80, 20, () => { this.yes = false; render(); }, () => this.answer(false), T("hd_later"));
  },
  async answer(yes) {
    await set("hd_prompted", true);
    if (yes) await bridge.call("hd_start");
    S.hd = await bridge.call("hd_status");
    go(mainMenu, false);
  },
  key(k) {
    if (k === "left" || k === "right") { this.yes = !this.yes; render(); }
    else if (k === "ok") this.answer(this.yes);
    else if (k === "back") this.answer(false);
  },
};

/* ------------------------------------------------------------------- boot */
(async () => {
  if (!A) return;
  S = await bridge.call("state");
  const h = S.hd || {};
  screen = !h.pack && !h.running && !h.progress && !S.cfg.hd_prompted && !(h.missing || []).length
    ? hdPrompt : mainMenu;
  render();
  fit();
})();
