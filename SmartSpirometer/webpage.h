// ================================================================
// webpage.h — Smart Spirometer mobile dashboard
//
// Served from PROGMEM at http://192.168.4.1
// IMPORTANT: the ESP32 access point has NO internet access, so this
// page must stay fully self-contained: no CDN fonts, no external
// scripts, no icon libraries. Everything is inline.
// ================================================================

#ifndef WEBPAGE_H
#define WEBPAGE_H

#include <pgmspace.h>

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Spirometer</title>
<style>
  :root {
    --bg: #0d1117;
    --panel: #161b22;
    --text: #e6edf3;
    --dim: #8b949e;
    --accent: #3fb950;
    --accent-dim: #1f6f33;
    --warn: #d29922;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: -apple-system, "Segoe UI", Roboto, sans-serif;
    background: var(--bg);
    color: var(--text);
    min-height: 100vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    padding: 24px 16px;
  }
  h1 {
    font-size: 0.85em;
    letter-spacing: 0.25em;
    color: var(--dim);
    font-weight: 600;
    margin-bottom: 4px;
  }
  .conn {
    font-size: 0.75em;
    margin-bottom: 24px;
  }
  .conn.ok  { color: var(--accent); }
  .conn.bad { color: var(--warn); }

  /* --- Live reading --- */
  .reading {
    background: var(--panel);
    border-radius: 16px;
    padding: 24px;
    width: 100%;
    max-width: 400px;
    text-align: center;
  }
  .volume {
    font-size: 3.4em;
    font-weight: 700;
    font-variant-numeric: tabular-nums;
    line-height: 1.1;
  }
  .volume span { font-size: 0.35em; color: var(--dim); font-weight: 400; }
  .target-line { color: var(--dim); margin: 6px 0 18px; }

  .bar-track {
    background: #21262d;
    border-radius: 8px;
    height: 22px;
    overflow: hidden;
  }
  .bar-fill {
    background: var(--accent);
    height: 100%;
    width: 0%;
    transition: width 0.12s linear;
  }

  .meta {
    display: flex;
    justify-content: space-between;
    margin-top: 14px;
    font-size: 0.9em;
    color: var(--dim);
    font-variant-numeric: tabular-nums;
  }
  .state {
    margin-top: 14px;
    font-size: 1.15em;
    font-weight: 600;
    min-height: 1.3em;
  }
  .state.done { color: var(--accent); }

  /* --- Goal setting --- */
  .goal-box {
    margin-top: 24px;
    width: 100%;
    max-width: 400px;
    background: var(--panel);
    border-radius: 16px;
    padding: 20px;
  }
  .goal-box label {
    display: block;
    color: var(--dim);
    font-size: 0.85em;
    margin-bottom: 10px;
  }
  .goal-row { display: flex; gap: 10px; }
  input[type=number] {
    flex: 1;
    font-size: 1.2em;
    padding: 12px;
    border-radius: 10px;
    border: 1px solid #30363d;
    background: #0d1117;
    color: var(--text);
    text-align: center;
    -moz-appearance: textfield;
  }
  input::-webkit-outer-spin-button,
  input::-webkit-inner-spin-button { -webkit-appearance: none; }
  input:focus { outline: 2px solid var(--accent-dim); }
  button {
    font-size: 1.05em;
    font-weight: 600;
    padding: 12px 20px;
    border-radius: 10px;
    border: none;
    background: var(--accent);
    color: #04120a;
    cursor: pointer;
  }
  button:active { background: var(--accent-dim); color: var(--text); }
  .goal-msg {
    margin-top: 10px;
    font-size: 0.85em;
    color: var(--dim);
    min-height: 1.2em;
  }
</style>
</head>
<body>

  <h1>SMART SPIROMETER</h1>
  <div class="conn bad" id="conn">Connecting…</div>

  <!-- Feature 2: current reading -->
  <div class="reading">
    <div class="volume"><span id="vol">0</span><span> mL</span></div>
    <div class="target-line">Target: <span id="target">not set</span></div>
    <div class="bar-track"><div class="bar-fill" id="bar"></div></div>
    <div class="meta">
      <div>Flow: <span id="flow">0</span> mL/s</div>
      <div>Peak: <span id="score">0</span> mL</div>
    </div>
    <div class="state" id="state"></div>
  </div>

  <!-- Feature 1: set a volume goal -->
  <div class="goal-box">
    <label for="goalInput">Set volume goal (1&ndash;4000 mL)</label>
    <div class="goal-row">
      <input type="number" id="goalInput" inputmode="numeric" placeholder="e.g. 2000" min="1" max="4000">
      <button onclick="setGoal()">Set goal</button>
    </div>
    <div class="goal-msg" id="goalMsg"></div>
  </div>

<script>
  const $ = (id) => document.getElementById(id);
  let ws;

  function connect() {
    ws = new WebSocket(`ws://${location.host}/ws`);

    ws.onopen = () => {
      $('conn').textContent = 'Connected';
      $('conn').className = 'conn ok';
    };

    ws.onmessage = (event) => {
      const d = JSON.parse(event.data);

      $('vol').textContent   = Math.round(d.volume);
      $('flow').textContent  = Math.round(d.flow);
      $('score').textContent = Math.round(d.score);

      if (d.target > 0) {
        $('target').textContent = Math.round(d.target) + ' mL';
        const frac = Math.min(100, (d.volume / d.target) * 100);
        $('bar').style.width = frac + '%';
      } else {
        $('target').textContent = 'not set';
        $('bar').style.width = '0%';
      }

      $('state').textContent = d.state;
      $('state').className = 'state' + (d.state === 'Done' ? ' done' : '');
    };

    ws.onclose = () => {
      $('conn').textContent = 'Disconnected — retrying…';
      $('conn').className = 'conn bad';
      setTimeout(connect, 1500);   // auto-reconnect
    };
  }
  connect();

  function setGoal() {
    const val = $('goalInput').value;
    if (!val) return;
    fetch('/setGoal', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'value=' + encodeURIComponent(val)
    })
    .then(r => r.text().then(t => {
      $('goalMsg').textContent = r.ok ? ('Goal set to ' + val + ' mL') : t;
    }))
    .catch(() => { $('goalMsg').textContent = 'Could not reach device'; });
  }
</script>
</body>
</html>
)rawliteral";

#endif
