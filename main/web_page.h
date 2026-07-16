/*
 * Embedded Web Page for Camera Status Monitor
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#pragma once

static const char WEB_PAGE_HTML[] = R"rawhtml(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32-P4 Camera Monitor</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  background: #0f172a; color: #e2e8f0; min-height: 100vh;
}
.header {
  background: linear-gradient(135deg, #1e293b, #334155);
  padding: 16px 24px; border-bottom: 1px solid #475569;
  display: flex; align-items: center; gap: 12px;
}
.header h1 { font-size: 1.3em; font-weight: 600; }
.header .badge {
  background: #22c55e; color: #000; padding: 2px 10px;
  border-radius: 12px; font-size: 0.75em; font-weight: 600;
}
.header .badge.offline { background: #ef4444; color: #fff; }
.container { max-width: 1200px; margin: 0 auto; padding: 20px; }
.grid { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
@media (max-width: 768px) { .grid { grid-template-columns: 1fr; } }
.card {
  background: #1e293b; border: 1px solid #334155; border-radius: 12px;
  padding: 20px; transition: border-color 0.2s;
}
.card:hover { border-color: #64748b; }
.card h2 {
  font-size: 0.9em; text-transform: uppercase; letter-spacing: 0.05em;
  color: #94a3b8; margin-bottom: 12px; display: flex; align-items: center; gap: 8px;
}
.card h2 .icon { font-size: 1.2em; }
.stat-row {
  display: flex; justify-content: space-between; align-items: center;
  padding: 8px 0; border-bottom: 1px solid #1e293b;
}
.stat-row:last-child { border-bottom: none; }
.stat-label { color: #94a3b8; font-size: 0.9em; }
.stat-value { font-weight: 600; font-size: 0.95em; }
.stat-value.ok { color: #22c55e; }
.stat-value.warn { color: #f59e0b; }
.stat-value.err { color: #ef4444; }
.stream-card { grid-column: 1 / -1; }
.stream-container {
  background: #000; border-radius: 8px; overflow: hidden;
  position: relative; aspect-ratio: 1/1; max-height: 500px;
  margin: 0 auto; width: 100%; max-width: 500px;
}
.stream-container img {
  width: 100%; height: 100%; object-fit: contain;
}
.stream-overlay {
  position: absolute; top: 8px; right: 8px;
  background: rgba(0,0,0,0.7); padding: 4px 10px;
  border-radius: 6px; font-size: 0.8em; color: #f59e0b;
}
.stream-controls {
  display: flex; gap: 8px; margin-top: 12px; justify-content: center;
}
.btn {
  padding: 8px 20px; border: 1px solid #475569; border-radius: 8px;
  background: #334155; color: #e2e8f0; cursor: pointer;
  font-size: 0.9em; transition: all 0.2s;
}
.btn:hover { background: #475569; border-color: #64748b; }
.btn.active { background: #3b82f6; border-color: #3b82f6; }
.status-dot {
  display: inline-block; width: 8px; height: 8px;
  border-radius: 50%; margin-right: 6px;
}
.status-dot.green { background: #22c55e; box-shadow: 0 0 6px #22c55e; }
.status-dot.red { background: #ef4444; box-shadow: 0 0 6px #ef4444; }
.status-dot.yellow { background: #f59e0b; box-shadow: 0 0 6px #f59e0b; }
.face-count {
  font-size: 2.5em; font-weight: 700; color: #3b82f6;
  text-align: center; padding: 10px 0;
}
.face-label { text-align: center; color: #94a3b8; font-size: 0.85em; }
.footer {
  text-align: center; padding: 20px; color: #475569; font-size: 0.8em;
}
</style>
</head>
<body>
<div class="header">
  <h1>ESP32-P4 Camera Monitor</h1>
  <span id="conn-badge" class="badge">ONLINE</span>
</div>
<div class="container">
  <div class="grid">
    <!-- Camera Info -->
    <div class="card">
      <h2><span class="icon">&#128247;</span> Camera</h2>
      <div class="stat-row">
        <span class="stat-label">Sensor</span>
        <span class="stat-value" id="cam-sensor">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Resolution</span>
        <span class="stat-value" id="cam-res">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Format</span>
        <span class="stat-value" id="cam-format">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Frame Rate</span>
        <span class="stat-value" id="cam-fps">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Status</span>
        <span class="stat-value" id="cam-status">--</span>
      </div>
    </div>

    <!-- Detection Info -->
    <div class="card">
      <h2><span class="icon">&#128065;</span> Face Detection</h2>
      <div class="stat-row">
        <span class="stat-label">Model</span>
        <span class="stat-value" id="det-model">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Detect FPS</span>
        <span class="stat-value" id="det-fps">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Status</span>
        <span class="stat-value" id="det-status">--</span>
      </div>
      <div class="face-count" id="face-count">0</div>
      <div class="face-label">Faces Detected</div>
    </div>

    <!-- Stream -->
    <div class="card stream-card">
      <h2><span class="icon">&#127909;</span> Live Stream</h2>
      <div class="stream-container">
        <img id="stream-img" src="" alt="Camera stream not started">
        <div class="stream-overlay" id="stream-info">--</div>
      </div>
      <div class="stream-controls">
        <button class="btn" id="btn-stream" onclick="toggleStream()">Start Stream</button>
        <button class="btn" id="btn-snapshot" onclick="takeSnapshot()">Snapshot</button>
      </div>
    </div>

    <!-- System Info -->
    <div class="card">
      <h2><span class="icon">&#9881;</span> System</h2>
      <div class="stat-row">
        <span class="stat-label">Uptime</span>
        <span class="stat-value" id="sys-uptime">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Free Heap</span>
        <span class="stat-value" id="sys-heap">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Free PSRAM</span>
        <span class="stat-value" id="sys-psram">--</span>
      </div>
      <div class="stat-row">
        <span class="stat-label">Vending</span>
        <span class="stat-value" id="sys-vending">--</span>
      </div>
    </div>
  </div>
</div>
<div class="footer">Smart Vending Machine &middot; ESP32-P4 + OV5647 + ESP32-C6 WiFi</div>

<script>
let streaming = false;
let pollTimer = null;

function formatUptime(ms) {
  const s = Math.floor(ms / 1000);
  const m = Math.floor(s / 60);
  const h = Math.floor(m / 60);
  if (h > 0) return h + 'h ' + (m % 60) + 'm';
  if (m > 0) return m + 'm ' + (s % 60) + 's';
  return s + 's';
}

function formatBytes(b) {
  if (b > 1048576) return (b / 1048576).toFixed(1) + ' MB';
  if (b > 1024) return (b / 1024).toFixed(1) + ' KB';
  return b + ' B';
}

function updateStatus() {
  fetch('/status').then(r => r.json()).then(d => {
    document.getElementById('conn-badge').textContent = 'ONLINE';
    document.getElementById('conn-badge').classList.remove('offline');

    document.getElementById('cam-sensor').textContent = d.camera.sensor;
    document.getElementById('cam-res').textContent = d.camera.resolution;
    document.getElementById('cam-format').textContent = d.camera.format;
    document.getElementById('cam-fps').textContent = d.camera.fps + ' fps';
    document.getElementById('cam-status').innerHTML =
      '<span class="status-dot ' + (d.camera.running ? 'green' : 'red') + '"></span>' +
      (d.camera.running ? 'Running' : 'Stopped');

    document.getElementById('det-model').textContent = d.detection.model;
    document.getElementById('det-fps').textContent = d.detection.fps + ' fps';
    document.getElementById('det-status').innerHTML =
      '<span class="status-dot ' + (d.detection.running ? 'green' : 'red') + '"></span>' +
      (d.detection.running ? 'Running' : 'Paused');
    document.getElementById('face-count').textContent = d.detection.faces;

    document.getElementById('sys-uptime').textContent = formatUptime(d.system.uptime_ms);
    document.getElementById('sys-heap').textContent = formatBytes(d.system.free_heap);
    document.getElementById('sys-psram').textContent = formatBytes(d.system.psram_free);
    document.getElementById('sys-vending').innerHTML = d.system.vending_active ?
      '<span class="stat-value warn">Active</span>' :
      '<span class="stat-value ok">Idle</span>';
  }).catch(() => {
    document.getElementById('conn-badge').textContent = 'OFFLINE';
    document.getElementById('conn-badge').classList.add('offline');
  });
}

function toggleStream() {
  const img = document.getElementById('stream-img');
  const btn = document.getElementById('btn-stream');
  if (streaming) {
    img.src = '';
    streaming = false;
    btn.textContent = 'Start Stream';
    btn.classList.remove('active');
    document.getElementById('stream-info').textContent = '--';
  } else {
    img.src = '/stream';
    streaming = true;
    btn.textContent = 'Stop Stream';
    btn.classList.add('active');
    document.getElementById('stream-info').textContent = 'LIVE';
  }
}

function takeSnapshot() {
  window.open('/snapshot', '_blank');
}

updateStatus();
pollTimer = setInterval(updateStatus, 2000);
</script>
</body>
</html>
)rawhtml";
