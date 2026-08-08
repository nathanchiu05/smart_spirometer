// ================================================================
// webpage.h — Smart Spirometer app UI  (v6)
//
// v6 — one breath per trip through the flow. A completed breath goes
// straight to its own overview and check-in, then home, where the
// daily ring shows the updated count (e.g. 1/10). The user starts each
// subsequent breath manually from the home card. Progress is therefore
// per-day (the firmware's bToday) rather than per-visit, and a history
// record is one breath rather than a run of them.
//
// v5 — manual-start state machine: goal setup never starts a session,
// and after every breath (Inhale -> Hold -> Exhale) the app returns
// to the Prepare screen and waits for the next explicit "Start
// breath" tap. Auto-looping into the next rep and auto-entering the
// flow on sensor movement were removed — the firmware now samples
// the ToF sensor only while a session is active.
//
// v4 adds the 6-stage breathing exercise flow (replaces the old
// Session view). Entered from the Next-session card or auto-entered
// when a breath starts:
//
//   Stage 1  Prepare        — checklist + "Start breathing"
//   Stage 2  Inhale Coaching— live circle driven by sensor volume,
//                             flow states (Good/Too fast/Too slow),
//                             screen pulses + haptic stubs
//   Stage 3  Hold           — full green circle, 5s countdown
//                             (mirrors firmware HOLD_SECONDS)
//   Stage 4  Reset & Exhale — shrink animation, then on to Stage 5
//   Stage 5  Breath overview— daily-progress ring, stars, metric bars
//                             for that one breath (computed client-side
//                             from live data) -> POST /session
//   Stage 6  Check-in       — survey -> POST /checkin, then home
//
// UI-first notes (firmware catch-up hooks):
//   - Stage 4 advance is timer-based; swap to a real "piston returned
//     to rest" signal when the firmware exposes one.
//   - Per-rep stats (duration, peak flow, volume, consistency) are
//     computed in JS from the 10 Hz WebSocket stream.
//   - haptic() wraps navigator.vibrate (Android Chrome; iOS ignores)
//     — single hook point for a native bridge later.
//
// IMPORTANT: AP mode has NO internet — fully self-contained, all
// icons/illustrations are inline SVG.
// ================================================================

#ifndef WEBPAGE_H
#define WEBPAGE_H

