'use strict';

const $ = (id) => document.getElementById(id);
let CFG = null;
let bboxRect = null;      // L.rectangle for the selection
let gridLayer = null;     // L.layerGroup for grid cells
let drawing = false;
let dragStart = null;
let pollTimer = null;

const map = L.map('map', { zoomControl: true }).setView([32.30, 118.31], 11);
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
  maxZoom: 19,
  attribution: '© OpenStreetMap',
}).addTo(map);

// --- config ----------------------------------------------------------------
fetch('/api/config').then(r => r.json()).then(cfg => {
  CFG = cfg;
  const s = $('src-status');
  if (cfg.china_pbf_exists) {
    s.textContent = '就绪';
    s.className = 'v ok';
  } else {
    s.textContent = '缺失!';
    s.className = 'v bad';
  }
  s.title = cfg.china_pbf;
  $('region-km').value = cfg.default_region_km;
  refreshOutputs();
});

// --- bbox helpers ----------------------------------------------------------
function readBbox() {
  const w = parseFloat($('west').value), s = parseFloat($('south').value);
  const e = parseFloat($('east').value), n = parseFloat($('north').value);
  if ([w, s, e, n].some(v => Number.isNaN(v))) return null;
  return { west: Math.min(w, e), south: Math.min(s, n),
           east: Math.max(w, e), north: Math.max(s, n) };
}

function writeBbox(b) {
  $('west').value = b.west.toFixed(5);
  $('south').value = b.south.toFixed(5);
  $('east').value = b.east.toFixed(5);
  $('north').value = b.north.toFixed(5);
}

function drawBboxRect(b) {
  if (bboxRect) map.removeLayer(bboxRect);
  bboxRect = L.rectangle([[b.south, b.west], [b.north, b.east]], {
    color: '#2563eb', weight: 2, fillOpacity: 0.08,
  }).addTo(map);
}

function updateFromFields() {
  const b = readBbox();
  if (!b) return;
  drawBboxRect(b);
  updateGrid();
}

['west', 'south', 'east', 'north'].forEach(id =>
  $(id).addEventListener('change', updateFromFields));
$('region-km').addEventListener('change', updateGrid);

// --- grid preview ----------------------------------------------------------
function updateGrid() {
  const b = readBbox();
  const est = $('est');
  if (!b) { est.textContent = '—'; return; }
  const region_km = parseFloat($('region-km').value) || CFG.default_region_km;
  fetch('/api/grid', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ ...b, region_km }),
  }).then(r => r.json()).then(d => {
    if (gridLayer) map.removeLayer(gridLayer);
    gridLayer = L.layerGroup();
    (d.cells || []).forEach(c => {
      L.rectangle([[c.south, c.west], [c.north, c.east]], {
        color: '#f59e0b', weight: 1, fill: false, dashArray: '4 4',
      }).addTo(gridLayer);
    });
    gridLayer.addTo(map);
    const wkm = (b.east - b.west) * 111.32 * Math.cos(b.south * Math.PI / 180);
    const hkm = (b.north - b.south) * 111.32;
    est.innerHTML =
      `范围约 <b>${wkm.toFixed(1)} × ${hkm.toFixed(1)} km</b>，` +
      `覆盖 <b>${d.count}</b> 个全国网格 (region_km=${region_km})，` +
      `打包后约生成 <b>${d.count}</b> 个 x*_y*.vpk（lon*/lat* 分桶）。`;
    $('grid-status').textContent = `${d.count} 格 · ${region_km}km`;
  });
}

// --- draw on map -----------------------------------------------------------
$('draw-btn').addEventListener('click', () => {
  drawing = !drawing;
  $('draw-btn').classList.toggle('active', drawing);
  $('draw-btn').textContent = drawing ? '⏹ 拖拽框选中…' : '✏️ 在地图上框选';
  map.dragging[drawing ? 'disable' : 'enable']();
  map.getContainer().style.cursor = drawing ? 'crosshair' : '';
});

$('clear-btn').addEventListener('click', () => {
  if (bboxRect) { map.removeLayer(bboxRect); bboxRect = null; }
  if (gridLayer) { map.removeLayer(gridLayer); gridLayer = null; }
  ['west', 'south', 'east', 'north'].forEach(id => $(id).value = '');
  $('est').textContent = '—';
  $('grid-status').textContent = '—';
});

map.on('mousedown', (e) => {
  if (!drawing) return;
  dragStart = e.latlng;
});
map.on('mousemove', (e) => {
  if (!drawing || !dragStart) return;
  const b = {
    west: Math.min(dragStart.lng, e.latlng.lng),
    south: Math.min(dragStart.lat, e.latlng.lat),
    east: Math.max(dragStart.lng, e.latlng.lng),
    north: Math.max(dragStart.lat, e.latlng.lat),
  };
  drawBboxRect(b);
});
map.on('mouseup', (e) => {
  if (!drawing || !dragStart) return;
  const b = {
    west: Math.min(dragStart.lng, e.latlng.lng),
    south: Math.min(dragStart.lat, e.latlng.lat),
    east: Math.max(dragStart.lng, e.latlng.lng),
    north: Math.max(dragStart.lat, e.latlng.lat),
  };
  dragStart = null;
  writeBbox(b);
  drawBboxRect(b);
  updateGrid();
  // one-shot: exit draw mode after a selection
  drawing = false;
  $('draw-btn').classList.remove('active');
  $('draw-btn').textContent = '✏️ 在地图上框选';
  map.dragging.enable();
  map.getContainer().style.cursor = '';
});

