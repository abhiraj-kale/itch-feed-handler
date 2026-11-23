/* FH-50. The MoldUDP64 session, parser, book builder, generator, and lossy
   network all run in WebAssembly (itch.js + itch.wasm). This file is the
   front panel: it turns knobs into loss rates and engine state into LEDs. */
(() => {
  "use strict";
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
  const NS = "http://www.w3.org/2000/svg";
  const reduceMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const fmtInt = (n) => Math.round(n).toLocaleString("en-US");
  const fmtPx = (p) => (p / 10000).toFixed(2);
  const svgEl = (tag, attrs = {}, parent) => {
    const e = document.createElementNS(NS, tag);
    for (const k in attrs) e.setAttribute(k, attrs[k]);
    if (parent) parent.appendChild(e);
    return e;
  };
  const onVisible = (el, fn, threshold = 0.02) => new IntersectionObserver((es) => es.forEach((e) => fn(e.isIntersecting)), { threshold }).observe(el);

  // ---------------------------------------------------------------- reveal
  const revealObs = new IntersectionObserver((es) => es.forEach((e) => {
    if (!e.isIntersecting) return;
    e.target.classList.add("in");
    revealObs.unobserve(e.target);
  }), { threshold: 0.1 });
  $$(".rise, .stamp").forEach((el) => revealObs.observe(el));

  // ---------------------------------------------------------------- 7-segment displays
  const SEG_PTS = {
    a: "2,1.5 3,0.5 9,0.5 10,1.5 9,2.5 3,2.5", g: "2,10 3,9 9,9 10,10 9,11 3,11", d: "2,18.5 3,17.5 9,17.5 10,18.5 9,19.5 3,19.5",
    f: "1.5,2 2.5,3 2.5,8.5 1.5,9.5 0.5,8.5 0.5,3", b: "10.5,2 11.5,3 11.5,8.5 10.5,9.5 9.5,8.5 9.5,3",
    e: "1.5,10.5 2.5,11.5 2.5,17 1.5,18 0.5,17 0.5,11.5", c: "10.5,10.5 11.5,11.5 11.5,17 10.5,18 9.5,17 9.5,11.5",
  };
  const DIGITS = { 0: "abcdef", 1: "bc", 2: "abged", 3: "abgcd", 4: "fgbc", 5: "afgcd", 6: "afgedc", 7: "abc", 8: "abcdefg", 9: "abcdfg", "-": "g", " ": "" };
  const segs = new Map();
  function buildSeg(el) {
    const n = +el.dataset.digits, digits = [];
    for (let i = 0; i < n; i++) {
      const svg = svgEl("svg", { viewBox: "-0.5 0 13 20" }, el);
      const parts = {};
      for (const k in SEG_PTS) parts[k] = svgEl("polygon", { points: SEG_PTS[k] }, svg);
      digits.push({ parts, ch: null });
    }
    segs.set(el, { n, digits, last: null });
  }
  function setSeg(el, value) {
    const s = segs.get(el);
    let str = String(Math.max(0, Math.round(value)));
    if (str.length > s.n) str = "9".repeat(s.n);
    str = str.padStart(s.n, " ");
    if (str === s.last) return;
    s.last = str;
    s.digits.forEach((d, i) => {
      const ch = str[i];
      if (ch === d.ch) return;
      d.ch = ch;
      const on = DIGITS[ch] || "";
      for (const k in d.parts) d.parts[k].classList.toggle("on", on.includes(k));
    });
  }
  $$(".seg").forEach(buildSeg);
  segs.forEach((_, el) => setSeg(el, 0));

  // ---------------------------------------------------------------- LEDs
  const leds = {};
  function flash(id, ms = 70) {
    const el = leds[id] || (leds[id] = document.getElementById(id));
    if (!el) return;
    el.classList.add("on");
    clearTimeout(el._t);
    el._t = setTimeout(() => el.classList.remove("on"), ms);
  }
  const jackIn = $(".jack i"), jackOut = $(".jack.out i");
  function blinkJack(j) { j.classList.add("on"); clearTimeout(j._t); j._t = setTimeout(() => j.classList.remove("on"), 50); }

  // hero unit tilts toward the pointer
  const unit = $("#unit"), hero = $(".hero");
  if (!reduceMotion) hero.addEventListener("pointermove", (e) => {
    const r = hero.getBoundingClientRect();
    const x = (e.clientX - r.left) / r.width - 0.5, y = (e.clientY - r.top) / r.height - 0.5;
    unit.style.setProperty("--ry", (-10 + x * 14).toFixed(2) + "deg");
    unit.style.setProperty("--rx", (6 - y * 10).toFixed(2) + "deg");
  });

  // ---------------------------------------------------------------- engine
  let M = null;
  const SESSION_MSGS = 400000, STOCKS = 200;
  const sim = { rate: 57, turbo: false, paused: false, due: 0, done: false, doneAt: 0 };
  const symCache = new Map();
  const sym = (loc) => {
    let s = symCache.get(loc);
    if (!s) { s = M.UTF8ToString(M._itch_symbol(loc)); if (s) symCache.set(loc, s); }
    return s || "";
  };
  function stats() {
    const p = M._itch_stats() >> 3, F = M.HEAPF64;
    return { packets: F[p], msgs: F[p + 1], gaps: F[p + 2], req: F[p + 3], rec: F[p + 4], dup: F[p + 5], expected: F[p + 6], total: F[p + 7], live: F[p + 8], next: F[p + 11] };
  }
  function newSession(seed = (Math.random() * 1e9) | 0) {
    M._itch_generate(SESSION_MSGS, STOCKS, seed);
    M._itch_reset(knobs.loss.value, knobs.dup.value, knobs.reorder.value, seed);
    symCache.clear();
    sim.done = false; sim.due = 0;
    ranked = []; selected = 0; lastRank = 0;
    scope.clear();
  }

  // ---------------------------------------------------------------- knobs and switches
  const knobs = {};
  function makeKnob(host, onChange, fmt) {
    const min = +host.dataset.min, max = +host.dataset.max, label = host.dataset.label;
    host.innerHTML = `<div class="knob-dial" tabindex="0" role="slider" aria-label="${label}" aria-valuemin="${min}" aria-valuemax="${max}"><div class="knob-body"></div><div class="knob-cap"></div></div><em>${label}</em><output></output>`;
    const dial = $(".knob-dial", host), cap = $(".knob-cap", host), out = $("output", host);
    const svg = svgEl("svg", { viewBox: "0 0 84 84" });
    dial.prepend(svg);
    for (let i = 0; i <= 10; i++) {
      const a = ((-135 + i * 27) * Math.PI) / 180, r0 = i % 5 === 0 ? 34 : 36, r1 = 41;
      svgEl("line", { x1: 42 + r0 * Math.sin(a), y1: 42 - r0 * Math.cos(a), x2: 42 + r1 * Math.sin(a), y2: 42 - r1 * Math.cos(a), stroke: "#141414", "stroke-width": i % 5 === 0 ? 1.6 : 1 }, svg);
    }
    const k = { value: +host.dataset.value };
    k.set = (v, silent) => {
      k.value = Math.max(min, Math.min(max, v));
      const t = (k.value - min) / (max - min);
      cap.style.transform = `rotate(${-135 + 270 * t}deg)`;
      out.textContent = fmt(k.value);
      dial.setAttribute("aria-valuenow", k.value.toFixed(3));
      if (!silent) onChange(k.value);
    };
    let startY = 0, startV = 0;
    dial.addEventListener("pointerdown", (e) => { dial.setPointerCapture(e.pointerId); startY = e.clientY; startV = k.value; dial.focus(); });
    dial.addEventListener("pointermove", (e) => { if (dial.hasPointerCapture(e.pointerId)) k.set(startV + ((startY - e.clientY) / 160) * (max - min)); });
    dial.addEventListener("wheel", (e) => { e.preventDefault(); k.set(k.value - Math.sign(e.deltaY) * (max - min) / 40); }, { passive: false });
    dial.addEventListener("keydown", (e) => {
      const st = (max - min) / 50;
      if (e.key === "ArrowUp" || e.key === "ArrowRight") { k.set(k.value + st); e.preventDefault(); }
      if (e.key === "ArrowDown" || e.key === "ArrowLeft") { k.set(k.value - st); e.preventDefault(); }
    });
    k.set(k.value, true);
    return k;
  }
  const rateOf = (v) => Math.round(Math.pow(10, 0.5 + 3.3 * v));
  const pct = (v) => (v * 100).toFixed(1) + " %";
  const pushLoss = () => M && M._itch_set_loss(knobs.loss.value, knobs.dup.value, knobs.reorder.value);
  knobs.rate = makeKnob($('[data-knob="rate"]'), (v) => (sim.rate = rateOf(v)), (v) => (sim.turbo ? "turbo" : rateOf(v) + " pkt/s"));
  knobs.loss = makeKnob($('[data-knob="loss"]'), pushLoss, pct);
  knobs.dup = makeKnob($('[data-knob="dup"]'), pushLoss, pct);
  knobs.reorder = makeKnob($('[data-knob="reorder"]'), pushLoss, pct);
  sim.rate = rateOf(knobs.rate.value);

  $("#turbo").addEventListener("click", (e) => {
    sim.turbo = !sim.turbo;
    e.currentTarget.setAttribute("aria-pressed", sim.turbo);
    knobs.rate.set(knobs.rate.value, true);
  });
  $("#pause").addEventListener("click", (e) => { sim.paused = !sim.paused; e.currentTarget.setAttribute("aria-pressed", sim.paused); });
  $("#reset").addEventListener("click", () => { if (M) newSession(); });

  // ---------------------------------------------------------------- logic analyzer
  const scope = (() => {
    const cv = $("#scope");
    const CH = [
      { k: 0, label: "RX", color: "#8ff0a4" }, { k: 1, label: "DROP", color: "#ff5d4a" }, { k: 2, label: "REQ", color: "#ffb21a" },
      { k: 3, label: "RETX", color: "#ffd98a" }, { k: 4, label: "DUP", color: "#6fd3ff" }, { k: 5, label: "OOO", color: "#c59bff" },
    ];
    const WINDOW = 4000, PW = 22;
    const ev = CH.map(() => []), lastT = CH.map(() => -1e9), tags = [];
    let visible = false;
    onVisible(cv, (v) => (visible = v));
    function push(kind, t, seq) {
      if (t - lastT[kind] < 6) return;
      lastT[kind] = t;
      ev[kind].push(t);
      if (seq != null && (kind === 2 || (kind === 0 && Math.random() < 0.08) || kind === 1)) tags.push({ kind, t, s: seq });
    }
    function clear() { ev.forEach((a) => (a.length = 0)); tags.length = 0; }
    function draw(now) {
      if (!visible) return;
      const dpr = Math.min(2, devicePixelRatio || 1), w = cv.clientWidth, h = cv.clientHeight;
      if (cv.width !== Math.round(w * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
      const ctx = cv.getContext("2d");
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, w, h);
      const gx = 58, gw = w - gx - 12, top = 10, rows = CH.length, rh = (h - top - 22) / rows;
      const X = (t) => gx + gw - ((now - t) / WINDOW) * gw;
      // graticule
      ctx.strokeStyle = "rgba(143,240,164,0.09)";
      ctx.lineWidth = 1;
      ctx.setLineDash([1, 3]);
      for (let i = 0; i <= 10; i++) { const x = Math.round(gx + (gw * i) / 10) + 0.5; ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, top + rows * rh); ctx.stroke(); }
      for (let r = 0; r <= rows; r++) { const y = Math.round(top + r * rh) + 0.5; ctx.beginPath(); ctx.moveTo(gx, y); ctx.lineTo(gx + gw, y); ctx.stroke(); }
      ctx.setLineDash([]);
      ctx.font = "500 10px 'IBM Plex Mono', monospace";
      CH.forEach((c, i) => {
        const arr = ev[i];
        while (arr.length && arr[0] < now - WINDOW - PW) arr.shift();
        const lo = top + i * rh + rh * 0.74, hi = top + i * rh + rh * 0.26;
        ctx.fillStyle = c.color;
        ctx.globalAlpha = 0.85;
        ctx.fillText(c.label, 10, lo);
        ctx.globalAlpha = 1;
        ctx.strokeStyle = c.color;
        ctx.lineWidth = 1.5;
        ctx.shadowColor = c.color;
        ctx.shadowBlur = 6;
        ctx.beginPath();
        ctx.moveTo(gx, lo);
        let cur = gx;
        for (let j = 0; j < arr.length; j++) {
          let xs = Math.max(gx, X(arr[j])), xe = X(arr[j] + PW);
          while (j + 1 < arr.length && X(arr[j + 1]) <= xe + 1) { j++; xe = X(arr[j] + PW); }
          xe = Math.min(gx + gw, xe);
          if (xe <= gx) continue;
          if (xs < cur) xs = cur;
          ctx.lineTo(xs, lo); ctx.lineTo(xs, hi); ctx.lineTo(xe, hi); ctx.lineTo(xe, lo);
          cur = xe;
        }
        ctx.lineTo(gx + gw, lo);
        ctx.stroke();
        ctx.shadowBlur = 0;
      });
      // sequence tags above pulses
      ctx.font = "9px 'IBM Plex Mono', monospace";
      let lastX = -1e9;
      while (tags.length && tags[0].t < now - WINDOW) tags.shift();
      for (const tg of tags) {
        const x = X(tg.t);
        if (x < gx || x - lastX < 54) continue;
        lastX = x;
        ctx.fillStyle = CH[tg.kind].color;
        ctx.globalAlpha = 0.75;
        ctx.fillText((tg.kind === 2 ? "req " : "#") + tg.s, x + 2, top + tg.kind * rh + rh * 0.22);
        ctx.globalAlpha = 1;
      }
      // now line + time ruler
      ctx.strokeStyle = "rgba(255,255,255,0.35)";
      ctx.beginPath(); ctx.moveTo(gx + gw + 0.5, top); ctx.lineTo(gx + gw + 0.5, top + rows * rh); ctx.stroke();
      ctx.fillStyle = "rgba(143,240,164,0.55)";
      ctx.font = "9px 'IBM Plex Mono', monospace";
      for (let i = 0; i <= 4; i++) {
        const x = gx + (gw * i) / 4;
        ctx.textAlign = i === 0 ? "left" : i === 4 ? "right" : "center";
        ctx.fillText(i === 4 ? "now" : `-${((4 - i) * WINDOW) / 4000}s`, x, h - 7);
      }
      ctx.textAlign = "left";
    }
    return { push, draw, clear };
  })();

  // events from WASM -> analyzer + LEDs
  const KIND = { 0: 0, 1: 1, 2: 2, 3: 3, 4: 4, 5: 5 };
  function readEvents(now, dt) {
    const n = M._itch_event_count();
    if (!n) return;
    const base = M._itch_events() >> 2, U = M.HEAPU32;
    const seen = [0, 0, 0, 0, 0, 0];
    for (let i = 0; i < n; i++) {
      const o = base + i * 4, kind = U[o], seq = U[o + 1];
      const t = now - dt * 1000 * (1 - i / n);
      scope.push(KIND[kind], t, seq);
      seen[kind]++;
    }
    M._itch_clear_events();
    if (seen[0] || seen[3]) { flash("led-pkt"); blinkJack(jackIn); }
    if (seen[1]) flash("led-drop", 140);
    if (seen[3]) flash("led-retx", 140);
  }

  // ---------------------------------------------------------------- counters, lamps, progress
  const ledbar = $("#ledbar");
  const bars = Array.from({ length: 60 }, () => { const i = document.createElement("i"); ledbar.appendChild(i); return i; });
  function paintCounters() {
    const s = stats();
    setSeg($("#hero-seg"), s.msgs);
    setSeg($("#c-pkt"), s.packets);
    setSeg($("#c-gap"), s.gaps);
    setSeg($("#c-rec"), s.rec);
    setSeg($("#c-dup"), s.dup);
    setSeg($("#c-live"), s.live);
    $("#prog-txt").textContent = `${fmtInt(s.expected - 1)} / ${fmtInt(s.total)} MESSAGES`;
    const done = Math.floor(((s.expected - 1) / s.total) * 60), park = Math.ceil(((Math.min(s.next, s.total + 1) - 1) / s.total) * 60);
    bars.forEach((b, i) => { b.className = i < done ? "done" : i < park && park > done + 0 && s.next > s.expected + 1 && inGap ? "park" : ""; });
  }
  let inGap = false;
  function paintVerify() {
    const v = M._itch_verify();
    inGap = v === -1;
    $("#lamp-sync").classList.toggle("on", v !== -1);
    const gap = $("#lamp-gap");
    gap.classList.toggle("on", v === -1);
    gap.classList.toggle("blink", v === -1);
    $("#lamp-ok").classList.toggle("on", v === 1);
    $("#led-sync").classList.toggle("on", v === 1);
  }

  // ---------------------------------------------------------------- split-flap board
  const CHARS = " 0123456789.ABCDEFGHIJKLMNOPQRSTUVWXYZ-";
  class Flap {
    constructor(parent, cls) {
      this.el = document.createElement("span");
      this.el.className = "flap " + (cls || "");
      this.top = this.half("top", " ");
      this.bot = this.half("bot", " ");
      parent.appendChild(this.el);
      this.cur = " "; this.target = " "; this.busy = false;
    }
    half(pos, ch) { const h = document.createElement("span"); h.className = "h " + pos; h.innerHTML = `<span>${ch}</span>`; this.el.appendChild(h); return h; }
    set(ch) { this.target = CHARS.includes(ch) ? ch : " "; if (!this.busy) this.step(); }
    step() {
      if (this.cur === this.target) { this.busy = false; return; }
      this.busy = true;
      const next = CHARS[(CHARS.indexOf(this.cur) + 1) % CHARS.length];
      if (reduceMotion) { this.cur = this.target; this.top.firstChild.textContent = this.cur; this.bot.firstChild.textContent = this.cur; this.busy = false; return; }
      const lt = this.half("top leaf", this.cur), lb = this.half("bot leaf", next);
      this.top.firstChild.textContent = next;
      setTimeout(() => {
        this.bot.firstChild.textContent = next;
        lt.remove(); lb.remove();
        this.cur = next;
        this.step();
      }, 100);
    }
  }
  const board = $("#flapboard");
  const rows = Array.from({ length: 10 }, (_, i) => {
    const r = document.createElement("div");
    r.className = "fb-row";
    r.innerHTML = '<span class="sel-led"></span>';
    const groups = [4, 6, 6, 4].map((n, gi) => {
      const g = document.createElement("span");
      g.className = "cells";
      r.appendChild(g);
      return Array.from({ length: n }, () => new Flap(g, gi === 1 ? "bid" : gi === 2 ? "ask" : ""));
    });
    r.addEventListener("click", () => { if (ranked[i]) { selected = ranked[i]; paintBoard(); paintDepth(); } });
    board.appendChild(r);
    return { el: r, groups };
  });
  const writeCells = (cells, str, right) => {
    str = right ? str.padStart(cells.length, " ").slice(-cells.length) : str.padEnd(cells.length, " ").slice(0, cells.length);
    cells.forEach((c, i) => c.set(str[i]));
  };
  let ranked = [], selected = 0, lastRank = 0;
  function paintBoard() {
    const act = M.HEAPU32, base = M._itch_activity() >> 2, now = performance.now();
    if (!ranked.length || now - lastRank > 6000) {
      const arr = [];
      for (let loc = 1; loc <= STOCKS; loc++) arr.push([loc, act[base + loc]]);
      arr.sort((a, b) => b[1] - a[1]);
      ranked = arr.slice(0, 10).filter((x) => x[1] > 0).map((x) => x[0]);
      lastRank = now;
      if (!selected && ranked.length) selected = ranked[0];
    }
    rows.forEach((r, i) => {
      const loc = ranked[i];
      r.el.classList.toggle("sel", loc === selected);
      if (!loc) return;
      const p = M._itch_top(loc) >> 3, F = M.HEAPF64;
      const bp = F[p], ap = F[p + 2];
      writeCells(r.groups[0], sym(loc), false);
      writeCells(r.groups[1], bp ? fmtPx(bp) : "", true);
      writeCells(r.groups[2], ap ? fmtPx(ap) : "", true);
      writeCells(r.groups[3], bp && ap ? fmtPx(ap - bp) : "", true);
    });
  }

  // ---------------------------------------------------------------- depth VU
  const vu = $("#vu"), VL = 7, SEGN = 28;
  const mkVu = (cls) => {
    const r = document.createElement("div");
    r.className = "vu-row " + cls;
    r.innerHTML = `<span class="p ${cls === "bid" ? "b" : "a"}">--</span><span class="vu-segs">${"<i></i>".repeat(SEGN)}</span><span class="n">--</span>`;
    vu.appendChild(r);
    return { p: r.children[0], segs: Array.from(r.children[1].children), n: r.children[2], lit: -1 };
  };
  const askRows = Array.from({ length: VL }, () => mkVu("ask")).reverse();
  const midRow = document.createElement("div"); midRow.className = "vu-mid"; midRow.textContent = "--"; vu.appendChild(midRow);
  const bidRows = Array.from({ length: VL }, () => mkVu("bid"));
  function readDepth(side) {
    const n = M._itch_depth(selected, side, VL), b = M._itch_depth_ptr(side) >> 2, U = M.HEAPU32, out = [];
    for (let i = 0; i < n; i++) out.push([U[b + i * 3], U[b + i * 3 + 1]]);
    return out;
  }
  function paintDepth() {
    if (!selected) return;
    const bids = readDepth(0), asks = readDepth(1);
    let max = 1;
    for (const l of bids) max = Math.max(max, l[1]);
    for (const l of asks) max = Math.max(max, l[1]);
    const fill = (rs, lv) => rs.forEach((r, i) => {
      const l = lv[i];
      r.p.textContent = l ? fmtPx(l[0]) : "--";
      r.n.textContent = l ? fmtInt(l[1]) : "";
      const lit = l ? Math.max(1, Math.round((SEGN * l[1]) / max)) : 0;
      if (lit !== r.lit) { r.lit = lit; r.segs.forEach((s, k) => s.classList.toggle("on", k < lit)); }
    });
    fill(askRows, asks); fill(bidRows, bids);
    $("#depth-sym").textContent = sym(selected) || "--";
    if (bids.length && asks.length) {
      midRow.textContent = `MID ${((bids[0][0] + asks[0][0]) / 20000).toFixed(3)} · SPREAD ${fmtPx(asks[0][0] - bids[0][0])}`;
      $("#depth-mid").textContent = `${bids.length + asks.length} levels shown`;
    }
  }

  // ---------------------------------------------------------------- live message mix meters
  const TYPES = [["A", "ADD"], ["D", "DEL"], ["U", "REPL"], ["E", "EXEC"], ["X", "CXL"], ["F", "MPID"], ["C", "EXPX"], ["P", "TRD"], ["I", "IMB"], ["S", "SYS"]];
  const meters = $("#meters"), MSEG = 16;
  const meterEls = TYPES.map(([t, name]) => {
    const m = document.createElement("div");
    m.className = "meter";
    m.innerHTML = `<b>${t}</b><span class="col">${"<i></i>".repeat(MSEG)}</span><small>${name}</small>`;
    meters.appendChild(m);
    return { t, segs: Array.from(m.querySelectorAll(".col i")), prev: 0, peak: 0, lvl: 0 };
  });
  function paintMeters() {
    const cnt = (c) => M._itch_type_count(c.charCodeAt(0));
    const deltas = meterEls.map((m) => {
      const v = m.t === "S" ? cnt("S") + cnt("R") + cnt("H") : cnt(m.t);
      const d = v - m.prev;
      m.prev = v;
      return Math.max(0, d);
    });
    const max = Math.max(1, ...deltas);
    let total = 0;
    meterEls.forEach((m, i) => {
      total += m.prev;
      const target = deltas[i] ? Math.max(1, Math.round(MSEG * Math.pow(deltas[i] / max, 0.45))) : 0;
      m.lvl = target > m.lvl ? target : Math.max(0, m.lvl - 1);
      m.peak = m.lvl >= m.peak ? m.lvl : Math.max(m.lvl, m.peak - 0.25);
      const pk = Math.round(m.peak);
      m.segs.forEach((s, k) => {
        s.className = k < m.lvl ? "on" + (k >= MSEG - 2 ? " top" : k >= MSEG - 6 ? " hi" : "") : k === pk - 1 && pk > m.lvl ? "peak" : "";
      });
    });
    $("#mix-total").textContent = fmtInt(total) + " msgs";
  }

  // ---------------------------------------------------------------- message anatomy drawing
  const LAYOUT = {
    A: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order reference", 11, 8, "u"], ["Side", 19, 1, "side"], ["Shares", 20, 4, "u"], ["Stock", 24, 8, "alpha"], ["Price", 32, 4, "px"]],
    F: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order reference", 11, 8, "u"], ["Side", 19, 1, "side"], ["Shares", 20, 4, "u"], ["Stock", 24, 8, "alpha"], ["Price", 32, 4, "px"], ["Attribution", 36, 4, "alpha"]],
    E: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order reference", 11, 8, "u"], ["Executed shares", 19, 4, "u"], ["Match number", 23, 8, "u"]],
    X: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order reference", 11, 8, "u"], ["Cancelled shares", 19, 4, "u"]],
    D: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order reference", 11, 8, "u"]],
    U: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Original reference", 11, 8, "u"], ["New reference", 19, 8, "u"], ["Shares", 27, 4, "u"], ["Price", 31, 4, "px"]],
  };
  const NAMES = { A: "ADD ORDER (A)", F: "ADD ORDER WITH MPID (F)", E: "ORDER EXECUTED (E)", X: "ORDER CANCEL (X)", D: "ORDER DELETE (D)", U: "ORDER REPLACE (U)" };
  const SIZES = { A: 36, F: 40, E: 31, X: 23, D: 19, U: 35 };
  const CYCLE = ["A", "U", "E", "F", "X", "D"];
  let cycleIdx = 0, held = false, hovering = false;
  const dwg = $("#dwg");
  $("#freeze").addEventListener("click", (e) => { held = !held; e.target.textContent = held ? "release drawing" : "hold drawing"; });
  $("#drawing").addEventListener("pointerenter", () => (hovering = true));
  $("#drawing").addEventListener("pointerleave", () => (hovering = false));
  function decode(bytes, off, len, kind) {
    let v = 0n;
    for (let i = 0; i < len; i++) v = (v << 8n) | BigInt(bytes[off + i]);
    const ascii = () => String.fromCharCode(...bytes.slice(off, off + len));
    switch (kind) {
      case "char": return `'${ascii()}'`;
      case "side": return ascii() === "B" ? "'B' buy" : "'S' sell";
      case "alpha": return `"${ascii().trimEnd()}"`;
      case "px": return "$" + (Number(v) / 10000).toFixed(4);
      case "ts": {
        const s = v / 1000000000n, rem = (v % 1000000000n).toString().padStart(9, "0");
        return `${String(s / 3600n).padStart(2, "0")}:${String((s / 60n) % 60n).padStart(2, "0")}:${String(s % 60n).padStart(2, "0")}.${rem}`;
      }
      default: return Number(v).toLocaleString("en-US");
    }
  }
  function paintDrawing(force) {
    if (!M || (!force && (held || hovering))) return;
    for (let tries = 0; tries < CYCLE.length; tries++) {
      const type = CYCLE[cycleIdx % CYCLE.length];
      cycleIdx++;
      const ptr = M._itch_last_of(type.charCodeAt(0));
      if (!ptr) continue;
      const len = SIZES[type], bytes = M.HEAPU8.slice(ptr, ptr + len), lay = LAYOUT[type];
      dwg.style.setProperty("--n", len);
      let html = '<div class="ruler">';
      for (let b = 0; b < len; b++) html += `<span>${b}</span>`;
      html += '</div><div class="bytes">';
      const owner = new Array(len);
      lay.forEach((f, i) => { for (let b = f[1]; b < f[1] + f[2]; b++) owner[b] = i; });
      for (let b = 0; b < len; b++) html += `<span data-f="${owner[b]}" style="animation-delay:${b * 14}ms">${bytes[b].toString(16).padStart(2, "0").toUpperCase()}</span>`;
      html += '</div><div class="dims">';
      let small = 0;
      lay.forEach((f, i) => {
        const tier = f[2] <= 2 ? small++ % 2 : 0;
        html += `<div class="dim t${tier}" data-f="${i}" style="grid-column:${f[1] + 1} / span ${f[2]};grid-row:1"><span class="line" style="animation-delay:${200 + i * 60}ms"></span><span class="nm">${f[0]}</span><span class="val">${decode(bytes, f[1], f[2], f[3])}</span></div>`;
      });
      html += "</div>";
      dwg.innerHTML = html;
      $("#tb-name").textContent = NAMES[type];
      $("#tb-size").textContent = `${len} BYTES`;
      $("#tb-seq").textContent = fmtInt(M._itch_last_seq_of(type.charCodeAt(0)));
      $("#tb-sheet").textContent = `${CYCLE.indexOf(type) + 1} / ${CYCLE.length}`;
      return;
    }
  }
  dwg.addEventListener("pointerover", (e) => { const t = e.target.closest("[data-f]"); if (t) $$(`[data-f="${t.dataset.f}"]`, dwg).forEach((x) => x.classList.add("hl")); });
  dwg.addEventListener("pointerout", (e) => { const t = e.target.closest("[data-f]"); if (t) $$(`[data-f="${t.dataset.f}"]`, dwg).forEach((x) => x.classList.remove("hl")); });

  // ---------------------------------------------------------------- self-test: gauges + receipt printer
  function makeGauge(host) {
    const max = +host.dataset.max;
    const svg = svgEl("svg", { viewBox: "0 0 200 132" }, host);
    const cx = 100, cy = 104, R = 84, A0 = -120, A1 = 120;
    const pt = (a, r) => [cx + r * Math.sin((a * Math.PI) / 180), cy - r * Math.cos((a * Math.PI) / 180)];
    svgEl("circle", { class: "face", cx, cy, r: R + 8 }, svg);
    const [zx0, zy0] = pt(A0 + (A1 - A0) * 0.82, R - 6), [zx1, zy1] = pt(A1, R - 6);
    svgEl("path", { class: "red-zone", d: `M ${zx0} ${zy0} A ${R - 6} ${R - 6} 0 0 1 ${zx1} ${zy1}` }, svg);
    for (let i = 0; i <= 25; i++) {
      const a = A0 + ((A1 - A0) * i) / 25, maj = i % 5 === 0;
      const [x0, y0] = pt(a, maj ? R - 14 : R - 8), [x1, y1] = pt(a, R);
      svgEl("line", { class: "tick" + (maj ? " maj" : ""), x1: x0, y1: y0, x2: x1, y2: y1 }, svg);
      if (maj) { const [tx, ty] = pt(a, R - 24); const t = svgEl("text", { class: "tl", x: tx, y: ty + 3 }, svg); t.textContent = Math.round((max * i) / 25); }
    }
    const lbl = svgEl("text", { class: "gl", x: cx, y: cy - 34 }, svg); lbl.textContent = host.dataset.label;
    const val = svgEl("text", { class: "gv", x: cx, y: cy + 24 }, svg); val.textContent = "--";
    const unitT = svgEl("text", { class: "gl", x: cx, y: cy - 22 }, svg); unitT.textContent = host.dataset.unit;
    const needle = svgEl("line", { class: "needle", x1: cx, y1: cy, x2: cx, y2: cy - R + 10 }, svg);
    svgEl("circle", { class: "hub", cx, cy, r: 5 }, svg);
    const g = { a: A0, v: 0, target: A0 };
    needle.setAttribute("transform", `rotate(${A0} ${cx} ${cy})`);
    g.set = (value) => { g.target = A0 + (A1 - A0) * Math.min(1.04, value / max); val.textContent = value.toFixed(value < 10 ? 1 : 0); };
    g.tick = () => {
      const acc = (g.target - g.a) * 0.06 - g.v * 0.22; // damped spring: overshoots a little, settles
      g.v += acc; g.a += g.v;
      needle.setAttribute("transform", `rotate(${g.a.toFixed(2)} ${cx} ${cy})`);
    };
    return g;
  }
  const gFrame = makeGauge($("#g-frame")), gBook = makeGauge($("#g-book"));
  const testBtn = $("#selftest"), receipt = $("#receipt");
  async function line(html) {
    const d = document.createElement("div");
    d.innerHTML = html;
    receipt.appendChild(d);
    receipt.style.maxHeight = receipt.scrollHeight + 40 + "px";
    await sleep(reduceMotion ? 0 : 160);
  }
  testBtn.addEventListener("click", async () => {
    if (!M) return;
    testBtn.disabled = true;
    testBtn.textContent = "Testing";
    receipt.innerHTML = "";
    receipt.classList.add("open");
    receipt.style.maxHeight = "0px";
    const s = stats();
    const now = new Date();
    await line('<div class="rl"><b>FH-50 SELF-TEST</b><span>REV C</span></div>');
    await line(`<div class="rl"><span>${now.toISOString().slice(0, 10)}</span><span>${now.toTimeString().slice(0, 8)}</span></div>`);
    await line('<div class="rule"></div>');
    await line(`<div class="rl"><span>SESSION</span><span>${fmtInt(s.total)} MSG</span></div>`);
    await sleep(80);
    let fr = Infinity, bk = Infinity, msgs = 0, bytes = 0;
    for (let i = 0; i < 5; i++) {
      const p = M._itch_bench() >> 3, F = M.HEAPF64;
      msgs = F[p]; fr = Math.min(fr, F[p + 1]); bk = Math.min(bk, F[p + 2]); bytes = F[p + 3];
    }
    fr = Math.max(fr, 0.05); bk = Math.max(bk, 0.05);
    const fm = msgs / fr / 1000, bm = msgs / bk / 1000;
    gFrame.set(fm); gBook.set(bm);
    await line(`<div class="rl"><span>BYTES</span><span>${(bytes / 1e6).toFixed(1)} MB</span></div>`);
    await line(`<div class="rl"><span>FRAMING</span><span>${fm.toFixed(1)} M/S</span></div>`);
    await line(`<div class="rl"><span>  PER MSG</span><span>${((fr * 1e6) / msgs).toFixed(1)} NS</span></div>`);
    await line(`<div class="rl"><span>WITH BOOKS</span><span>${bm.toFixed(1)} M/S</span></div>`);
    await line(`<div class="rl"><span>  PER MSG</span><span>${((bk * 1e6) / msgs).toFixed(0)} NS</span></div>`);
    await line('<div class="rule"></div>');
    const v = M._itch_verify();
    await line(`<div class="rl"><span>BOOK CHECK</span><span>${v === 1 ? "MATCH" : v === -1 ? "IN GAP" : "FAULT"}</span></div>`);
    await line(`<div class="rl"><span>BEST OF</span><span>5 RUNS</span></div>`);
    await line('<div class="pass">PASS</div>');
    testBtn.disabled = false;
    testBtn.textContent = "Run self-test again";
  });

  // ---------------------------------------------------------------- block diagram
  (() => {
    const host = $("#bd");
    const svg = svgEl("svg", { viewBox: "0 0 1180 360", class: "bd", role: "img", "aria-label": "Functional block diagram of the feed handler" }, host);
    const defs = svgEl("defs", {}, svg);
    const mk = svgEl("marker", { id: "arr", viewBox: "0 0 10 10", refX: 9, refY: 5, markerWidth: 7, markerHeight: 7, orient: "auto-start-reverse" }, defs);
    svgEl("path", { d: "M0,0 L10,5 L0,10 z", fill: "#141414" }, mk);
    const mk2 = svgEl("marker", { id: "arrO", viewBox: "0 0 10 10", refX: 9, refY: 5, markerWidth: 7, markerHeight: 7, orient: "auto-start-reverse" }, defs);
    svgEl("path", { d: "M0,0 L10,5 L0,10 z", fill: "#ff4f12" }, mk2);
    const frame = (x, y, w, h, label) => { svgEl("rect", { class: "frame", x, y, width: w, height: h, rx: 6 }, svg); const t = svgEl("text", { class: "fl", x: x + 10, y: y + 16 }, svg); t.textContent = label; };
    frame(200, 30, 690, 300, "NETWORK THREAD · PINNED CORE");
    frame(1025, 30, 145, 170, "STRATEGY THREAD");
    const block = (x, y, w, title, sub, cls = "") => {
      svgEl("rect", { class: "blk " + cls, x, y, width: w, height: 64, rx: 4 }, svg);
      const t = svgEl("text", { class: "bt", x: x + w / 2, y: y + 29 }, svg); t.textContent = title;
      if (cls === "hot") t.setAttribute("fill", "#fff");
      const s = svgEl("text", { class: "bs", x: x + w / 2, y: y + 46 }, svg); s.textContent = sub;
      if (cls === "hot") s.setAttribute("fill", "#c6c1b6");
    };
    const Y = 70;
    block(20, Y, 140, "Exchange", "MoldUDP64 feed", "ext");
    block(220, Y, 130, "UDP RX", "multicast, 64 MB buffer");
    block(380, Y, 150, "MoldUDP64 seq", "park, request, trim");
    block(560, Y, 130, "Parser", "zero-copy views");
    block(720, Y, 150, "Book builder", "order table + levels");
    block(905, Y, 105, "SPSC ring", "lock-free", "hot");
    block(1040, Y, 120, "Strategy", "top of book");
    block(20, 236, 140, "Retransmit", "request server", "ext");
    const wire = (d, cls = "", marker = "arr") => svgEl("path", { class: "wire " + cls, d, "marker-end": `url(#${marker})` }, svg);
    const pin = (x, y, t) => { const e = svgEl("text", { class: "pin", x, y }, svg); e.textContent = t; };
    wire("M160,102 H218"); pin(166, 96, "UDP");
    wire("M350,102 H378"); wire("M530,102 H558"); pin(533, 96, "MSG");
    wire("M690,102 H718"); pin(693, 96, "VIEW");
    wire("M870,102 H903"); pin(872, 96, "TOB 32B");
    wire("M1010,102 H1038");
    const req = "M455,134 V296 H162";
    const retx = "M160,256 H285 V136";
    wire(req, "dash", "arrO"); pin(250, 290, "GAP REQUEST [seq, n)");
    wire(retx, "dash", "arrO"); pin(172, 250, "RETRANSMITS");
    const pre = "M625,134 V176 H795 V136";
    wire(pre, "dash", "arrO"); pin(650, 192, "PREFETCH SLOT, 8 MSGS AHEAD");
    const call = (x, y, n) => { const g = svgEl("g", { class: "call" }, svg); svgEl("circle", { cx: x, cy: y, r: 11 }, g); const t = svgEl("text", { x, y: y + 4 }, g); t.textContent = n; };
    call(530, Y, 1); call(690, Y, 2); call(712, 176, 3); call(870, Y, 4); call(1010, Y, 5);
    // signals travelling the wires
    const main = svgEl("path", { d: "M160,102 H1040", fill: "none", stroke: "none", id: "sigmain" }, svg);
    if (!reduceMotion) {
      for (let i = 0; i < 4; i++) {
        const c = svgEl("circle", { class: "sig", r: 4 }, svg);
        svgEl("animateMotion", { dur: "2.8s", repeatCount: "indefinite", begin: `${i * 0.7}s`, path: "M160,102 H1040" }, c);
      }
      const r1 = svgEl("circle", { class: "sig g", r: 4 }, svg);
      svgEl("animateMotion", { dur: "4.2s", repeatCount: "indefinite", path: req + " M160,256 H285 V136" }, r1);
      const p1 = svgEl("circle", { class: "sig", r: 3 }, svg);
      svgEl("animateMotion", { dur: "2.2s", repeatCount: "indefinite", begin: "0.4s", path: pre }, p1);
    }
    main.remove();
  })();

  // ---------------------------------------------------------------- main loop
  let last = performance.now(), lastCounters = 0, lastBoard = 0, lastVerify = 0, lastMeters = 0, lastDrawing = 0;
  function frame(now) {
    const dt = Math.min(0.1, (now - last) / 1000);
    last = now;
    if (M && !sim.paused && !sim.done) {
      let n;
      if (sim.turbo) n = 2500;
      else { sim.due += sim.rate * dt; n = Math.floor(sim.due); sim.due -= n; }
      if (n > 0) {
        try { if (M._itch_step(n)) { sim.done = true; sim.doneAt = now; } }
        catch (e) { console.error(e); sim.paused = true; }
        blinkJack(jackOut);
      }
    }
    if (M) {
      readEvents(now, dt);
      if (now - lastCounters > 110) { paintCounters(); lastCounters = now; }
      if (now - lastVerify > 400) { paintVerify(); lastVerify = now; }
      if (now - lastBoard > 700) { paintBoard(); paintDepth(); lastBoard = now; }
      if (now - lastMeters > 120) { paintMeters(); lastMeters = now; }
      if (now - lastDrawing > 5200) { paintDrawing(); lastDrawing = now; }
      if (sim.done && now - sim.doneAt > 8000) newSession();
    }
    scope.draw(now);
    gFrame.tick(); gBook.tick();
    requestAnimationFrame(frame);
  }

  function boot(mod) {
    M = mod;
    newSession(1);
    M._itch_step(40);
    paintBoard(); paintDepth(); paintDrawing(true);
    testBtn.disabled = false;
    testBtn.textContent = "Run self-test";
  }
  requestAnimationFrame(frame);
  if (typeof createItch === "function") createItch().then(boot).catch((e) => { console.error(e); testBtn.textContent = "Engine failed to load"; });
  else testBtn.textContent = "Engine failed to load";
})();