#include <pgmspace.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#f2f5f9">
<title>Smart Spirometer</title>
<style>
  :root {
    --bg: #f2f5f9;
    --card: #ffffff;
    --navy: #16244c;
    --muted: #8a94a6;
    --blue: #2f6bed;
    --blue-soft: #e8efff;
    --teal: #2dd4bf;
    --green: #34b678;
    --green-soft: #e5f6ee;
    --red: #e5484d;
    --red-soft: #fdecec;
    --amber: #e8973a;
    --amber-soft: #fdf2e3;
    --warn: #d29922;
    --shadow: 0 6px 18px rgba(22, 36, 76, 0.08);
    --radius: 16px;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
  html { height: 100%; }
  body {
    /* min-height, not height: a fixed 100% leaves the bottom padding inside
       the viewport box, so long views scroll their last control under the
       fixed bottom nav instead of clearing it. */
    min-height: 100%;
    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background: var(--bg);
    color: var(--navy);
    padding: 12px 18px 92px;
    max-width: 430px;
    margin: 0 auto;
  }

  /* ---------- Top header ---------- */
  .top-bar { display: flex; justify-content: space-between; align-items: center; padding: 6px 0 14px; position: relative; }
  .icon-btn { background: none; border: none; color: var(--navy); padding: 6px; cursor: pointer; display: flex; }
  .conn-dot {
    position: absolute; left: 50%; transform: translateX(-50%);
    font-size: 0.7em; color: var(--muted);
    display: flex; align-items: center; gap: 5px; white-space: nowrap;
  }
  .conn-dot::before { content: ""; width: 7px; height: 7px; border-radius: 50%; background: var(--warn); }
  .conn-dot.ok::before { background: var(--green); }

  .view { display: none; }
  .view.active { display: block; }

  /* ---------- Home (unchanged from v3) ---------- */
  .greeting h2 { font-size: 1.75em; font-weight: 800; line-height: 1.2; }
  .greeting p { color: var(--muted); margin-top: 6px; font-size: 0.95em; }
  .card {
    background: var(--card); border-radius: var(--radius);
    box-shadow: var(--shadow); padding: 18px 20px; margin-top: 16px;
  }
  .card h3 { font-size: 0.85em; font-weight: 700; margin-bottom: 6px; }
  .card .sub { color: var(--muted); font-size: 0.85em; }
  .goal-card { display: flex; justify-content: space-between; align-items: center; }
  .goal-score { font-size: 2.4em; font-weight: 800; font-variant-numeric: tabular-nums; line-height: 1.1; }
  .goal-score .num { color: var(--blue); }
  .goal-score .of  { color: var(--muted); font-weight: 400; font-size: 0.75em; }
  .ring-wrap { position: relative; width: 96px; height: 96px; flex: none; }
  .ring-wrap svg { transform: rotate(-90deg); }
  .ring-track { stroke: #e7edf5; }
  .ring-fill { stroke: url(#ringGrad); stroke-linecap: round; transition: stroke-dashoffset 0.4s ease; }
  .ring-lungs { position: absolute; inset: 0; display: flex; align-items: center; justify-content: center; color: var(--blue); }
  .session-card { display: flex; align-items: center; gap: 14px; cursor: pointer; }
  .session-card .clock {
    width: 44px; height: 44px; border-radius: 12px;
    background: var(--blue-soft); color: var(--blue);
    display: flex; align-items: center; justify-content: center; flex: none;
  }
  .session-card .grow { flex: 1; }
  .session-card .value { font-size: 1.15em; font-weight: 700; margin-top: 2px; }
  .session-card .chev { color: var(--muted); flex: none; }
  .recovery-card { display: flex; justify-content: space-between; align-items: center; overflow: hidden; }
  .recovery-card .value { font-size: 1.15em; font-weight: 700; margin-top: 2px; }
  .recovery-card .sprout { flex: none; margin: -6px -8px -18px 0; }

  /* ---------- Exercise flow shell ---------- */
  .stage { display: none; animation: stageIn 0.35s ease; }
  .stage.active { display: block; }
  @keyframes stageIn {
    from { opacity: 0; transform: translateY(8px); }
    to   { opacity: 1; transform: none; }
  }
  .flow-head { display: flex; align-items: center; min-height: 34px; }
  .flow-back { background: none; border: none; color: var(--navy); cursor: pointer; padding: 4px 8px 4px 0; display: flex; }
  .stage-title { text-align: center; font-size: 1.45em; font-weight: 800; line-height: 1.25; margin-top: 4px; }
  .stage-sub { text-align: center; color: var(--muted); margin-top: 8px; font-size: 0.95em; }

  /* Pulse overlay (Too fast / Too slow feedback) */
  #pulse {
    position: fixed; inset: 0; pointer-events: none;
    opacity: 0; z-index: 50;
  }
  #pulse.red   { background: rgba(229, 72, 77, 0.18); }
  #pulse.amber { background: rgba(232, 151, 58, 0.18); }
  #pulse.go { animation: pulseAnim 0.14s ease; }
  @keyframes pulseAnim { 0% {opacity: 0;} 40% {opacity: 1;} 100% {opacity: 0;} }

  /* ---------- Stage 1: Prepare ---------- */
  .device-hero {
    width: 140px; height: 140px; border-radius: 50%;
    background: radial-gradient(circle at 35% 30%, #eaf2ff, #d7e6fb);
    margin: 14px auto 8px;
    display: flex; align-items: center; justify-content: center;
    box-shadow: var(--shadow);
  }
  .check-list { margin-top: 18px; }
  .check-item {
    background: var(--card); border-radius: 14px; box-shadow: var(--shadow);
    display: flex; align-items: center; gap: 12px;
    padding: 13px 16px; margin-top: 10px; font-size: 0.95em; font-weight: 600;
  }
  .check-item .ci { color: var(--blue); display: flex; flex: none; }
  .check-item .grow { flex: 1; }
  .check-item .ok { color: var(--green); display: flex; flex: none; }

  /* ---------- Stages 2-4: breathing circle ---------- */
  .breath-wrap {
    position: relative;
    width: 272px; height: 272px;
    margin: 22px auto 6px;
  }
  .breath-track {
    position: absolute; inset: 0; border-radius: 50%;
    border: 3px solid #dbe6f5;
    transition: border-color 0.3s, box-shadow 0.3s;
  }
  .breath-wrap.good  .breath-track { border-color: #9fdcbb; box-shadow: 0 0 24px rgba(52, 182, 120, 0.35); }
  .breath-wrap.fast  .breath-track { border-color: var(--red); box-shadow: 0 0 24px rgba(229, 72, 77, 0.35); }
  .breath-wrap.slow  .breath-track { border-color: var(--amber); box-shadow: 0 0 24px rgba(232, 151, 58, 0.3); }
  .breath-wrap.exhale .breath-track { border: 3px dashed #a9c4ec; box-shadow: none; }
  .breath-circle {
    position: absolute; inset: 12px; border-radius: 50%;
    transform: scale(0.25);
    transition: transform 0.13s linear, background 0.3s;
    background: radial-gradient(circle at 32% 28%, #8fd8b4, var(--green));
  }
  .breath-wrap.fast .breath-circle { background: radial-gradient(circle at 32% 28%, #f29a9d, var(--red)); }
  .breath-wrap.slow .breath-circle { background: radial-gradient(circle at 32% 28%, #f6c98d, var(--amber)); }
  .breath-wrap.exhale .breath-circle {
    background: radial-gradient(circle at 32% 28%, #9cc4f5, var(--blue));
    transition: transform 2.4s ease;
  }
  .breath-wrap.holdfull .breath-circle { transform: scale(1) !important; }
  .exhale-chevs span {
    position: absolute; color: #a9c4ec; font-size: 18px; font-weight: 700;
  }
  .exhale-chevs .n { top: 8px; left: 50%; transform: translateX(-50%); }
  .exhale-chevs .s { bottom: 8px; left: 50%; transform: translateX(-50%) rotate(180deg); }
  .exhale-chevs .e { right: 10px; top: 50%; transform: translateY(-50%) rotate(90deg); }
  .exhale-chevs .w { left: 10px; top: 50%; transform: translateY(-50%) rotate(-90deg); }

  .coach-msg { text-align: center; margin-top: 14px; min-height: 74px; }
  .coach-msg .main { font-size: 1.2em; font-weight: 800; }
  .coach-msg .line2 { color: var(--muted); font-size: 0.92em; margin-top: 4px; }
  .coach-msg.good .main { color: var(--green); }
  .coach-msg.fast .main { color: var(--red); }
  .coach-msg.slow .main { color: var(--amber); }
  .flow-chip {
    display: inline-flex; align-items: center; gap: 6px;
    margin-top: 8px; padding: 5px 12px; border-radius: 999px;
    font-size: 0.75em; font-weight: 800; letter-spacing: 0.06em;
  }
  .flow-chip.good { background: var(--green-soft); color: var(--green); }
  .flow-chip.fast { background: var(--red-soft); color: var(--red); }
  .flow-chip.slow { background: var(--amber-soft); color: var(--amber); }

  /* Hold stage */
  .hold-word { text-align: center; font-size: 1.7em; font-weight: 800; margin-top: 6px; }
  .hold-count {
    width: 84px; height: 84px; border-radius: 50%;
    background: var(--green-soft); color: var(--green);
    font-size: 2.2em; font-weight: 800;
    display: flex; align-items: center; justify-content: center;
    margin: 14px auto 0;
  }
  .hold-note { text-align: center; color: var(--green); font-size: 0.85em; font-weight: 700; margin-top: 12px; }

  /* Bottom status pill (stages 2-4) */
  .status-pill {
    background: var(--card); border-radius: var(--radius); box-shadow: var(--shadow);
    display: flex; align-items: stretch; margin-top: 18px; padding: 12px 6px;
  }
  .status-pill .cell {
    flex: 1; display: flex; align-items: center; justify-content: center; gap: 10px;
  }
  .status-pill .cell + .cell { border-left: 1px solid #e7edf5; }
  .status-pill .lab { font-size: 0.68em; font-weight: 700; letter-spacing: 0.08em; color: var(--muted); text-transform: uppercase; }
  .status-pill .val { font-size: 0.98em; font-weight: 800; }
  .status-pill .icn { display: flex; color: var(--blue); }
  .status-pill.good .icn, .status-pill.good .val { color: var(--green); }
  .status-pill.fast .icn, .status-pill.fast .val { color: var(--red); }
  .status-pill.slow .icn, .status-pill.slow .val { color: var(--amber); }

  .end-link {
    display: block; text-align: center; margin-top: 14px;
    color: var(--muted); font-size: 0.85em; background: none; border: none;
    cursor: pointer; width: 100%; text-decoration: underline;
  }

  /* ---------- Stage 5: Summary ---------- */
  .sum-ring { position: relative; width: 190px; height: 190px; margin: 22px auto 8px; }
  .sum-ring svg { transform: rotate(-90deg); }
  .sum-ring .inner {
    position: absolute; inset: 0;
    display: flex; flex-direction: column; align-items: center; justify-content: center;
  }
  .sum-ring .big { font-size: 1.9em; font-weight: 800; font-variant-numeric: tabular-nums; }
  .sum-ring .lab { color: var(--muted); font-size: 0.85em; }
  .stars { display: flex; justify-content: center; gap: 8px; margin-top: 14px; }
  .stars svg { width: 30px; height: 30px; }
  .stars .off { opacity: 0.22; }
  .stars-label { text-align: center; color: var(--muted); font-size: 0.9em; margin-top: 8px; }
  .metric-row {
    display: flex; align-items: center; gap: 12px;
    padding: 11px 0; border-top: 1px solid #eef2f8;
    font-size: 0.9em;
  }
  .metric-row:first-child { border-top: none; }
  .metric-row .name { width: 108px; color: var(--navy); font-weight: 600; flex: none; }
  .metric-row .segs { flex: 1; display: flex; gap: 4px; }
  .metric-row .seg { height: 12px; flex: 1; border-radius: 3px; background: #e7edf5; }
  .metric-row .seg.on-g { background: var(--green); }
  .metric-row .seg.on-b { background: var(--blue); }
  .metric-row .seg.on-t { background: var(--teal); }
  .metric-row .num { width: 64px; text-align: right; font-weight: 700; font-variant-numeric: tabular-nums; flex: none; }

  /* ---------- Stage 6: Check-in ---------- */
  .ci-card {
    background: var(--card); border-radius: 14px; box-shadow: var(--shadow);
    padding: 15px 16px; margin-top: 12px;
  }
  .ci-head { display: flex; align-items: center; gap: 12px; }
  .ci-icon {
    width: 40px; height: 40px; border-radius: 50%; flex: none;
    background: var(--blue-soft); color: var(--blue);
    display: flex; align-items: center; justify-content: center;
  }
  .ci-head .t { font-weight: 700; font-size: 0.95em; }
  .ci-head .s { color: var(--muted); font-size: 0.8em; margin-top: 2px; }
  .ticks { display: flex; justify-content: space-between; font-size: 0.65em; color: var(--muted); margin: 10px 2px 2px; }
  input[type=range] {
    width: 100%; accent-color: var(--blue); height: 26px;
  }
  .range-ends { display: flex; justify-content: space-between; font-size: 0.7em; color: var(--muted); margin-top: 2px; }
  .seg-row { display: flex; gap: 8px; flex-wrap: wrap; margin-top: 12px; }
  .seg-btn {
    /* basis auto (not 0) so a long label wraps the row instead of
       overflowing a button squeezed to an equal share of the width */
    flex: 1 1 auto; min-width: 64px;
    border: 1px solid #dde4ee; background: #fbfcfe; color: var(--navy);
    border-radius: 999px; padding: 8px 10px; font-size: 0.82em; font-weight: 600;
    cursor: pointer; white-space: nowrap;
  }
  /* Paired half-width groups (Thickness / Amount) need to fit 3 chips */
  .seg-row.compact { gap: 6px; }
  .seg-row.compact .seg-btn { min-width: 0; padding: 8px 4px; font-size: 0.75em; }
  .seg-btn.sel { background: var(--blue-soft); border-color: var(--blue); color: var(--blue); }
  .ci-q { font-weight: 700; font-size: 0.88em; margin-top: 14px; }
  .hidden { display: none !important; }
  .dots-row { display: flex; justify-content: space-between; margin-top: 10px; }
  .dot-opt { display: flex; flex-direction: column; align-items: center; gap: 5px; cursor: pointer; width: 15%; }
  .dot-opt .dot { width: 26px; height: 26px; border-radius: 50%; border: 2px solid #dde4ee; }
  .dot-opt.sel .dot { border-color: var(--blue); box-shadow: 0 0 0 3px var(--blue-soft); }
  .dot-opt .dl { font-size: 0.6em; color: var(--muted); text-align: center; line-height: 1.2; }
  textarea {
    width: 100%; min-height: 74px; margin-top: 10px;
    border: 1px solid #dde4ee; border-radius: 12px; background: #fbfcfe;
    padding: 10px 12px; font-family: inherit; font-size: 0.9em; color: var(--navy);
    resize: vertical;
  }
  textarea:focus { outline: 2px solid var(--blue-soft); border-color: var(--blue); }
  .char-count { text-align: right; color: var(--muted); font-size: 0.7em; margin-top: 4px; }
  .notice {
    display: flex; gap: 10px; align-items: flex-start;
    background: var(--blue-soft); color: #3a5db0;
    border-radius: 12px; padding: 12px 14px; font-size: 0.82em; margin-top: 14px;
  }

  /* ---------- Shared controls ---------- */
  .btn {
    display: block; width: 100%;
    font-size: 1.05em; font-weight: 700; padding: 13px;
    border-radius: 12px; border: none;
    background: var(--blue); color: #fff; cursor: pointer; margin-top: 16px;
  }
  .btn:active { opacity: 0.85; }
  .btn.ghost { background: var(--blue-soft); color: var(--blue); }
  .field-label { display: block; color: var(--muted); font-size: 0.85em; margin: 14px 0 8px; }
  input[type=number], input[type=text] {
    width: 100%; font-size: 1.1em; padding: 12px;
    border-radius: 12px; border: 1px solid #dde4ee; background: #fbfcfe; color: var(--navy);
  }
  input:focus { outline: 2px solid var(--blue-soft); border-color: var(--blue); }
  input::-webkit-outer-spin-button, input::-webkit-inner-spin-button { -webkit-appearance: none; }
  input[type=number] { -moz-appearance: textfield; }
  .row { display: flex; gap: 10px; }
  .row > * { flex: 1; }
  .row .btn { margin-top: 0; flex: none; width: auto; padding: 12px 18px; }
  .form-msg { margin-top: 10px; font-size: 0.85em; color: var(--muted); min-height: 1.2em; }
  .check-row { display: flex; align-items: center; gap: 10px; margin-top: 14px; font-size: 0.9em; }
  .check-row input { width: 18px; height: 18px; accent-color: var(--blue); }
  .placeholder { text-align: center; padding: 48px 20px; color: var(--muted); }
  .placeholder .big { font-size: 2em; margin-bottom: 10px; }

  /* ---------- History ---------- */
  .hist-title { font-size: 1.45em; font-weight: 800; margin: 4px 0 2px; }
  .hist-sub { color: var(--muted); font-size: 0.9em; }
  .hist-top { display: flex; justify-content: space-between; align-items: center; gap: 10px; cursor: pointer; }
  .hist-date { font-weight: 700; font-size: 0.95em; }
  .hist-meta { color: var(--muted); font-size: 0.82em; margin-top: 3px; }
  .chip {
    flex: none; padding: 5px 12px; border-radius: 999px;
    font-size: 0.72em; font-weight: 800; letter-spacing: 0.04em;
  }
  .chip.perfect { background: var(--green-soft); color: var(--green); }
  .chip.fast    { background: var(--red-soft);   color: var(--red); }
  .chip.slow    { background: var(--amber-soft); color: var(--amber); }
  .chip.none    { background: #eef2f8; color: var(--muted); }
  .hist-detail { border-top: 1px solid #eef2f8; margin-top: 12px; padding-top: 12px; }
  .hist-stats { display: flex; flex-wrap: wrap; gap: 6px 16px; font-size: 0.82em; color: var(--muted); }
  .hist-stats b { color: var(--navy); font-weight: 700; }
  .hist-ci-title { font-weight: 700; font-size: 0.82em; margin: 12px 0 6px; }
  .hist-ci { display: flex; flex-wrap: wrap; gap: 6px 16px; font-size: 0.82em; color: var(--muted); }
  .hist-ci b { color: var(--navy); font-weight: 600; }
  .hist-notes {
    margin-top: 8px; font-size: 0.82em; color: var(--navy);
    background: #f6f8fc; border-radius: 10px; padding: 9px 12px; font-style: italic;
  }
  .hist-none { color: var(--muted); font-size: 0.82em; font-style: italic; }

  /* ---------- Trends ---------- */
  .trend-seg { display: flex; gap: 8px; margin-top: 14px; }
  .trend-seg .seg-btn { flex: 1; }
  .adh-card { display: flex; align-items: center; gap: 14px; }
  .adh-left { flex: none; }
  .adh-pct { font-size: 2.3em; font-weight: 800; line-height: 1.1; }
  .adh-sub { color: var(--muted); font-size: 0.8em; margin-top: 3px; }
  .adh-chart { flex: 1; min-width: 0; }
  .trend-rows { padding: 4px 18px; }
  .trend-row {
    display: flex; justify-content: space-between; align-items: center;
    padding: 13px 0; border-top: 1px solid #eef2f8;
    font-size: 0.92em;
  }
  .trend-row:first-child { border-top: none; }
  .trend-row .tl { color: #5b6784; }
  .trend-row .tv { font-weight: 800; font-variant-numeric: tabular-nums; }
  .trend-note {
    background: var(--blue-soft); color: #3a5db0;
    border-radius: 12px; padding: 13px 15px; font-size: 0.86em; margin-top: 14px;
    line-height: 1.45;
  }

  /* ---------- Bottom nav ---------- */
  .bottom-nav {
    position: fixed; left: 0; right: 0; bottom: 0;
    background: var(--card); border-top: 1px solid #e7edf5;
    display: flex; justify-content: space-around;
    padding: 8px 0 calc(10px + env(safe-area-inset-bottom));
    max-width: 430px; margin: 0 auto;
  }
  .nav-item {
    background: none; border: none; color: var(--muted);
    font-size: 0.7em; font-weight: 600;
    display: flex; flex-direction: column; align-items: center; gap: 3px;
    cursor: pointer; width: 64px;
  }
  .nav-item.active { color: var(--blue); }
</style>
</head>
<body>

  <div id="pulse"></div>

  // <!-- Top header -->
  // <div class="top-bar">
  //   <button class="icon-btn" aria-label="Menu">
  //     <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><line x1="3" y1="6" x2="21" y2="6"/><line x1="3" y1="12" x2="21" y2="12"/><line x1="3" y1="18" x2="21" y2="18"/></svg>
  //   </button>
  //   <div class="conn-dot" id="conn">Connecting</div>
  //   <button class="icon-btn" aria-label="Notifications">
  //     <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M18 8a6 6 0 0 0-12 0c0 7-3 9-3 9h18s-3-2-3-9"/><path d="M13.7 21a2 2 0 0 1-3.4 0"/></svg>
  //   </button>
  // </div>

  <!-- ================= HOME ================= -->
  <div class="view active" id="view-home">
    <div class="greeting">
      <h2><span id="greetWord">Good morning</span>,<br><span id="greetName">Friend</span> &#128075;</h2>
      <p>Every breath counts toward your recovery.</p>
    </div>

    <div class="card goal-card">
      <div>
        <h3>Today's goal</h3>
        <div class="goal-score"><span class="num" id="bToday">0</span> <span class="of">/ <span id="bGoal">10</span></span></div>
        <div class="sub">breaths</div>
      </div>
      <div class="ring-wrap">
        <svg width="96" height="96" viewBox="0 0 96 96">
          <defs>
            <linearGradient id="ringGrad" x1="0%" y1="0%" x2="100%" y2="100%">
              <stop offset="0%" stop-color="#2dd4bf"/>
              <stop offset="100%" stop-color="#2f6bed"/>
            </linearGradient>
          </defs>
          <circle class="ring-track" cx="48" cy="48" r="40" fill="none" stroke-width="9"/>
          <circle class="ring-fill" id="ringFill" cx="48" cy="48" r="40" fill="none" stroke-width="9"
                  stroke-dasharray="251.3" stroke-dashoffset="251.3"/>
        </svg>
        <div class="ring-lungs">
          <svg width="34" height="34" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round">
            <path d="M12 3v7"/>
            <path d="M12 10c0 3-1.5 4.5-3.5 6-1.6 1.2-4 1.3-4.9-.2C2.6 14 3 10.5 4.5 8.5 5.7 6.9 8 6.6 9 8.2"/>
            <path d="M12 10c0 3 1.5 4.5 3.5 6 1.6 1.2 4 1.3 4.9-.2 1-1.8.6-5.3-.9-7.3-1.2-1.6-3.5-1.9-4.5-.3"/>
          </svg>
        </div>
      </div>
    </div>

    <div class="card session-card" onclick="enterFlow()">
      <div class="clock">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="9"/><polyline points="12 7 12 12 15.5 14"/></svg>
      </div>
      <div class="grow">
        <h3>Start next breath</h3>
        <div class="value" id="nextSession">&mdash;</div>
      </div>
      <div class="chev">
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 6 15 12 9 18"/></svg>
      </div>
    </div>

    <div class="card recovery-card">
      <div>
        <h3>Recovery progress</h3>
        <div class="value" id="recoveryDay">Day &mdash;</div>
      </div>
      <div class="sprout">
        <svg width="110" height="84" viewBox="0 0 110 84">
          <path d="M0 84 Q 55 56 110 84 Z" fill="#bfe6c3"/>
          <path d="M55 74 V 52" stroke="#3fae5a" stroke-width="3.5" stroke-linecap="round" fill="none"/>
          <path d="M55 60 C 45 58 40 50 40 42 C 50 42 55 50 55 58 Z" fill="#3fae5a"/>
          <path d="M55 54 C 65 52 70 44 70 36 C 60 36 55 44 55 52 Z" fill="#56c274"/>
        </svg>
      </div>
    </div>
  </div>

  <!-- ================= EXERCISE FLOW ================= -->
  <div class="view" id="view-session">

    <div class="flow-head">
      <button class="flow-back" onclick="exitFlow()" aria-label="Back">
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><line x1="19" y1="12" x2="5" y2="12"/><polyline points="12 19 5 12 12 5"/></svg>
      </button>
    </div>

    <!-- ---- Stage 1: Prepare ---- -->
    <div class="stage" id="stage-prepare">
      <div class="stage-title">Let's get ready</div>
      <div class="stage-sub">Sit up tall and place the<br>mouthpiece in your mouth.</div>
      <div class="stage-sub" id="prepProgress" style="font-weight:700;color:var(--blue);"></div>

      <div class="device-hero">
        <svg width="100" height="100" viewBox="0 0 24 24" fill="none" stroke="#2f6bed" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
          <!-- Clipboard Outline -->
          <path d="M16 4h2a2 2 0 0 1 2 2v14a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2h2"/>
          <!-- Top Clip -->
          <rect x="8" y="2" width="8" height="4" rx="1" ry="1" fill="#e8efff"/>
          <!-- Checklist Lines & Checkmarks -->
          <path d="m9 10 1 1 2-2" stroke="#34b678" stroke-width="2"/>
          <line x1="14" y1="10" x2="17" y2="10"/>
          <path d="m9 14 1 1 2-2" stroke="#34b678" stroke-width="2"/>
          <line x1="14" y1="14" x2="17" y2="14"/>
          <line x1="9" y1="18" x2="17" y2="18"/>
        </svg>
      </div>

      <div class="check-list">
        <div class="check-item">
          <span class="ci"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="6" r="3"/><path d="M12 9v6m0 0-3 6m3-6 3 6M8 12h8"/></svg></span>
          <span class="grow">Sit upright</span>
          <span class="ok"><svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="10" opacity="0.15"/><path d="M8 12.5l2.5 2.5L16 9.5" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></svg></span>
        </div>
        <div class="check-item">
          <span class="ci"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="8"/><path d="M9 13c1 1.4 5 1.4 6 0"/></svg></span>
          <span class="grow">Seal lips around mouthpiece</span>
          <span class="ok"><svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="10" opacity="0.15"/><path d="M8 12.5l2.5 2.5L16 9.5" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></svg></span>
        </div>
        <div class="check-item">
          <span class="ci"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v7"/><path d="M12 10c0 3-1.5 4.5-3.5 6-1.6 1.2-4 1.3-4.9-.2C2.6 14 3 10.5 4.5 8.5 5.7 6.9 8 6.6 9 8.2"/><path d="M12 10c0 3 1.5 4.5 3.5 6 1.6 1.2 4 1.3 4.9-.2 1-1.8.6-5.3-.9-7.3-1.2-1.6-3.5-1.9-4.5-.3"/></svg></span>
          <span class="grow">Take a normal breath in</span>
          <span class="ok"><svg width="20" height="20" viewBox="0 0 24 24" fill="currentColor"><circle cx="12" cy="12" r="10" opacity="0.15"/><path d="M8 12.5l2.5 2.5L16 9.5" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"/></svg></span>
        </div>
      </div>

      <button class="btn" onclick="startBreathing()">Start breath</button>
      <button class="end-link" onclick="endEarly()">End breath</button>
    </div>

    <!-- ---- Stage 2: Inhale coaching ---- -->
    <div class="stage" id="stage-inhale">
      <div class="stage-title" style="font-size:1.2em;">Take a slow, deep breath</div>

      <div class="breath-wrap good" id="inhaleWrap">
        <div class="breath-track"></div>
        <div class="breath-circle" id="inhaleCircle"></div>
      </div>

      <div class="coach-msg good" id="coachMsg">
        <div class="main" id="coachMain">Inhale to begin</div>
        <div class="line2" id="coachLine2">Fill the circle by inhaling slowly and steadily.</div>
        <div class="flow-chip good" id="coachChip" style="visibility:hidden;">&#8776; GOOD</div>
      </div>

      <div class="status-pill good" id="inhalePill">
        <div class="cell">
          <span class="icn"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v7"/><path d="M12 10c0 3-1.5 4.5-3.5 6-1.6 1.2-4 1.3-4.9-.2C2.6 14 3 10.5 4.5 8.5 5.7 6.9 8 6.6 9 8.2"/><path d="M12 10c0 3 1.5 4.5 3.5 6 1.6 1.2 4 1.3 4.9-.2 1-1.8.6-5.3-.9-7.3-1.2-1.6-3.5-1.9-4.5-.3"/></svg></span>
          <span><span class="lab">Target</span><br><span class="val">3&ndash;5 sec</span></span>
        </div>
        <div class="cell">
          <span class="icn"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 9c2-2 4-2 6 0s4 2 6 0 4-2 6 0"/><path d="M3 15c2-2 4-2 6 0s4 2 6 0 4-2 6 0"/></svg></span>
          <span><span class="lab">Flow</span><br><span class="val" id="flowVal">&mdash;</span></span>
        </div>
      </div>

      <button class="end-link" onclick="endEarly()">End breath</button>
    </div>

    <!-- ---- Stage 3: Hold ---- -->
    <div class="stage" id="stage-hold">
      <div class="stage-title" style="font-size:1.2em;">Hold your breath</div>

      <div class="breath-wrap good holdfull">
        <div class="breath-track"></div>
        <div class="breath-circle"></div>
      </div>

      <div class="hold-note">&#9834; Nice breath!</div>
      <div class="hold-word">Hold now.</div>
      <div class="hold-count" id="holdCount">5</div>

      <div class="status-pill good">
        <div class="cell">
          <span class="icn"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v7"/><path d="M12 10c0 3-1.5 4.5-3.5 6-1.6 1.2-4 1.3-4.9-.2C2.6 14 3 10.5 4.5 8.5 5.7 6.9 8 6.6 9 8.2"/><path d="M12 10c0 3 1.5 4.5 3.5 6 1.6 1.2 4 1.3 4.9-.2 1-1.8.6-5.3-.9-7.3-1.2-1.6-3.5-1.9-4.5-.3"/></svg></span>
          <span><span class="lab">Target</span><br><span class="val">3&ndash;5 sec</span></span>
        </div>
        <div class="cell">
          <span class="icn"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 9c2-2 4-2 6 0s4 2 6 0 4-2 6 0"/><path d="M3 15c2-2 4-2 6 0s4 2 6 0 4-2 6 0"/></svg></span>
          <span><span class="lab">Flow</span><br><span class="val">Good</span></span>
        </div>
      </div>
    </div>

    <!-- ---- Stage 4: Reset & exhale ---- -->
    <div class="stage" id="stage-exhale">
      <div class="stage-title" style="font-size:1.2em;">Remove mouthpiece<br>and exhale</div>
      <div class="stage-sub">Exhale normally and relax.</div>

      <div class="breath-wrap exhale" id="exhaleWrap">
        <div class="breath-track"></div>
        <div class="exhale-chevs"><span class="n">&#8963;</span><span class="s">&#8963;</span><span class="e">&#8963;</span><span class="w">&#8963;</span></div>
        <div class="breath-circle" id="exhaleCircle"></div>
      </div>

      <div class="coach-msg">
        <div class="line2">Exhale to reset.<br>We'll be ready for your next breath.</div>
      </div>

      <div class="status-pill">
        <div class="cell">
          <span class="icn"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v7"/><path d="M12 10c0 3-1.5 4.5-3.5 6-1.6 1.2-4 1.3-4.9-.2C2.6 14 3 10.5 4.5 8.5 5.7 6.9 8 6.6 9 8.2"/><path d="M12 10c0 3 1.5 4.5 3.5 6 1.6 1.2 4 1.3 4.9-.2 1-1.8.6-5.3-.9-7.3-1.2-1.6-3.5-1.9-4.5-.3"/></svg></span>
          <span><span class="lab">Breath</span><br><span class="val" id="repCounter">1 of 10</span></span>
        </div>
        <div class="cell">
          <span class="icn"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 9c2-2 4-2 6 0s4 2 6 0 4-2 6 0"/><path d="M3 15c2-2 4-2 6 0s4 2 6 0 4-2 6 0"/></svg></span>
          <span><span class="lab">Flow</span><br><span class="val">&mdash;&mdash;</span></span>
        </div>
      </div>

      <button class="end-link" onclick="endEarly()">End breath</button>
    </div>

    <!-- ---- Stage 5: Summary ---- -->
    <div class="stage" id="stage-summary">
      <div class="stage-title">Breath overview</div>
      <div class="stage-sub">Nice work — here's how that breath went.</div>

      <div class="sum-ring">
        <svg width="190" height="190" viewBox="0 0 190 190">
          <circle cx="95" cy="95" r="84" fill="none" stroke="#e5f6ee" stroke-width="11"/>
          <circle id="sumRing" cx="95" cy="95" r="84" fill="none" stroke="#4cc08c" stroke-width="11"
                  stroke-linecap="round" stroke-dasharray="527.8" stroke-dashoffset="527.8"/>
        </svg>
        <div class="inner">
          <div class="big" id="sumBreaths">0 / 10</div>
          <div class="lab">breaths today</div>
        </div>
      </div>

      <div class="stars" id="stars"></div>
      <div class="stars-label" id="starsLabel"></div>

      <div class="card" style="padding: 8px 18px;">
        <div class="metric-row"><span class="name">Consistency</span><span class="segs" id="mConsist"></span><span class="num" id="mConsistV">&mdash;</span></div>
        <div class="metric-row"><span class="name">Duration</span><span class="segs" id="mDur"></span><span class="num" id="mDurV">&mdash;</span></div>
        <div class="metric-row"><span class="name">Peak Flow</span><span class="segs" id="mPeak"></span><span class="num" id="mPeakV">&mdash;</span></div>
        <div class="metric-row"><span class="name">Volume</span><span class="segs" id="mVol"></span><span class="num" id="mVolV">&mdash;</span></div>
      </div>

      <button class="btn" onclick="gotoStage('checkin')">Continue to check-in</button>
    </div>

    <!-- ---- Stage 6: Check-in ---- -->
    <div class="stage" id="stage-checkin">
      <div class="stage-title" style="font-size:1.25em;">How did that breath feel?</div>
      <div class="stage-sub">Your feedback helps us personalize your care.</div>

      <!-- Sliders -->
      <div class="ci-card">
        <div class="ci-head">
          <span class="ci-icon"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="5" r="2.5"/><path d="M12 8v6m0 0-3.5 6M12 14l3.5 6M7 10.5 12 9l5 1.5"/></svg></span>
          <span><span class="t">Pain</span><br><span class="s" id="painLabel">Minimal pain</span></span>
        </div>
        <div class="ticks"><span>1</span><span>2</span><span>3</span><span>4</span><span>5</span><span>6</span><span>7</span><span>8</span><span>9</span><span>10</span></div>
        <input type="range" id="ciPain" min="1" max="10" value="1" oninput="sliderLabel('Pain')">
        <div class="range-ends"><span>Minimal</span><span>Severe pain</span></div>
      </div>

      <div class="ci-card">
        <div class="ci-head">
          <span class="ci-icon"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 3.6-6 8-6s8 2 8 6"/></svg></span>
          <span><span class="t">Tiredness</span><br><span class="s" id="tiredLabel">Barely tired</span></span>
        </div>
        <div class="ticks"><span>1</span><span>2</span><span>3</span><span>4</span><span>5</span><span>6</span><span>7</span><span>8</span><span>9</span><span>10</span></div>
        <input type="range" id="ciTired" min="1" max="10" value="1" oninput="sliderLabel('Tired')">
        <div class="range-ends"><span>Minimal</span><span>Very tired</span></div>
      </div>

      <div class="ci-card">
        <div class="ci-head">
          <span class="ci-icon"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v7"/><path d="M12 10c0 3-1.5 4.5-3.5 6-1.6 1.2-4 1.3-4.9-.2C2.6 14 3 10.5 4.5 8.5 5.7 6.9 8 6.6 9 8.2"/><path d="M12 10c0 3 1.5 4.5 3.5 6 1.6 1.2 4 1.3 4.9-.2 1-1.8.6-5.3-.9-7.3-1.2-1.6-3.5-1.9-4.5-.3"/></svg></span>
          <span><span class="t">Chest Tightness</span><br><span class="s" id="chestLabel">Minimal</span></span>
        </div>
        <div class="ticks"><span>1</span><span>2</span><span>3</span><span>4</span><span>5</span><span>6</span><span>7</span><span>8</span><span>9</span><span>10</span></div>
        <input type="range" id="ciChest" min="1" max="10" value="1" oninput="sliderLabel('Chest')">
        <div class="range-ends"><span>Minimal</span><span>Severe</span></div>
      </div>

      <!-- Dry mouth -->
      <div class="ci-card">
        <div class="ci-head">
          <span class="ci-icon"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3s6 7 6 11a6 6 0 0 1-12 0c0-4 6-11 6-11z"/></svg></span>
          <span><span class="t">Dry Mouth / Throat</span></span>
        </div>
        <div class="seg-row" data-group="dryMouth">
          <button class="seg-btn sel" onclick="segSel(this)">None</button>
          <button class="seg-btn" onclick="segSel(this)">Mild</button>
          <button class="seg-btn" onclick="segSel(this)">Severe</button>
        </div>
      </div>

      <!-- Cough -->
      <div class="ci-card">
        <div class="ci-head">
          <span class="ci-icon"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="9" cy="7" r="3"/><path d="M3 20c0-3.5 2.7-5 6-5"/><path d="M15 10c2 0 3 1.2 3 3m-2 3c2.5 0 4-1.5 4-4"/></svg></span>
          <span><span class="t">Did you cough?</span></span>
        </div>
        <div class="seg-row" data-group="cough">
          <button class="seg-btn sel" onclick="segSel(this); coughToggle(false)">No</button>
          <button class="seg-btn" onclick="segSel(this); coughToggle(true)">Yes</button>
        </div>

        <div id="coughSection" class="hidden">
          <div class="ci-q">Did coughing help clear your chest?</div>
          <div class="seg-row" data-group="coughClear">
            <button class="seg-btn" onclick="segSel(this)">Yes, I feel clearer</button>
            <button class="seg-btn" onclick="segSel(this)">A little</button>
            <button class="seg-btn" onclick="segSel(this)">No difference</button>
            <button class="seg-btn" onclick="segSel(this)">I'm not sure</button>
          </div>

          <div class="ci-q">Was coughing difficult?</div>
          <div class="seg-row" data-group="coughHard">
            <button class="seg-btn sel" onclick="segSel(this)">No</button>
            <button class="seg-btn" onclick="segSel(this)">Somewhat</button>
            <button class="seg-btn" onclick="segSel(this)">Yes</button>
          </div>
        </div>
      </div>

      <!-- Mucus -->
      <div class="ci-card">
        <div class="ci-head">
          <span class="ci-icon"><svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M9 3.5s4 4.7 4 7.4a4 4 0 0 1-8 0c0-2.7 4-7.4 4-7.4z"/><path d="M17 12.5s3 3.5 3 5.4a3 3 0 0 1-6 0c0-1.9 3-5.4 3-5.4z"/></svg></span>
          <span><span class="t">Was mucus present?</span></span>
        </div>
        <div class="seg-row" data-group="mucus">
          <button class="seg-btn sel" onclick="segSel(this); mucusToggle(false)">No</button>
          <button class="seg-btn" onclick="segSel(this); mucusToggle(true)">Yes</button>
        </div>

        <div id="mucusSection" class="hidden">
          <div class="ci-q">Mucus colour</div>
          <div class="dots-row" data-group="mucusColour">
            <div class="dot-opt sel" data-val="Clear" onclick="dotSel(this)"><span class="dot" style="background:#fff;"></span><span class="dl">Clear</span></div>
            <div class="dot-opt" data-val="White" onclick="dotSel(this)"><span class="dot" style="background:#e9e9e9;"></span><span class="dl">White</span></div>
            <div class="dot-opt" data-val="Yellow" onclick="dotSel(this)"><span class="dot" style="background:#f2c94c;"></span><span class="dl">Yellow</span></div>
            <div class="dot-opt" data-val="Green" onclick="dotSel(this)"><span class="dot" style="background:#6aa84f;"></span><span class="dl">Green</span></div>
            <div class="dot-opt" data-val="Brown" onclick="dotSel(this)"><span class="dot" style="background:#9a6b3f;"></span><span class="dl">Brown</span></div>
            <div class="dot-opt" data-val="Bloody" onclick="dotSel(this)"><span class="dot" style="background:#b02525;"></span><span class="dl">Bloody</span></div>
          </div>

          <div class="row" style="gap:14px;">
            <div style="flex:1;">
              <div class="ci-q">Thickness</div>
              <div class="seg-row compact" data-group="thickness">
                <button class="seg-btn" onclick="segSel(this)">Thin</button>
                <button class="seg-btn sel" onclick="segSel(this)">Moderate</button>
                <button class="seg-btn" onclick="segSel(this)">Thick</button>
              </div>
            </div>
            <div style="flex:1;">
              <div class="ci-q">Amount</div>
              <div class="seg-row compact" data-group="amount">
                <button class="seg-btn" onclick="segSel(this)">Small</button>
                <button class="seg-btn sel" onclick="segSel(this)">Moderate</button>
                <button class="seg-btn" onclick="segSel(this)">Large</button>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- Notes -->
      <div class="ci-card">
        <div class="ci-head">
          <span class="ci-icon"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M17 3l4 4L8 20l-5 1 1-5z"/></svg></span>
          <span><span class="t">Additional notes <span style="font-weight:400;color:var(--muted);">(optional)</span></span></span>
        </div>
        <textarea id="ciNotes" maxlength="250" placeholder="Anything else you'd like your care team to know?" oninput="notesCount()"></textarea>
        <div class="char-count"><span id="notesCount">0</span> / 250</div>
      </div>

      <div class="notice">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" style="flex:none;margin-top:1px;"><circle cx="12" cy="12" r="9"/><line x1="12" y1="10" x2="12" y2="16"/><circle cx="12" cy="7.2" r="0.4" fill="currentColor"/></svg>
        If you have severe symptoms or are concerned, please contact your care team.
      </div>

      <button class="btn" onclick="saveCheckin()">Save Check-In</button>
      <button class="btn ghost" style="margin-top:10px;" onclick="skipCheckin()">Skip</button>
      <div class="form-msg" id="checkinMsg" style="text-align:center;"></div>
    </div>
  </div>

  <!-- ================= HISTORY / TRENDS ================= -->
  <div class="view" id="view-history">
    <div class="hist-title">History</div>
    <div class="hist-sub">Your completed breathing sessions.</div>
    <div id="histList"></div>
  </div>
  <div class="view" id="view-trends">
    <div class="hist-title">Trends</div>
    <div class="hist-sub">Your breathing patterns over time.</div>

    <div class="trend-seg" data-group="trendRange">
      <button class="seg-btn" onclick="setTrendRange(3, this)">3 days</button>
      <button class="seg-btn sel" onclick="setTrendRange(7, this)">7 days</button>
      <button class="seg-btn" onclick="setTrendRange(0, this)">All time</button>
    </div>

    <div id="trendBody"></div>
  </div>

  <!-- ================= PROFILE ================= -->
  <div class="view" id="view-profile">
    <div class="card">
      <h3>Profile</h3>
      <label class="field-label" for="pName">Your name</label>
      <input type="text" id="pName" maxlength="20" placeholder="e.g. Ryan">
      <label class="field-label" for="pDaily">Daily goal (breaths per day)</label>
      <input type="number" id="pDaily" inputmode="numeric" min="1" max="50" placeholder="10">
      <label class="field-label" for="pDays">Recovery program length (days)</label>
      <input type="number" id="pDays" inputmode="numeric" min="1" max="365" placeholder="21">
      <div class="check-row">
        <input type="checkbox" id="pRestart">
        <label for="pRestart">Restart recovery (today becomes Day 1)</label>
      </div>
      <button class="btn" onclick="saveProfile()">Save</button>
      <div class="form-msg" id="profileMsg"></div>
    </div>

    <div class="card">
      <h3>Volume goal</h3>
      <label class="field-label" for="goalInput">Target per breath (1&ndash;4000 mL)</label>
      <div class="row">
        <input type="number" id="goalInput" inputmode="numeric" placeholder="e.g. 2000" min="1" max="4000">
        <button class="btn" onclick="setGoal()">Set</button>
      </div>
      <div class="form-msg" id="goalMsg"></div>
    </div>

    <div class="card">
      <h3>Device</h3>
      <p class="sub" id="deviceInfo">&mdash;</p>
      <p class="sub" id="syncStatus" style="margin-top:6px;">Cloud sync: not configured</p>
      <button class="btn ghost" onclick="syncHistory()">Sync now</button>
    </div>
  </div>

  <!-- ================= BOTTOM NAV ================= -->
  <nav class="bottom-nav">
    <button class="nav-item active" id="nav-home" onclick="showView('home')">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="currentColor"><path d="M12 3l9 8h-3v9h-5v-6h-2v6H6v-9H3z"/></svg>
      Home
    </button>
    <button class="nav-item" id="nav-history" onclick="showView('history')">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M3 12a9 9 0 1 0 3-6.7"/><polyline points="3 4 3 9 8 9"/><polyline points="12 8 12 12 15 14"/></svg>
      History
    </button>
    <button class="nav-item" id="nav-trends" onclick="showView('trends')">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="3 17 9 11 13 15 21 7"/><polyline points="15 7 21 7 21 13"/></svg>
      Trends
    </button>
    <button class="nav-item" id="nav-profile" onclick="showView('profile')">
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="8" r="4"/><path d="M4 21c0-4 3.6-6 8-6s8 2 8 6"/></svg>
      Profile
    </button>
  </nav>

<script>
  const $ = (id) => document.getElementById(id);
  const RING_C = 251.3;          // home ring, r=40
  const SUM_C  = 527.8;          // summary ring, r=84
  let ws;
  let currentView = 'home';
  let profileFilled = false;

  // ================================================================
  // Supabase cloud sync (phone-relay: the BROWSER talks to Supabase,
  // the ESP32 never needs TLS). Fill these two in from your project:
  // Supabase dashboard -> Settings -> API.
  // Leave SUPABASE_URL empty to disable sync entirely.
  // ================================================================
  const SUPABASE_URL = 'https://bwaqswfqeybvtlbqzozf.supabase.co';
  const SUPABASE_KEY = 'eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImJ3YXFzd2ZxZXlidnRsYnF6b3pmIiwicm9sZSI6ImFub24iLCJpYXQiOjE3ODM2MTkyNDIsImV4cCI6MjA5OTE5NTI0Mn0.WTLWspvlBkFHSLYPY-NSfzLjoOApEw-HkU1tDZUObIA';
  let deviceId = '';         // arrives from the device broadcast
  let syncBusy = false;

  function setSyncStatus(msg) {
    const el = $('syncStatus');
    if (el) el.textContent = msg;
  }

  // Upsert ALL on-device history to Supabase. The device only holds 20
  // records and (device_id, epoch) is unique server-side, so pushing
  // everything every time is idempotent and needs no sync bookkeeping.
  // merge-duplicates means a check-in attached after an earlier sync
  // still updates the existing row.
  function syncHistory() {
    if (!SUPABASE_URL || !SUPABASE_KEY || !deviceId || syncBusy) return;
    syncBusy = true;
    setSyncStatus('Syncing\u2026');
    fetch('/history')
      .then(r => r.json())
      .then(list => {
        const rows = (Array.isArray(list) ? list : [])
          .filter(r => r.e)
          .map(r => ({
            device_id: deviceId,
            epoch: r.e,
            breaths: r.n !== undefined ? r.n : null,
            breaths_target: r.N !== undefined ? r.N : null,
            outcome: r.o || null,
            consistency: r.c !== undefined ? r.c : null,
            avg_volume_ml: r.v !== undefined ? r.v : null,
            avg_duration_s: r.d !== undefined ? r.d : null,
            avg_peak_flow_mls: r.p !== undefined ? r.p : null,
            checkin: r.ci || null
          }));
        if (rows.length === 0) { setSyncStatus('Nothing to sync'); return; }
        return fetch(SUPABASE_URL + '/rest/v1/sessions?on_conflict=device_id,epoch', {
          method: 'POST',
          headers: {
            'apikey': SUPABASE_KEY,
            'Authorization': 'Bearer ' + SUPABASE_KEY,
            'Content-Type': 'application/json',
            'Prefer': 'resolution=merge-duplicates,return=minimal'
          },
          body: JSON.stringify(rows)
        }).then(r => {
          setSyncStatus(r.ok
            ? 'Synced ' + rows.length + ' sessions \u00b7 ' + new Date().toLocaleTimeString()
            : 'Sync failed (' + r.status + ')');
        });
      })
      .catch(() => setSyncStatus('Offline \u2014 will sync when internet is available'))
      .finally(() => { syncBusy = false; });
  }

  // Flow thresholds mirroring firmware constants (mL/s)
  const FLOW_TOO_SLOW = 250, FLOW_TOO_FAST = 700, RAMP_SKIP_MS = 1000;
  const HOLD_SECONDS = 5;        // mirrors firmware HOLD_SECONDS
  const MIN_ACTIVE_FLOW = 30;    // below this we don't nag "too slow"

  // ================================================================
  // Haptics + pulse feedback
  // navigator.vibrate: works on Android Chrome; iOS Safari ignores it.
  // Single hook point — swap the body for a native bridge later.
  // ================================================================
  function haptic(kind) {
    const patterns = { tap: 45, double: [35, 50, 35] };
    if (navigator.vibrate) navigator.vibrate(patterns[kind] || 45);
  }
  function screenPulse(color) {           // 140ms tint, not a full flash
    const p = $('pulse');
    p.className = color;                  // 'red' | 'amber'
    void p.offsetWidth;                   // restart animation
    p.classList.add('go');
  }

  // ================================================================
  // Views & flow state machine
  // ================================================================
  const NAV_VIEWS = ['home', 'history', 'trends', 'profile'];
  function showView(name) {
    currentView = name;
    document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));
    $('view-' + name).classList.add('active');
    NAV_VIEWS.forEach(n => {
      const active = (n === name) || (n === 'home' && name === 'session');
      $('nav-' + n).classList.toggle('active', active);
    });
    window.scrollTo(0, 0);
    if (name === 'history') loadHistory();
    if (name === 'trends') loadTrends();
  }

  // flow.stage: prepare | inhale | hold | exhale | summary | checkin
  const flow = {
    active: false,
    stage: 'prepare',
    repsTarget: 10,             // daily breath goal (mirrors d.bGoal)
    todayDone: 0,               // breaths completed today (mirrors d.bToday)
    volTarget: 0,               // per-breath volume target, mL (mirrors d.target)
    reps: [],                    // per-rep stats {dur, peak, vol, consist}
    breathStartTs: 0,
    samples: [],                 // {t, flow} during inhale
    lastFlowClass: 'good',
    cntFast: 0,                  // session-wide sample counts (for outcome)
    cntSlow: 0,
    sessionSaved: false,
    holdTimer: null,
    exhaleTimer: null
  };

  // Progress is now per-day, not per-visit: one breath is one trip through
  // the flow, so the device's running count is the only meaningful tally.
  function progressText() {
    return Math.min(flow.todayDone, flow.repsTarget) + ' of ' + flow.repsTarget;
  }

  function gotoStage(name) {
    flow.stage = name;
    document.querySelectorAll('.stage').forEach(s => s.classList.remove('active'));
    $('stage-' + name).classList.add('active');
    window.scrollTo(0, 0);
    if (name === 'prepare') {
      const next = Math.min(flow.todayDone + 1, flow.repsTarget);
      $('prepProgress').textContent =
        'Breath ' + next + ' of ' + flow.repsTarget +
        (flow.todayDone > 0 ? ' — tap Start breath when ready' : '');
    }
    if (name === 'summary') renderSummary();
    if (name === 'checkin') resetCheckin();
  }

  function enterFlow() {
    flow.active = true;
    flow.reps = [];
    flow.cntFast = 0;
    flow.cntSlow = 0;
    flow.sessionSaved = false;
    resetRepCapture();
    showView('session');
    gotoStage('prepare');
  }

  function startBreathing() {
    // /startBreath arms the firmware — breaths NEVER begin without it
    fetch('/startBreath', { method: 'POST' }).catch(() => {});
    resetRepCapture();
    setInhaleClass('good');
    $('coachMain').textContent = 'Inhale to begin';
    $('coachLine2').textContent = 'Fill the circle by inhaling slowly and steadily.';
    $('coachChip').style.visibility = 'hidden';
    gotoStage('inhale');
  }

  function exitFlow() {
    clearTimers();
    flow.active = false;
    fetch('/newBreath', { method: 'POST' }).catch(() => {});  // reset & disarm (standby)
    showView('home');
  }

  function endEarly() {
    clearTimers();
    if (flow.reps.length > 0) gotoStage('summary');
    else exitFlow();
  }

  function clearTimers() {
    if (flow.holdTimer)   { clearInterval(flow.holdTimer);  flow.holdTimer = null; }
    if (flow.exhaleTimer) { clearTimeout(flow.exhaleTimer); flow.exhaleTimer = null; }
  }

  function resetRepCapture() {
    flow.breathStartTs = 0;
    flow.samples = [];
    flow.lastFlowClass = 'good';
  }

  // ---------------- Stage 2: live coaching ----------------
  function setInhaleClass(cls) {
    $('inhaleWrap').className = 'breath-wrap ' + cls;
    $('coachMsg').className = 'coach-msg ' + cls;
    $('inhalePill').className = 'status-pill ' + cls;
    const chip = $('coachChip');
    chip.className = 'flow-chip ' + cls;
    chip.innerHTML = '&#8776; ' + (cls === 'good' ? 'GOOD' : cls === 'fast' ? 'TOO FAST' : 'TOO SLOW');
  }

  const COACH_TEXT = {
    good: ['Good job!', 'Great inhale. Keep it smooth.'],
    fast: ['Slow down your inhale', 'Try to keep a steady, slow pace.'],
    slow: ['Inhale a little faster', 'You can take a deeper breath.']
  };
  const FLOW_LABEL = { good: 'Good', fast: 'Too fast', slow: 'Too slow' };

  function handleInhale(d) {
    // Circle size tracks volume toward the target
    const target = d.target > 0 ? d.target : 2000;
    const frac = Math.max(0.25, Math.min(1, d.volume / target));
    $('inhaleCircle').style.transform = 'scale(' + frac + ')';

    if (d.state !== 'Breathing') return;   // idle: waiting for inhale
    if (!flow.breathStartTs) flow.breathStartTs = Date.now();
    const sinceStart = Date.now() - flow.breathStartTs;
    flow.samples.push({ t: sinceStart, flow: d.flow });

    // Classification mirrors the firmware: skip the 1s ramp-up
    let cls = 'good';
    if (sinceStart >= RAMP_SKIP_MS) {
      if (d.flow > FLOW_TOO_FAST) cls = 'fast';
      else if (d.flow > MIN_ACTIVE_FLOW && d.flow < FLOW_TOO_SLOW) cls = 'slow';
      if (cls === 'fast') flow.cntFast++;
      else if (cls === 'slow') flow.cntSlow++;
    }

    setInhaleClass(cls);
    const [main, line2] = COACH_TEXT[cls];
    $('coachMain').textContent = main;
    $('coachLine2').textContent = line2;
    $('coachChip').style.visibility = 'visible';
    $('flowVal').textContent = FLOW_LABEL[cls];

    // Pulse + haptic only on ENTERING a correction state, not continuously
    if (cls !== flow.lastFlowClass && cls !== 'good') {
      screenPulse(cls === 'fast' ? 'red' : 'amber');
      haptic('tap');
    }
    flow.lastFlowClass = cls;
  }

  // ---------------- Stage 3: hold ----------------
  function enterHold() {
    gotoStage('hold');
    let n = HOLD_SECONDS;
    $('holdCount').textContent = n;
    clearTimers();
    flow.holdTimer = setInterval(() => {
      n--;
      if (n >= 1) {
        $('holdCount').textContent = n;
      } else {
        clearInterval(flow.holdTimer);
        flow.holdTimer = null;
      }
      // Stage exit is driven by the firmware's Done state (onmessage),
      // so the pill and firmware hold stay in lockstep even if drift.
    }, 1000);
  }

  // ---------------- Stage 4: exhale/reset ----------------
  function enterExhale(d) {
    // Capture this rep's stats before resetting
    const post = flow.samples.filter(s => s.t >= RAMP_SKIP_MS);
    const good = post.filter(s => s.flow >= FLOW_TOO_SLOW && s.flow <= FLOW_TOO_FAST).length;
    flow.reps.push({
      dur: flow.breathStartTs ? (flow.samples.length ? flow.samples[flow.samples.length - 1].t / 1000 : 0) : 0,
      peak: Math.max(0, ...flow.samples.map(s => s.flow)),
      vol: d.score,
      consist: post.length ? good / post.length : 1
    });

    $('repCounter').textContent = progressText();
    gotoStage('exhale');

    // Shrink animation
    const c = $('exhaleCircle');
    c.style.transition = 'none';
    c.style.transform = 'scale(1)';
    void c.offsetWidth;
    c.style.transition = '';
    requestAnimationFrame(() => { c.style.transform = 'scale(0.3)'; });

    // The firmware is already in DONE (sampling stopped). After the
    // exhale animation, return the device to standby and show this
    // breath's overview — one breath is one trip through the flow, so
    // the run always ends here rather than looping to the next rep.
    clearTimers();
    flow.exhaleTimer = setTimeout(() => {
      fetch('/newBreath', { method: 'POST' }).catch(() => {});  // DONE -> IDLE standby
      gotoStage('summary');
    }, 3000);
  }

  // ---------------- Stage 5: summary ----------------
  function segsInto(el, frac, colorClass) {
    el.innerHTML = '';
    const on = Math.round(Math.max(0, Math.min(1, frac)) * 10);
    for (let i = 0; i < 10; i++) {
      const s = document.createElement('span');
      s.className = 'seg' + (i < on ? ' on-' + colorClass : '');
      el.appendChild(s);
    }
  }

  function renderSummary() {
    const rep = flow.reps[flow.reps.length - 1];
    if (!rep) return;

    // The ring tracks the daily goal, not this visit: the device has
    // already counted this breath, so it reads e.g. 1 / 10 on the way home.
    const N = flow.repsTarget;
    const done = Math.min(flow.todayDone, N);
    $('sumBreaths').textContent = done + ' / ' + N;
    $('sumRing').style.strokeDashoffset = SUM_C * (1 - (N > 0 ? Math.min(1, done / N) : 0));

    const consist = rep.consist;
    const dur     = rep.dur;
    const peak    = rep.peak / 1000;           // mL/s -> L/s
    const vol     = rep.vol  / 1000;           // mL -> L

    segsInto($('mConsist'), consist, 'g');
    segsInto($('mDur'),  dur / 6,   'b');      // 6s span for the bar scale
    segsInto($('mPeak'), peak / 1,  'b');      // 1 L/s span
    segsInto($('mVol'),  vol / 2,   't');      // 2 L span
    $('mConsistV').textContent = Math.round(consist * 100) + '%';
    $('mDurV').textContent  = dur.toFixed(1) + ' sec';
    $('mPeakV').textContent = peak.toFixed(2) + ' L/s';
    $('mVolV').textContent  = vol.toFixed(2) + ' L';

    // Stars rate THIS breath: how steady it was and how close it came to
    // the volume target. Completion count no longer applies — a single
    // breath would otherwise always score 1/10 of the daily goal.
    const volFrac = flow.volTarget > 0 ? Math.min(1, rep.vol / flow.volTarget) : consist;
    const score = 0.5 * consist + 0.5 * volFrac;
    const starsN = Math.max(1, Math.round(score * 5));
    const starSvg = '<svg viewBox="0 0 24 24" fill="#f2b53d"><path d="M12 2.5l2.9 6 6.6 0.9-4.8 4.6 1.2 6.5L12 17.4l-5.9 3.1 1.2-6.5L2.5 9.4l6.6-0.9z"/></svg>';
    $('stars').innerHTML = Array.from({length: 5}, (_, i) =>
      '<span class="' + (i < starsN ? '' : 'off') + '">' + starSvg + '</span>').join('');
    $('starsLabel').textContent =
      starsN === 5 ? 'Perfect breath!' :
      starsN >= 4 ? 'Great breath!' :
      starsN >= 3 ? 'Good breath!' : 'Breath complete';

    // Persist this breath to the device (once per trip through the flow).
    // Each record is now a single breath, which is what the check-in
    // attaches to and what the trends page sums per day.
    if (!flow.sessionSaved) {
      flow.sessionSaved = true;
      const outcome = consist >= 0.85 ? 'Perfect'
                    : flow.cntFast >= flow.cntSlow ? 'Too fast' : 'Too slow';
      const body =
        'epoch='    + Math.floor(Date.now() / 1000) +
        '&n=1&N='   + N +
        '&outcome=' + encodeURIComponent(outcome) +
        '&consist=' + Math.round(consist * 100) +
        '&vol='     + Math.round(rep.vol) +
        '&dur='     + dur.toFixed(1) +
        '&peak='    + Math.round(rep.peak);
      fetch('/session', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body
      }).catch(() => {});
    }
  }

  // ---------------- History ----------------
  const OUTCOME_CHIP = { 'Perfect': 'perfect', 'Too fast': 'fast', 'Too slow': 'slow' };

  function fmtHistDate(e) {
    if (!e) return 'Unknown time';
    const d = new Date(e * 1000);
    return d.toLocaleDateString(undefined, { weekday: 'short', month: 'short', day: 'numeric' })
      + ' \u00b7 ' + d.toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
  }

  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function ciPairs(ci) {
    const out = [];
    out.push('<span><b>Pain</b> ' + esc(ci.pain) + '/10</span>');
    out.push('<span><b>Tiredness</b> ' + esc(ci.tired) + '/10</span>');
    out.push('<span><b>Chest</b> ' + esc(ci.chest) + '/10</span>');
    if (ci.dry)   out.push('<span><b>Dry mouth</b> ' + esc(ci.dry) + '</span>');
    if (ci.cough) out.push('<span><b>Cough</b> ' + esc(ci.cough) + '</span>');
    if (ci.clear) out.push('<span><b>Chest cleared</b> ' + esc(ci.clear) + '</span>');
    if (ci.hard)  out.push('<span><b>Coughing difficult</b> ' + esc(ci.hard) + '</span>');
    if (ci.mucus === 'Yes') {
      let m = 'Yes';
      if (ci.colour) m += ', ' + ci.colour;
      if (ci.thick)  m += ', ' + ci.thick.toLowerCase();
      if (ci.amt)    m += ', ' + ci.amt.toLowerCase() + ' amount';
      out.push('<span><b>Mucus</b> ' + esc(m) + '</span>');
    } else if (ci.mucus) {
      out.push('<span><b>Mucus</b> ' + esc(ci.mucus) + '</span>');
    }
    return out.join('');
  }

  function renderHistory(list) {
    const el = $('histList');
    if (!Array.isArray(list) || list.length === 0) {
      el.innerHTML = '<div class="card placeholder"><div class="big">&#127793;</div>' +
        '<h3>No sessions yet</h3><p class="sub">Complete a breathing session and it will show up here.</p></div>';
      return;
    }
    el.innerHTML = list.map((r, i) => {
      const hasSession = r.n !== undefined;
      const chipCls = hasSession ? (OUTCOME_CHIP[r.o] || 'none') : 'none';
      const chipTxt = hasSession ? (r.o || '\u2014') : 'Check-in only';
      // Records are one breath each now; older multi-breath ones still render.
      const meta = hasSession
        ? (r.n === 1 ? '1 breath \u00b7 ' + ((r.v || 0) / 1000).toFixed(2) + ' L'
                     : r.n + ' of ' + r.N + ' breaths \u00b7 ' + ((r.v || 0) / 1000).toFixed(2) + ' L avg')
        : 'Survey response';
      const stats = hasSession
        ? '<div class="hist-stats">' +
            '<span><b>Consistency</b> ' + (r.c || 0) + '%</span>' +
            '<span><b>Duration</b> ' + (r.d || 0) + ' s</span>' +
            '<span><b>Peak flow</b> ' + ((r.p || 0) / 1000).toFixed(2) + ' L/s</span>' +
          '</div>'
        : '';
      const ci = r.ci
        ? '<div class="hist-ci-title">Check-in</div><div class="hist-ci">' + ciPairs(r.ci) + '</div>' +
          (r.ci.notes ? '<div class="hist-notes">&ldquo;' + esc(r.ci.notes) + '&rdquo;</div>' : '')
        : '<div class="hist-ci-title">Check-in</div><div class="hist-none">No check-in recorded.</div>';
      return '<div class="card">' +
        '<div class="hist-top" onclick="toggleHist(' + i + ')">' +
          '<div><div class="hist-date">' + fmtHistDate(r.e) + '</div>' +
          '<div class="hist-meta">' + meta + '</div></div>' +
          '<span class="chip ' + chipCls + '">' + esc(chipTxt) + '</span>' +
        '</div>' +
        '<div class="hist-detail hidden" id="histDetail' + i + '">' + stats + ci + '</div>' +
      '</div>';
    }).join('');
  }

  function toggleHist(i) {
    $('histDetail' + i).classList.toggle('hidden');
  }

  function loadHistory() {
    $('histList').innerHTML = '<div class="card placeholder"><p class="sub">Loading&hellip;</p></div>';
    fetch('/history')
      .then(r => r.json())
      .then(renderHistory)
      .catch(() => {
        $('histList').innerHTML = '<div class="card placeholder"><p class="sub">Could not load history.</p></div>';
      });
  }

  // ---------------- Trends ----------------
  let trendsData = null;
  let trendRange = 7;          // 3, 7, or 0 = all time

  function setTrendRange(days, btn) {
    trendRange = days;
    segSel(btn);
    renderTrends();
  }

  function loadTrends() {
    $('trendBody').innerHTML = '<div class="card placeholder"><p class="sub">Loading&hellip;</p></div>';
    fetch('/history')
      .then(r => r.json())
      .then(list => { trendsData = Array.isArray(list) ? list : []; renderTrends(); })
      .catch(() => {
        $('trendBody').innerHTML = '<div class="card placeholder"><p class="sub">Could not load trends.</p></div>';
      });
  }

  function dayKey(e) { return new Date(e * 1000).toDateString(); }

  function sparkline(values) {
    // values: daily adherence %, oldest first
    const W = 200, H = 70, PAD = 6;
    const n = values.length;
    if (n === 0) return '';
    const x = (i) => n === 1 ? W / 2 : PAD + (W - 2 * PAD) * i / (n - 1);
    const y = (v) => H - PAD - (H - 2 * PAD) * Math.min(100, v) / 100;
    const pts = values.map((v, i) => x(i).toFixed(1) + ',' + y(v).toFixed(1));
    const line = pts.join(' ');
    const area = 'M' + pts[0] + ' L' + pts.slice(1).join(' L') +
                 ' L' + x(n - 1).toFixed(1) + ',' + (H - PAD) +
                 ' L' + x(0).toFixed(1) + ',' + (H - PAD) + ' Z';
    const grid = [0.25, 0.5, 0.75].map(f =>
      '<line x1="' + (PAD + (W - 2 * PAD) * f) + '" y1="' + PAD + '" x2="' + (PAD + (W - 2 * PAD) * f) +
      '" y2="' + (H - PAD) + '" stroke="#eef2f8" stroke-width="1"/>').join('');
    const last = pts[n - 1].split(',');
    return '<svg viewBox="0 0 ' + W + ' ' + H + '" width="100%" preserveAspectRatio="none" style="display:block;">' +
      grid +
      '<path d="' + area + '" fill="rgba(76,192,140,0.16)"/>' +
      '<polyline points="' + line + '" fill="none" stroke="#4cc08c" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"/>' +
      '<circle cx="' + last[0] + '" cy="' + last[1] + '" r="3.5" fill="#4cc08c"/>' +
    '</svg>';
  }

  function renderTrends() {
    if (!trendsData) return;
    const goal = flow.repsTarget || 10;
    const sessions = trendsData.filter(r => r.e && r.n !== undefined);

    if (sessions.length === 0) {
      $('trendBody').innerHTML = '<div class="card placeholder"><div class="big">&#127793;</div>' +
        '<h3>No data yet</h3><p class="sub">Complete a few sessions to see your trends.</p></div>';
      return;
    }

    // Window: N days back from today, or oldest record (capped 30) for all-time
    const now = Date.now() / 1000;
    let days = trendRange;
    if (days === 0) {
      const oldest = Math.min(...sessions.map(r => r.e));
      days = Math.min(30, Math.max(1, Math.ceil((now - oldest) / 86400) + 1));
    }
    const cutoff = now - days * 86400;
    const inWin = sessions.filter(r => r.e >= cutoff);

    // Daily buckets, oldest first
    const daily = [];
    for (let i = days - 1; i >= 0; i--) {
      const key = dayKey(now - i * 86400);
      const breaths = inWin.filter(r => dayKey(r.e) === key)
                           .reduce((a, r) => a + (r.n || 0), 0);
      daily.push(Math.min(100, 100 * breaths / goal));
    }

    const totBreaths = inWin.reduce((a, r) => a + (r.n || 0), 0);
    const totTarget  = inWin.reduce((a, r) => a + (r.N || 0), 0);
    const adherence  = Math.min(100, Math.round(100 * totBreaths / (goal * days)));
    const avg = (fn) => inWin.length ? inWin.reduce((a, r) => a + fn(r), 0) / inWin.length : 0;
    const dur  = avg(r => r.d || 0);
    const peak = avg(r => r.p || 0) / 1000;
    const vol  = avg(r => r.v || 0) / 1000;
    const fastShare = inWin.length
      ? inWin.filter(r => r.o === 'Too fast').length / inWin.length : 0;
    const slowShare = inWin.length
      ? inWin.filter(r => r.o === 'Too slow').length / inWin.length : 0;

    let note = adherence >= 80 ? 'Excellent progress.'
             : adherence >= 50 ? 'Good progress \u2014 try to fit in one more session a day.'
             : 'Let\u2019s rebuild the habit \u2014 even one session today counts.';
    if (fastShare > 0.4)      note += ' Keep encouraging deep, slow breaths.';
    else if (slowShare > 0.4) note += ' Encourage slightly faster, fuller inhales.';

    const rangeLabel = trendRange === 0 ? 'All time (' + days + ' days)' : 'Last ' + days + ' days';

    $('trendBody').innerHTML =
      '<div class="card adh-card">' +
        '<div class="adh-left"><h3>Adherence</h3>' +
          '<div class="adh-pct">' + adherence + '%</div>' +
          '<div class="adh-sub">' + rangeLabel + '</div></div>' +
        '<div class="adh-chart">' + sparkline(daily) + '</div>' +
      '</div>' +
      '<div class="card trend-rows">' +
        '<div class="trend-row"><span class="tl">Avg. inspiratory duration</span><span class="tv">' + dur.toFixed(1) + ' sec</span></div>' +
        '<div class="trend-row"><span class="tl">Avg. peak flow</span><span class="tv">' + peak.toFixed(2) + ' L/s</span></div>' +
        '<div class="trend-row"><span class="tl">Avg. volume</span><span class="tv">' + vol.toFixed(2) + ' L</span></div>' +
        '<div class="trend-row"><span class="tl">Total sessions</span><span class="tv">' + inWin.length + '</span></div>' +
        '<div class="trend-row"><span class="tl">Total breaths</span><span class="tv">' + totBreaths + ' / ' + totTarget + '</span></div>' +
      '</div>' +
      '<div class="trend-note">' + note + '</div>';
  }

  // ---------------- Stage 6: check-in ----------------
  const SLIDER_WORDS = {
    Pain:  ['Minimal pain', 'Mild pain', 'Moderate pain', 'Severe pain'],
    Tired: ['Barely tired', 'A little tired', 'Quite tired', 'Very tired'],
    Chest: ['Minimal', 'Mild', 'Moderate', 'Severe']
  };
  const SLIDER_LABEL_EL = { Pain: 'painLabel', Tired: 'tiredLabel', Chest: 'chestLabel' };
  const SLIDER_INPUT_EL = { Pain: 'ciPain', Tired: 'ciTired', Chest: 'ciChest' };
  // Sliders run 1-10, so the four words split the range into quarters.
  function sliderLabel(which) {
    const v = +$(SLIDER_INPUT_EL[which]).value;
    const w = SLIDER_WORDS[which];
    $(SLIDER_LABEL_EL[which]).textContent = v <= 2 ? w[0] : v <= 5 ? w[1] : v <= 8 ? w[2] : w[3];
  }

  function segSel(btn) {
    btn.parentElement.querySelectorAll('.seg-btn').forEach(b => b.classList.remove('sel'));
    btn.classList.add('sel');
  }
  function dotSel(el) {
    el.parentElement.querySelectorAll('.dot-opt').forEach(d => d.classList.remove('sel'));
    el.classList.add('sel');
  }
  function coughToggle(yes) { $('coughSection').classList.toggle('hidden', !yes); }
  function mucusToggle(yes) { $('mucusSection').classList.toggle('hidden', !yes); }
  function notesCount() { $('notesCount').textContent = $('ciNotes').value.length; }

  function segVal(group) {
    const sel = document.querySelector('[data-group="' + group + '"] .sel');
    return sel ? (sel.dataset.val || sel.textContent.trim()) : '';
  }

  // Defaults are read from the markup once, so resetCheckin() can restore
  // them after a session without duplicating the option list here.
  const CI_DEFAULTS = {};
  document.querySelectorAll('#stage-checkin [data-group]').forEach(g => {
    const sel = g.querySelector('.sel');
    CI_DEFAULTS[g.dataset.group] = sel ? (sel.dataset.val || sel.textContent.trim()) : null;
  });

  function resetCheckin() {
    ['Pain', 'Tired', 'Chest'].forEach(k => {
      $(SLIDER_INPUT_EL[k]).value = 1;
      sliderLabel(k);
    });
    document.querySelectorAll('#stage-checkin [data-group]').forEach(g => {
      const want = CI_DEFAULTS[g.dataset.group];
      g.querySelectorAll('.seg-btn, .dot-opt').forEach(o => {
        const v = o.dataset.val || o.textContent.trim();
        o.classList.toggle('sel', want !== null && v === want);
      });
    });
    coughToggle(false);
    mucusToggle(false);
    $('ciNotes').value = '';
    notesCount();
    $('checkinMsg').textContent = '';
  }

  function skipCheckin() {
    flow.active = false;
    showView('home');
  }

  function saveCheckin() {
    const coughYes = segVal('cough') === 'Yes';
    const mucusYes = segVal('mucus') === 'Yes';
    const fields = {
      epoch: Math.floor(Date.now() / 1000),
      breaths: flow.reps.length,
      pain: $('ciPain').value,
      tiredness: $('ciTired').value,
      chestTightness: $('ciChest').value,
      dryMouth: segVal('dryMouth'),
      cough: segVal('cough'),
      mucus: segVal('mucus'),
      mucusColour: mucusYes ? segVal('mucusColour') : '',
      thickness: mucusYes ? segVal('thickness') : '',
      amount: mucusYes ? segVal('amount') : '',
      coughClear: coughYes ? segVal('coughClear') : '',
      coughHard: coughYes ? segVal('coughHard') : '',
      notes: $('ciNotes').value
    };
    const body = Object.entries(fields)
      .map(([k, v]) => k + '=' + encodeURIComponent(v)).join('&');
    fetch('/checkin', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    })
    .then(r => {
      $('checkinMsg').textContent = r.ok ? 'Saved. Take care!' : 'Save failed';
      if (r.ok) {
        setTimeout(syncHistory, 600);   // give loop() a moment to commit to NVS
        setTimeout(() => { flow.active = false; showView('home'); }, 900);
      }
    })
    .catch(() => { $('checkinMsg').textContent = 'Could not reach device'; });
  }

  // ================================================================
  // Greeting + countdown formatting
  // ================================================================
  function setGreeting() {
    const h = new Date().getHours();
    $('greetWord').textContent =
      h < 12 ? 'Good morning' : h < 18 ? 'Good afternoon' : 'Good evening';
  }
  setGreeting();
  setInterval(setGreeting, 60000);

  // The countdown is a suggestion, not a gate — the card starts a breath
  // whenever it is tapped.
  function fmtNext(s) {
    if (s <= 0)  return 'Ready when you are';
    const h = Math.floor(s / 3600), m = Math.ceil((s % 3600) / 60);
    return 'Ready now · next due in ' + (h > 0 ? h + 'h ' + m + 'm' : m + 'm');
  }

  // ================================================================
  // WebSocket
  // ================================================================
  function connect() {
    ws = new WebSocket(`ws://${location.host}/ws`);

    ws.onopen = () => {
      $('conn').textContent = 'Connected';
      $('conn').classList.add('ok');
      const now = new Date();
      const epoch = Math.floor(Date.now() / 1000 - now.getTimezoneOffset() * 60);
      ws.send(JSON.stringify({ epoch }));
    };

    ws.onmessage = (event) => {
      const d = JSON.parse(event.data);

      // ---- Home ----
      $('greetName').textContent = d.name;
      $('bToday').textContent = d.bToday;
      $('bGoal').textContent  = d.bGoal;
      flow.todayDone = d.bToday;
      if (d.target > 0) flow.volTarget = d.target;
      const frac = d.bGoal > 0 ? Math.min(1, d.bToday / d.bGoal) : 0;
      $('ringFill').style.strokeDashoffset = RING_C * (1 - frac);
      $('nextSession').textContent = fmtNext(d.nextS);
      $('recoveryDay').textContent =
        d.day > 0 ? 'Day ' + d.day + ' of ' + d.rDays : 'Day \u2014 of ' + d.rDays;
      flow.repsTarget = d.bGoal > 0 ? d.bGoal : 10;

      // ---- Profile ----
      if (!profileFilled && d.name !== undefined) {
        $('pName').value  = d.name;
        $('pDaily').value = d.bGoal;
        $('pDays').value  = d.rDays;
        profileFilled = true;
      }
      $('deviceInfo').textContent = d.mode + ' mode \u00b7 ' + d.ip;
      if (!deviceId && d.dev) {
        deviceId = d.dev;
        setTimeout(syncHistory, 800);   // first sync shortly after connect
      }

      // ---- Exercise flow ----
      // Sessions start only from the app's own "Start breath" button —
      // never from sensor movement or device state.
      if (!flow.active) return;

      if (flow.stage === 'inhale') {
        handleInhale(d);
        if (d.state === 'Hold') enterHold();
        else if (d.state === 'Done') enterExhale(d);   // hold skipped (e.g. timeout edge)
      } else if (flow.stage === 'hold') {
        if (d.state === 'Done') enterExhale(d);
      }
    };

    ws.onclose = () => {
      $('conn').textContent = 'Reconnecting';
      $('conn').classList.remove('ok');
      setTimeout(connect, 1500);
    };
  }
  connect();

  // ================================================================
  // Profile / goal actions
  // ================================================================
  function setGoal() {
    const val = $('goalInput').value;
    if (!val) return;
    fetch('/setGoal', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'value=' + encodeURIComponent(val)
    })
    .then(r => r.text().then(t => {
      $('goalMsg').textContent = r.ok
        ? ('Goal set to ' + val + ' mL — tap Start breath when you’re ready')
        : t;
    }))
    .catch(() => { $('goalMsg').textContent = 'Could not reach device'; });
  }

  function saveProfile() {
    const body =
      'name='          + encodeURIComponent($('pName').value) +
      '&dailyGoal='    + encodeURIComponent($('pDaily').value) +
      '&recoveryDays=' + encodeURIComponent($('pDays').value) +
      '&restart='      + ($('pRestart').checked ? '1' : '0');
    fetch('/profile', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body
    })
    .then(r => {
      $('profileMsg').textContent = r.ok ? 'Saved' : 'Save failed';
      $('pRestart').checked = false;
    })
    .catch(() => { $('profileMsg').textContent = 'Could not reach device'; });
  }
</script>
</body>
</html>
)rawliteral";

#endif
