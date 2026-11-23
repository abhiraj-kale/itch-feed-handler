/* Live demo for the ITCH feed handler. The MoldUDP64 session, parser, book
   builder, generator, and simulated network all run in WebAssembly
   (itch.js + itch.wasm); this file drives the simulation and draws. */
(() => {
  "use strict";

  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => Array.from(r.querySelectorAll(s));
  const reduceMotion = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const fmtInt = (n) => Math.round(n).toLocaleString("en-US");
  const fmtPx = (p) => (p / 10000).toFixed(2);
  const C = { accent: "#39d0e6", bid: "#31e0a1", ask: "#ff5d7a", amber: "#ffc163", late: "#b18cff", dup: "#59647a", dim: "#5d6a80", text: "#e8edf6" };

  // ------------------------------------------------------------------ chrome
  const nav = $(".nav");
  addEventListener("scroll", () => nav.classList.toggle("scrolled", scrollY > 30), { passive: true });
  function countUp(el) {
    const target = parseFloat(el.dataset.count), dec = parseInt(el.dataset.decimals || "0", 10), suf = el.dataset.suffix || "";
    const t0 = performance.now(), dur = reduceMotion ? 1 : 1600;
    const tick = (now) => {
      const p = Math.min(1, (now - t0) / dur);
      el.textContent = (target * (1 - Math.pow(1 - p, 4))).toFixed(dec) + suf;
      if (p < 1) requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  }
  const revealObs = new IntersectionObserver((entries) => {
    for (const e of entries) {
      if (!e.isIntersecting) continue;
      e.target.classList.add("in");
      $$("[data-count]", e.target).forEach(countUp);
      $$(".fill[data-w]", e.target).forEach((f) => (f.style.width = Math.min(100, (parseFloat(f.dataset.w) / parseFloat(f.dataset.max)) * 100) + "%"));
      revealObs.unobserve(e.target);
    }
  }, { threshold: 0.15 });
  $$(".reveal").forEach((el) => revealObs.observe(el));
  $$(".card").forEach((c) => c.addEventListener("pointermove", (e) => {
    const r = c.getBoundingClientRect();
    c.style.setProperty("--mx", e.clientX - r.left + "px");
    c.style.setProperty("--my", e.clientY - r.top + "px");
  }));
  function onVisible(el, fn) { new IntersectionObserver((es) => es.forEach((e) => fn(e.isIntersecting)), { threshold: 0.05 }).observe(el); }
  function fitCanvas(cv) {
    const dpr = Math.min(2, devicePixelRatio || 1), w = cv.clientWidth, h = cv.clientHeight;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr); }
    const ctx = cv.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return [ctx, w, h];
  }
  function roundRect(ctx, x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y); ctx.arcTo(x + w, y, x + w, y + h, r); ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r); ctx.arcTo(x, y, x + w, y, r); ctx.closePath();
  }

  // ------------------------------------------------------------------ hero: packet lanes with drops and recovery
  (() => {
    const cv = $("#hero-canvas");
    let visible = true;
    const lanes = [];
    function init() {
      const h = cv.clientHeight, n = Math.max(5, Math.round(h / 70));
      lanes.length = 0;
      for (let i = 0; i < n; i++) lanes.push({ y: ((i + 0.5) / n) * h, v: 60 + Math.random() * 90, pk: [], next: Math.random() * 1.5, falls: [] });
    }
    let last = performance.now();
    function frame(now) {
      const dt = Math.min(0.05, (now - last) / 1000);
      last = now;
      if (visible) {
        const [ctx, w] = fitCanvas(cv);
        ctx.clearRect(0, 0, w, cv.clientHeight);
        for (const L of lanes) {
          ctx.strokeStyle = "rgba(57,208,230,0.06)";
          ctx.lineWidth = 1;
          ctx.beginPath(); ctx.moveTo(0, L.y); ctx.lineTo(w, L.y); ctx.stroke();
          L.next -= dt;
          if (L.next <= 0) { L.pk.push({ x: -30, kind: 0 }); L.next = 0.35 + Math.random() * 0.9; }
          for (let i = L.pk.length - 1; i >= 0; i--) {
            const p = L.pk[i];
            p.x += L.v * dt * (p.kind === 2 ? 1.8 : 1);
            if (p.kind === 0 && !p.checked && p.x > w * (0.2 + Math.random() * 0.5)) {
              p.checked = true;
              if (Math.random() < 0.06) {
                L.pk.splice(i, 1);
                L.falls.push({ x: p.x, y: L.y, vy: 0, a: 1, r: 0 });
                L.pk.push({ x: -30, kind: 2 }); // retransmit follows
                continue;
              }
            }
            if (p.x > w + 40) { L.pk.splice(i, 1); continue; }
            const a = Math.min(1, (p.x + 30) / 120) * Math.min(1, (w + 40 - p.x) / 120);
            ctx.fillStyle = p.kind === 2 ? `rgba(255,193,99,${0.75 * a})` : `rgba(57,208,230,${0.45 * a})`;
            roundRect(ctx, p.x, L.y - 4, 22, 8, 3);
            ctx.fill();
          }
          for (let i = L.falls.length - 1; i >= 0; i--) {
            const f = L.falls[i];
            f.vy += 260 * dt; f.y += f.vy * dt; f.a -= dt * 1.1; f.r += dt * 3;
            if (f.a <= 0) { L.falls.splice(i, 1); continue; }
            ctx.save(); ctx.translate(f.x + 11, f.y); ctx.rotate(f.r);
            ctx.fillStyle = `rgba(255,93,122,${f.a})`;
            roundRect(ctx, -11, -4, 22, 8, 3); ctx.fill();
            ctx.restore();
          }
        }
      }
      requestAnimationFrame(frame);
    }
    addEventListener("resize", init);
    init();
    if (!reduceMotion) requestAnimationFrame(frame);
    onVisible($(".hero"), (v) => (visible = v));
  })();

  // ------------------------------------------------------------------ engine state
  let M = null;
  const SESSION_MSGS = 400000, STOCKS = 200;
  const sim = { rate: 60, turbo: false, paused: false, due: 0, done: false, doneAt: 0, seed: 1 };
  const sizes = { S: 12, R: 39, H: 25, A: 36, F: 40, E: 31, C: 36, X: 23, D: 19, U: 35, P: 44, Q: 40, B: 19, I: 50 };
  const symCache = new Map();
  const sym = (loc) => {
    let s = symCache.get(loc);
    if (!s) { s = M.UTF8ToString(M._itch_symbol(loc)); if (s) symCache.set(loc, s); }
    return s || "#" + loc;
  };

  function newSession() {
    sim.seed = (Math.random() * 1e9) | 0;
    $("#status-text").textContent = "generating session";
    setTimeout(() => {
      const n = M._itch_generate(SESSION_MSGS, STOCKS, sim.seed);
      M._itch_reset(lossVal(), dupVal(), reorderVal(), sim.seed);
      symCache.clear();
      sim.done = false; sim.due = 0;
      sprites.length = 0;
      ranked = [];
      selected = 0;
      $("#sess-size").textContent = fmtInt(n) + " messages, " + STOCKS + " symbols";
      $("#bench-size").textContent = fmtInt(n) + " message session";
      $("#status-text").textContent = sim.paused ? "paused" : "live";
    }, 30);
  }

  // ------------------------------------------------------------------ controls
  const speedIn = $("#speed"), lossIn = $("#loss"), dupIn = $("#dup"), reorderIn = $("#reorder");
  const lossVal = () => (+lossIn.value / 100) * 0.1;      // 0 .. 10%
  const dupVal = () => (+dupIn.value / 100) * 0.1;
  const reorderVal = () => (+reorderIn.value / 100) * 0.1;
  function paintRange(el) { el.style.setProperty("--p", el.value + "%"); }
  function syncControls() {
    sim.rate = Math.round(Math.pow(10, 0.5 + (3.5 * +speedIn.value) / 100));
    $("#speed-out").textContent = sim.turbo ? "turbo" : fmtInt(sim.rate) + " / s";
    $("#loss-out").textContent = (lossVal() * 100).toFixed(1) + "%";
    $("#dup-out").textContent = (dupVal() * 100).toFixed(1) + "%";
    $("#reorder-out").textContent = (reorderVal() * 100).toFixed(1) + "%";
    [speedIn, lossIn, dupIn, reorderIn].forEach(paintRange);
    if (M && M._itch_set_loss) M._itch_set_loss(lossVal(), dupVal(), reorderVal());
  }
  [speedIn, lossIn, dupIn, reorderIn].forEach((el) => el.addEventListener("input", syncControls));
  $("#turbo").addEventListener("click", (e) => { sim.turbo = !sim.turbo; e.target.classList.toggle("active", sim.turbo); syncControls(); });
  $("#pause").addEventListener("click", (e) => {
    sim.paused = !sim.paused;
    e.target.textContent = sim.paused ? "Resume" : "Pause";
    e.target.classList.toggle("active", sim.paused);
    $("#status-dot").classList.toggle("off", sim.paused);
    $("#status-text").textContent = sim.paused ? "paused" : "live";
  });
  $("#restart").addEventListener("click", () => M && newSession());
  syncControls();

  // ------------------------------------------------------------------ wire
  const wire = $("#wire");
  const sprites = [];
  const pulses = [];
  const DUR = { 0: 1.9, 1: 0.9, 2: 1.1, 3: 1.4, 4: 1.9, 5: 2.8 };
  function spawn(kind, seq, count) {
    if (sprites.length > 160) return;
    sprites.push({ kind, seq, count, t: 0, dropAt: 0.35 + Math.random() * 0.3, jitter: (Math.random() - 0.5) * 8 });
  }
  function readEvents() {
    const n = M._itch_event_count();
    if (!n) return;
    const base = M._itch_events() >> 2, U = M.HEAPU32;
    let liveBudget = sim.turbo ? 3 : 24;
    for (let i = 0; i < n; i++) {
      const o = base + i * 4, kind = U[o], seq = U[o + 1], count = U[o + 2];
      if (kind === 0 || kind === 4) { if (liveBudget-- <= 0) continue; }
      spawn(kind, seq, count);
    }
    M._itch_clear_events();
  }
  function drawWire(dt) {
    const [ctx, w, h] = fitCanvas(wire);
    ctx.clearRect(0, 0, w, h);
    const narrow = w < 640;
    const xL = narrow ? 24 : 150, xR = w - (narrow ? 24 : 150);
    const yM = h * 0.5 - 6, yU = yM - 44, yD = yM + 44;
    // lanes
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 6]);
    ctx.strokeStyle = "rgba(255,193,99,0.22)";
    ctx.beginPath(); ctx.moveTo(xL, yU); ctx.lineTo(xR, yU); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(xL, yD); ctx.lineTo(xR, yD); ctx.stroke();
    ctx.setLineDash([]);
    const g = ctx.createLinearGradient(xL, 0, xR, 0);
    g.addColorStop(0, "rgba(57,208,230,0.1)"); g.addColorStop(0.5, "rgba(57,208,230,0.35)"); g.addColorStop(1, "rgba(57,208,230,0.1)");
    ctx.strokeStyle = g; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.moveTo(xL, yM); ctx.lineTo(xR, yM); ctx.stroke();
    ctx.font = "10px JetBrains Mono, monospace";
    ctx.fillStyle = "rgba(255,193,99,0.55)";
    if (!narrow) {
      ctx.textAlign = "center";
      ctx.fillText("gap requests  ←", (xL + xR) / 2, yU - 8);
      ctx.fillText("retransmits  →", (xL + xR) / 2, yD + 18);
    }
    // endpoints
    const node = (x, label, glow) => {
      ctx.fillStyle = "#0e131d"; ctx.strokeStyle = glow > 0 ? `rgba(57,208,230,${0.4 + glow})` : "rgba(57,208,230,0.4)";
      ctx.lineWidth = 1.5;
      roundRect(ctx, x - 9, yM - 30, 18, 60 + 28, 7); ctx.fill(); ctx.stroke();
      if (glow > 0) { ctx.fillStyle = `rgba(57,208,230,${glow * 0.5})`; roundRect(ctx, x - 9, yM - 30, 18, 88, 7); ctx.fill(); }
    };
    let glowR = 0;
    for (let i = pulses.length - 1; i >= 0; i--) { pulses[i] -= dt * 2.5; if (pulses[i] <= 0) pulses.splice(i, 1); else glowR = Math.max(glowR, pulses[i]); }
    node(xL - 14, "exchange", 0);
    node(xR + 14, "handler", glowR);
    // sprites
    ctx.textAlign = "center";
    for (let i = sprites.length - 1; i >= 0; i--) {
      const s = sprites[i];
      s.t += dt / DUR[s.kind];
      if (s.t >= 1) {
        if (s.kind === 0 || s.kind === 3 || s.kind === 5) pulses.push(0.6);
        sprites.splice(i, 1);
        continue;
      }
      const e = s.t;
      let x, y, color, alpha = Math.min(1, e * 8) * Math.min(1, (1 - e) * 8), rot = 0, label = "#" + s.seq, outline = false;
      if (s.kind === 2) { x = xR - (xR - xL) * e; y = yU; color = C.amber; outline = true; label = "REQ " + s.seq + "+" + s.count; }
      else if (s.kind === 3) { x = xL + (xR - xL) * e; y = yD; color = C.amber; }
      else if (s.kind === 1) {
        const cut = s.dropAt;
        if (e < cut) { x = xL + (xR - xL) * (e / cut) * cut * 1.6; y = yM; }
        else {
          const f = (e - cut) / (1 - cut);
          x = xL + (xR - xL) * cut * 1.6 + f * 30;
          y = yM + f * f * 90;
          rot = f * 2.2;
          alpha = 1 - f;
        }
        color = C.ask;
      } else if (s.kind === 4) { x = xL + (xR - xL) * e; y = yM + 9; color = C.dup; }
      else if (s.kind === 5) { x = xL + (xR - xL) * e; y = yM - 9; color = C.late; }
      else { x = xL + (xR - xL) * e; y = yM + s.jitter * 0.3; color = C.accent; }
      const bw = outline ? Math.max(70, label.length * 6.4 + 12) : narrow ? 34 : 52;
      ctx.save();
      ctx.globalAlpha = alpha;
      ctx.translate(x, y);
      ctx.rotate(rot);
      if (outline) {
        ctx.setLineDash([3, 3]); ctx.strokeStyle = color; ctx.lineWidth = 1.2;
        roundRect(ctx, -bw / 2, -9, bw, 18, 5); ctx.stroke(); ctx.setLineDash([]);
        ctx.fillStyle = color;
      } else {
        ctx.shadowColor = color; ctx.shadowBlur = 10;
        ctx.fillStyle = color;
        roundRect(ctx, -bw / 2, -8, bw, 16, 5); ctx.fill();
        ctx.shadowBlur = 0;
        ctx.fillStyle = "#07090e";
      }
      if (!narrow || outline) ctx.fillText(label, 0, 3.5);
      if (s.kind === 1 && e > s.dropAt) { ctx.fillStyle = C.ask; ctx.font = "700 13px Inter, sans-serif"; ctx.fillText("×", 0, -14); ctx.font = "10px JetBrains Mono, monospace"; }
      ctx.restore();
    }
  }

  // ------------------------------------------------------------------ stats, verify, board, depth, mix
  const K = { packets: $("#k-packets"), gaps: $("#k-gaps"), req: $("#k-req"), rec: $("#k-rec"), dup: $("#k-dup"), live: $("#k-live") };
  function paintStats() {
    const p = M._itch_stats() >> 3, F = M.HEAPF64;
    const packets = F[p], msgs = F[p + 1], gaps = F[p + 2], req = F[p + 3], rec = F[p + 4], dup = F[p + 5], expected = F[p + 6], total = F[p + 7], live = F[p + 8], next = F[p + 11];
    K.packets.textContent = fmtInt(packets); K.gaps.textContent = fmtInt(gaps); K.req.textContent = fmtInt(req);
    K.rec.textContent = fmtInt(rec); K.dup.textContent = fmtInt(dup); K.live.textContent = fmtInt(live);
    $("#prog-out").textContent = fmtInt(msgs) + " / " + fmtInt(total);
    $("#prog").style.width = (total ? ((expected - 1) / total) * 100 : 0) + "%";
    const gapEl = $("#prog-gap");
    const parked = Math.max(0, Math.min(next, total + 1) - expected);
    gapEl.style.left = (total ? ((expected - 1) / total) * 100 : 0) + "%";
    gapEl.style.width = (total && parked > 0 ? (parked / total) * 100 : 0) + "%";
    $("#mix-total").textContent = fmtInt(msgs) + " messages";
  }
  function paintVerify() {
    const v = M._itch_verify(), el = $("#verify");
    el.className = "verify " + (v === 1 ? "ok" : v === 0 ? "bad" : "gap");
    const p = M._itch_stats() >> 3, F = M.HEAPF64;
    if (v === 1) { $("#v-title").textContent = sim.done ? "Session complete, book exact" : "Book matches"; $("#v-sub").textContent = "identical to a lossless replay through msg " + fmtInt(F[p + 6] - 1); }
    else if (v === 0) { $("#v-title").textContent = "Mismatch"; $("#v-sub").textContent = "book differs from lossless replay"; }
    else { $("#v-title").textContent = "Recovering a gap"; $("#v-sub").textContent = "packets parked until the retransmit arrives"; }
  }

  let ranked = [], selected = 0, lastRank = 0;
  const board = $("#board");
  const rows = [];
  for (let i = 0; i < 10; i++) {
    const r = document.createElement("div");
    r.className = "brow";
    r.innerHTML = "<span class='sym'>--</span><span>--</span><span class='bp'>--</span><span class='ap'>--</span><span>--</span><span class='n'>--</span><span class='n'>--</span>";
    r.addEventListener("click", () => { if (ranked[i]) { selected = ranked[i]; paintBoard(true); paintDepth(); } });
    board.appendChild(r);
    rows.push({ el: r, c: Array.from(r.children), loc: 0, bid: 0, ask: 0 });
  }
  function flash(el, cls) { el.classList.remove("up", "down"); void el.offsetWidth; el.classList.add(cls); }
  function paintBoard(force) {
    const act = M.HEAPU32, base = M._itch_activity() >> 2;
    const now = performance.now();
    if (force !== true && (now - lastRank > 2500 || ranked.length < 10)) {
      const arr = [];
      for (let loc = 1; loc <= STOCKS; loc++) arr.push([loc, act[base + loc]]);
      arr.sort((a, b) => b[1] - a[1]);
      ranked = arr.slice(0, 10).filter((x) => x[1] > 0).map((x) => x[0]);
      lastRank = now;
      if (!selected && ranked.length) selected = ranked[0];
    }
    rows.forEach((r, i) => {
      const loc = ranked[i];
      if (!loc) return;
      const p = M._itch_top(loc) >> 3, F = M.HEAPF64;
      const bp = F[p], bs = F[p + 1], ap = F[p + 2], as = F[p + 3];
      const same = r.loc === loc;
      r.c[0].textContent = sym(loc);
      r.c[1].textContent = bs ? fmtInt(bs) : "--";
      r.c[2].textContent = bp ? fmtPx(bp) : "--";
      r.c[3].textContent = ap ? fmtPx(ap) : "--";
      r.c[4].textContent = as ? fmtInt(as) : "--";
      r.c[5].textContent = bp && ap ? fmtPx(ap - bp) : "--";
      r.c[6].textContent = fmtInt(act[base + loc]);
      if (same && bp !== r.bid && bp && r.bid) flash(r.c[2], bp > r.bid ? "up" : "down");
      if (same && ap !== r.ask && ap && r.ask) flash(r.c[3], ap > r.ask ? "up" : "down");
      r.loc = loc; r.bid = bp; r.ask = ap;
      r.el.classList.toggle("sel", loc === selected);
    });
  }

  const LV = 8;
  function buildRows(container) {
    const out = [];
    for (let i = 0; i < LV; i++) {
      const r = document.createElement("div");
      r.className = "lrow empty";
      r.innerHTML = '<div class="bar"></div><span class="px">--</span><span class="sz">--</span><span class="cnt">--</span>';
      container.appendChild(r);
      out.push({ el: r, bar: r.firstChild, px: r.children[1], sz: r.children[2], cnt: r.children[3], p: -1, q: -1 });
    }
    return out;
  }
  const askRows = buildRows($("#asks")).reverse(), bidRows = buildRows($("#bids"));
  function readDepth(side) {
    const n = M._itch_depth(selected, side, LV), base = M._itch_depth_ptr(side) >> 2, U = M.HEAPU32, out = [];
    for (let i = 0; i < n; i++) out.push([U[base + i * 3], U[base + i * 3 + 1], U[base + i * 3 + 2]]);
    return out;
  }
  function paintSide(rs, lv, maxQ) {
    for (let i = 0; i < LV; i++) {
      const r = rs[i], l = lv[i];
      if (!l) { if (r.p !== -1) { r.el.className = "lrow empty"; r.px.textContent = r.sz.textContent = r.cnt.textContent = "--"; r.bar.style.width = "0"; r.p = r.q = -1; } continue; }
      const [p, q, c] = l;
      r.el.classList.remove("empty");
      if (p === r.p && q !== r.q) { r.el.classList.remove("flash-up", "flash-down"); void r.el.offsetWidth; r.el.classList.add(q > r.q ? "flash-up" : "flash-down"); }
      r.px.textContent = fmtPx(p); r.sz.textContent = fmtInt(q); r.cnt.textContent = c;
      r.bar.style.width = Math.max(2, (q / maxQ) * 100) + "%";
      r.p = p; r.q = q;
    }
  }
  function paintDepth() {
    if (!selected) return;
    const bids = readDepth(0), asks = readDepth(1);
    let maxQ = 1;
    for (const l of bids) maxQ = Math.max(maxQ, l[1]);
    for (const l of asks) maxQ = Math.max(maxQ, l[1]);
    paintSide(askRows, asks, maxQ);
    paintSide(bidRows, bids, maxQ);
    $("#depth-title").textContent = "Depth · " + sym(selected);
    if (bids.length && asks.length) {
      $("#mid").textContent = ((bids[0][0] + asks[0][0]) / 20000).toFixed(3);
      $("#spread-lbl").textContent = "spread " + fmtPx(asks[0][0] - bids[0][0]);
    }
  }

  const MIX = [["A", "Add order"], ["D", "Delete"], ["U", "Replace"], ["E", "Executed"], ["X", "Cancel"], ["F", "Add (MPID)"], ["C", "Exec at price"], ["P", "Hidden trade"], ["I", "Imbalance"], ["*", "System, directory"]];
  const mixEl = $("#mix");
  const mixRows = MIX.map(([t, name]) => {
    const d = document.createElement("div");
    d.className = "mx";
    d.innerHTML = `<span class="t">${t === "*" ? "S" : t}</span><span class="nm"><i></i><em>${name}</em></span><b>0</b>`;
    mixEl.appendChild(d);
    return { t, bar: d.querySelector("i"), val: d.querySelector("b") };
  });
  function paintMix() {
    const cnt = (c) => M._itch_type_count(c.charCodeAt(0));
    const vals = mixRows.map((r) => (r.t === "*" ? cnt("S") + cnt("R") + cnt("H") : cnt(r.t)));
    const total = vals.reduce((a, b) => a + b, 0) || 1, max = Math.max(...vals, 1);
    mixRows.forEach((r, i) => { r.bar.style.width = (vals[i] / max) * 100 + "%"; r.val.textContent = ((vals[i] / total) * 100).toFixed(1) + "%"; });
  }

  // ------------------------------------------------------------------ inspector
  const FIELD_COLORS = ["#39d0e6", "#8f7bff", "#5d6a80", "#7aa2ff", "#ffc163", "#31e0a1", "#ff9f6e", "#e8edf6", "#ff5d7a", "#b18cff"];
  const LAYOUT = {
    A: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order ref", 11, 8, "u"], ["Side", 19, 1, "side"], ["Shares", 20, 4, "u"], ["Stock", 24, 8, "alpha"], ["Price", 32, 4, "px"]],
    F: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order ref", 11, 8, "u"], ["Side", 19, 1, "side"], ["Shares", 20, 4, "u"], ["Stock", 24, 8, "alpha"], ["Price", 32, 4, "px"], ["Attribution", 36, 4, "alpha"]],
    E: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order ref", 11, 8, "u"], ["Executed shares", 19, 4, "u"], ["Match number", 23, 8, "u"]],
    X: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order ref", 11, 8, "u"], ["Cancelled shares", 19, 4, "u"]],
    D: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Order ref", 11, 8, "u"]],
    U: [["Type", 0, 1, "char"], ["Stock locate", 1, 2, "u"], ["Tracking", 3, 2, "u"], ["Timestamp", 5, 6, "ts"], ["Original ref", 11, 8, "u"], ["New ref", 19, 8, "u"], ["Shares", 27, 4, "u"], ["Price", 31, 4, "px"]],
  };
  const NAMES = { A: "Add Order", F: "Add Order with MPID", E: "Order Executed", X: "Order Cancel", D: "Order Delete", U: "Order Replace" };
  let frozen = false, hovering = false, inspectIdx = 0;
  const INSPECT_CYCLE = ["A", "U", "A", "E", "F", "X", "A", "D"];
  $("#insp-freeze").addEventListener("click", (e) => { frozen = !frozen; e.target.classList.toggle("active", frozen); e.target.textContent = frozen ? "Unfreeze" : "Freeze"; });
  function decode(bytes, off, len, kind) {
    let v = 0n;
    for (let i = 0; i < len; i++) v = (v << 8n) | BigInt(bytes[off + i]);
    const ascii = () => String.fromCharCode(...bytes.slice(off, off + len));
    switch (kind) {
      case "char": return `'${ascii()}'`;
      case "side": return ascii() === "B" ? "'B' buy" : "'S' sell";
      case "alpha": return `"${ascii()}"`;
      case "px": return "$" + (Number(v) / 10000).toFixed(4);
      case "ts": {
        const ns = v, s = ns / 1000000000n, rem = (ns % 1000000000n).toString().padStart(9, "0");
        const hh = String(s / 3600n).padStart(2, "0"), mm = String((s / 60n) % 60n).padStart(2, "0"), ss = String(s % 60n).padStart(2, "0");
        return `${hh}:${mm}:${ss}.${rem}`;
      }
      default: return Number(v).toLocaleString("en-US");
    }
  }
  function paintInspector() {
    if (frozen || hovering) return;
    // Cycle through message types, adds most often since they carry the most fields.
    const want = INSPECT_CYCLE[inspectIdx++ % INSPECT_CYCLE.length];
    let ptr = M._itch_last_of(want.charCodeAt(0)), seq = M._itch_last_seq_of(want.charCodeAt(0));
    if (!ptr) { ptr = M._itch_last_order(); seq = M._itch_last_order_seq(); }
    if (!ptr) return;
    const type = String.fromCharCode(M.HEAPU8[ptr]);
    const lay = LAYOUT[type];
    if (!lay) return;
    const len = sizes[type];
    const bytes = M.HEAPU8.slice(ptr, ptr + len);
    $("#insp-type").textContent = `'${type}' ${NAMES[type]}`;
    $("#insp-seq").textContent = `seq ${fmtInt(seq)} · ${len} bytes · big-endian`;
    const owner = new Array(len).fill(0);
    lay.forEach((f, i) => { for (let b = f[1]; b < f[1] + f[2]; b++) owner[b] = i; });
    const hex = $("#hex");
    hex.innerHTML = "";
    for (let b = 0; b < len; b++) {
      const sp = document.createElement("span");
      const fi = owner[b], col = FIELD_COLORS[fi % FIELD_COLORS.length];
      sp.className = "byte" + (b + 1 < len && owner[b + 1] !== fi ? " gap-after" : "");
      sp.textContent = bytes[b].toString(16).padStart(2, "0").toUpperCase();
      sp.style.color = col;
      sp.style.borderColor = col + "55";
      sp.style.animationDelay = b * 12 + "ms";
      sp.dataset.f = fi;
      hex.appendChild(sp);
    }
    const fields = $("#fields");
    fields.innerHTML = "";
    lay.forEach((f, i) => {
      const col = FIELD_COLORS[i % FIELD_COLORS.length];
      const d = document.createElement("div");
      d.className = "field-chip";
      d.dataset.f = i;
      d.style.borderColor = col + "55";
      d.innerHTML = `<span style="color:${col}">${f[0]}</span><b>${decode(bytes, f[1], f[2], f[3])}</b><small>bytes ${f[1]}${f[2] > 1 ? "-" + (f[1] + f[2] - 1) : ""}</small>`;
      fields.appendChild(d);
    });
  }
  function hl(fi, on) {
    $$(`#hex .byte[data-f="${fi}"], #fields .field-chip[data-f="${fi}"]`).forEach((el) => el.classList.toggle("hl", on));
  }
  const insp = $(".inspector");
  insp.addEventListener("pointerover", (e) => { const t = e.target.closest("[data-f]"); if (t) hl(t.dataset.f, true); });
  insp.addEventListener("pointerout", (e) => { const t = e.target.closest("[data-f]"); if (t) hl(t.dataset.f, false); });
  insp.addEventListener("pointerenter", () => (hovering = true));
  insp.addEventListener("pointerleave", () => (hovering = false));

  // ------------------------------------------------------------------ benchmark
  const runBtn = $("#run-bench");
  runBtn.addEventListener("click", () => {
    if (!M) return;
    runBtn.disabled = true;
    runBtn.classList.add("running");
    runBtn.textContent = "Running...";
    $("#wb-frame").style.width = "0"; $("#wb-book").style.width = "0";
    setTimeout(() => {
      let fr = Infinity, bk = Infinity, msgs = 0, bytes = 0;
      for (let i = 0; i < 5; i++) {
        const p = M._itch_bench() >> 3, F = M.HEAPF64;
        msgs = F[p]; fr = Math.min(fr, F[p + 1]); bk = Math.min(bk, F[p + 2]); bytes = F[p + 3];
      }
      fr = Math.max(fr, 0.05); bk = Math.max(bk, 0.05);
      const fm = msgs / fr / 1000, bm = msgs / bk / 1000;
      requestAnimationFrame(() => { $("#wb-frame").style.width = "100%"; $("#wb-book").style.width = (bm / fm) * 100 + "%"; });
      $("#wv-frame").textContent = fm.toFixed(0);
      $("#wv-book").textContent = bm.toFixed(1);
      $("#verdict").innerHTML = `<b>${((fr * 1e6) / msgs).toFixed(1)} ns</b> per message to frame and dispatch, <b>${((bk * 1e6) / msgs).toFixed(0)} ns</b> with the book rebuild, over ${fmtInt(msgs)} messages (${(bytes / 1e6).toFixed(1)} MB). Best of 5 runs.`;
      runBtn.classList.remove("running");
      runBtn.disabled = false;
      runBtn.textContent = "Run again";
    }, 60);
  });

  // ------------------------------------------------------------------ pipeline animation
  (() => {
    const stages = $$("#pipe .stage"), dot = $("#pipe .pipe-dot");
    let i = 0, timer = 0;
    onVisible($("#pipe"), (v) => {
      clearInterval(timer);
      if (!v) return;
      timer = setInterval(() => {
        stages.forEach((s, k) => s.classList.toggle("lit", k === i));
        const s = stages[i], pr = s.parentElement.getBoundingClientRect(), r = s.getBoundingClientRect();
        dot.style.opacity = 1;
        dot.style.transition = i === 0 ? "none" : "left 0.65s cubic-bezier(0.2,0.7,0.2,1)";
        dot.style.left = r.left - pr.left + r.width / 2 - 5 + "px";
        i = (i + 1) % stages.length;
      }, 750);
    });
  })();

  // ------------------------------------------------------------------ main loop
  let last = performance.now(), lastStats = 0, lastBoard = 0, lastVerify = 0, lastInsp = 0, lastMix = 0;
  function frame(now) {
    const dt = Math.min(0.1, (now - last) / 1000);
    last = now;
    if (M && M._itch_stats && !sim.paused && !sim.done && sprites !== null) {
      let n;
      if (sim.turbo) n = 4000;
      else { sim.due += sim.rate * dt; n = Math.floor(sim.due); sim.due -= n; }
      if (n > 0) {
        try {
          if (M._itch_step(n)) { sim.done = true; sim.doneAt = now; }
        } catch (e) { console.error(e); sim.paused = true; }
      }
    }
    if (M && M._itch_event_count) {
      readEvents();
      if (now - lastStats > 120) { paintStats(); lastStats = now; }
      if (now - lastBoard > 280) { paintBoard(); paintDepth(); lastBoard = now; }
      if (now - lastVerify > 450) { paintVerify(); lastVerify = now; }
      if (now - lastMix > 600) { paintMix(); lastMix = now; }
      if (now - lastInsp > 3200) { paintInspector(); lastInsp = now; }
      if (sim.done && now - sim.doneAt > 7000) newSession();
    }
    drawWire(dt);
    requestAnimationFrame(frame);
  }

  function boot(mod) {
    M = mod;
    const n = M._itch_generate(SESSION_MSGS, STOCKS, 1);
    M._itch_reset(lossVal(), dupVal(), reorderVal(), 1);
    $("#sess-size").textContent = fmtInt(n) + " messages, " + STOCKS + " symbols";
    $("#bench-size").textContent = fmtInt(n) + " message session";
    $("#status-text").textContent = "live";
    runBtn.disabled = false;
    runBtn.textContent = "Run benchmark";
    // Warm up so the board and inspector have content immediately.
    M._itch_step(30);
    requestAnimationFrame(frame);
  }
  if (typeof createItch === "function") {
    createItch().then(boot).catch((e) => { console.error(e); $("#status-text").textContent = "engine failed to load"; $("#status-dot").classList.add("off"); });
  } else {
    $("#status-text").textContent = "engine failed to load";
  }
})();