// --- generate --------------------------------------------------------------
function selectedZooms() {
  return [...document.querySelectorAll('#zoom-chips input:checked')]
    .map(c => parseInt(c.value, 10));
}

function setGenProgress(pct, label) {
  const b = $('gen-btn');
  const p = Math.max(0, Math.min(100, pct | 0));
  b.style.background =
    `linear-gradient(to right, #2563eb 0%, #2563eb ${p}%, #475569 ${p}%)`;
  b.textContent = `${label || '生成中'} · ${p}%`;
}

function resetGenBtn() {
  const b = $('gen-btn');
  b.style.background = '';
  b.textContent = '🚀 生成 vmap';
  b.disabled = false;
}

$('gen-btn').addEventListener('click', () => {
  const b = readBbox();
  if (!b) { alert('请先框选或填写范围'); return; }
  const zooms = selectedZooms();
  if (!zooms.length) { alert('请至少选择一个 zoom'); return; }
  const name = $('name').value.trim() || `region_${Date.now()}`;
  const payload = {
    ...b, name, zooms,
    region_km: parseFloat($('region-km').value) || CFG.default_region_km,
    build_graph: $('build-graph').checked,
  };
  $('gen-btn').disabled = true;
  setGenProgress(0, '启动中');
  $('log-card').hidden = false;
  $('result-card').hidden = true;
  $('log').textContent = '';
  $('job-status').textContent = '启动中…';

  fetch('/api/generate', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  }).then(r => r.json()).then(d => {
    if (d.error) { throw new Error(d.error); }
    pollJob(d.job_id, 0);
  }).catch(err => {
    $('job-status').textContent = '错误: ' + err.message;
    resetGenBtn();
  });
});

function pollJob(jobId, from) {
  clearTimeout(pollTimer);
  fetch(`/api/job/${jobId}?from=${from}`).then(r => r.json()).then(d => {
    const logEl = $('log');
    if (d.log && d.log.length) {
      logEl.textContent += d.log.join('\n') + '\n';
      logEl.scrollTop = logEl.scrollHeight;
    }
    $('job-status').textContent = d.stage
      ? `${d.status} · ${d.stage}` : d.status;
    if (d.status === 'error') {
      setGenProgress(d.progress || 0, '失败');
      $('gen-btn').style.background =
        'linear-gradient(to right, #dc2626 0%, #dc2626 100%)';
      $('gen-btn').textContent = '❌ 失败，点击重试';
      $('gen-btn').disabled = false;
      refreshOutputs();
      return;
    }
    if (d.status === 'done') {
      setGenProgress(100, '完成');
      setTimeout(resetGenBtn, 1500);
      showResult(d.result);
      refreshOutputs();
      return;
    }
    setGenProgress(d.progress || 0, d.stage || '生成中');
    pollTimer = setTimeout(() => pollJob(jobId, d.log_len), 1000);
  }).catch(() => {
    pollTimer = setTimeout(() => pollJob(jobId, from), 2000);
  });
}

function showResult(r) {
  if (!r || !r.name) return;
  $('result-card').hidden = false;
  const maxWarn = r.vpk_max_kb > 700
    ? '<span class="badge warn">分片偏大</span>'
    : '<span class="badge ok">分片合规</span>';
  $('result-body').innerHTML = `
    <div>区域 <b>${r.name}</b></div>
    <div>zoom: ${r.zooms.join(', ')} · region_km=${r.region_km}
      · 导航图: ${r.graph ? '有' : '无'}</div>
    <div>瓦片 .vt: ${r.vt_count} 个 / ${r.vt_mb} MB
      （缓存命中 ${r.tiles_cached}/${r.tiles_required}，新建 ${r.tiles_built}）</div>
    <div>打包分片 x*_y*.vpk: <b>${r.vpk_count}</b> 个 ${maxWarn}</div>
    <div>最大分片: <b>${r.vpk_max_kb} KB</b> · 合计 ${r.vpk_total_kb} KB</div>
    <div>经纬度分桶: lonN/latN（每夹 ≤16 个小图）
      · 单文件夹最多 <b>${r.max_files_per_dir}</b> 个文件</div>
    <div style="margin-top:8px">上传目录 (MTP → /mnt/lfs/map):</div>
    <div><code>${r.map_dir}</code></div>
  `;
}

function refreshOutputs() {
  fetch('/api/outputs').then(r => r.json()).then(d => {
    const el = $('outputs');
    if (!d.outputs || !d.outputs.length) { el.textContent = '（暂无）'; return; }
    el.innerHTML = d.outputs.map(o =>
      `<div>• <b>${o.name}</b> — ${o.vpk_count} vpk, ${o.total_kb} KB<br>
        <code>${o.map_dir}</code></div>`).join('');
  });
}
