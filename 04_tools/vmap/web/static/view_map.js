/* global L */
"use strict";

const STYLE = {
  bg: "#eae6de",
  forest: "#c8d8c8",
  landRes: "#ecc8b8",
  landCom: "#e8c4c4",
  landEdu: "#ffe4b0",
  landPark: "#bad8ba",
  water: "#98bcdA",
  waterway: "#98bcdA",
  roadCase: "#a8a49c",
  road: [
    null,
    { fill: "#ffdc78", w: 5 },
    { fill: "#ffe8a0", w: 4 },
    { fill: "#f8f8fa", w: 3 },
    { fill: "#f2f2f6", w: 3 },
    { fill: "#ececf0", w: 2 },
    { fill: "#e4e4e8", w: 2 },
    { fill: "#d8d8dc", w: 1 },
  ],
  lbl: { 1: "#000", 2: "#14468c", 3: "#000", 4: "#145f2d" },
};

const FLAG_CLOSED = 0x01;
const FLAG_NAMED = 0x02;
const LAYER_WATER = 0, LAYER_FOREST = 1, LAYER_WATERWAY = 2, LAYER_ROAD = 3, LAYER_LABEL = 4, LAYER_LAND = 5;
const LAND_RES = 1, LAND_COM = 2, LAND_IND = 3, LAND_EDU = 4, LAND_PARK = 5;

const decoder = new TextDecoder("utf-8");
const $ = (id) => document.getElementById(id);

function magic(buf, off) {
  return decoder.decode(new Uint8Array(buf, off, 4));
}

function padId(n, w) {
  return String(n).padStart(w, "0");
}

const GRID_ORIGIN_LON = 0;
const GRID_ORIGIN_LAT = 0;
const GRID_CELL_LAT = 0.026949335249730506;
const GRID_CELL_LON = 0.032899063849730509;
const GRID_BUCKET = 4;

/* 默认视野 = 内置演示包（04_tools/vmap/map，北京 8x8 = 64 块）的中心：
   x3534..x3541 / y1476..y1483 并集的中心，即 (3538*cellLon, 1480*cellLat)。
   加载完 catalog 后会按实际包重新铺满，这里只是加载前的兜底。 */
const DEMO_CENTER = [39.885010, 116.396890];
/* 包不大就自动铺满视野；全国包有几千块，缩到全国没意义（那也是老的
   "默认广州 + 手动跳转" 想规避的情况）。 */
const FIT_MAX_CELLS = 512;

function cellLabel(r) {
  if (r && r.ix != null && r.iy != null) return `x${r.ix}_y${r.iy}`;
  return `r${padId(r.id, 3)}`;
}

function vpkRelGrid(ix, iy) {
  const bx = Math.floor(ix / GRID_BUCKET);
  const by = Math.floor(iy / GRID_BUCKET);
  return `lon${bx}/lat${by}/x${ix}_y${iy}.vpk`;
}

function vpkRel(region, subdir) {
  if (region && region.vpk) return region.vpk;
  if (region && region.ix != null && region.iy != null) {
    return vpkRelGrid(region.ix, region.iy);
  }
  const rid = typeof region === "number" ? region : region.id;
  const name = `r${padId(rid, 3)}.vpk`;
  if (subdir) return `d${padId(Math.floor(rid / 20), 3)}/${name}`;
  return name;
}

function parseVreg(buf) {
  const dv = new DataView(buf);
  if (magic(buf, 0) !== "VREG") throw new Error("不是 VREG map.idx");
  const n = dv.getUint16(6, true);
  const originLon = dv.getFloat64(8, true);
  const originLat = dv.getFloat64(16, true);
  const cellLon = dv.getFloat64(24, true);
  const cellLat = dv.getFloat64(32, true);
  const regions = [];
  let o = 40;
  for (let i = 0; i < n; i++) {
    regions.push({
      id: dv.getUint16(o, true),
      ix: dv.getUint16(o + 2, true),
      iy: dv.getUint16(o + 4, true),
      neighbors: dv.getUint16(o + 6, true),
      west: dv.getFloat64(o + 8, true),
      south: dv.getFloat64(o + 16, true),
      east: dv.getFloat64(o + 24, true),
      north: dv.getFloat64(o + 32, true),
    });
    o += 48;
  }
  return { originLon, originLat, cellLon, cellLat, regions };
}

function parseRidx(payload) {
  const dv = new DataView(payload);
  if (magic(payload, 0) !== "RIDX") throw new Error("vpk 内不是 RIDX");
  const flags = dv.getUint8(5);
  const rid = dv.getUint16(6, true);
  const tileCount = dv.getUint32(8, true);
  const graphOff = dv.getUint32(12, true);
  const graphSz = dv.getUint32(16, true);
  const west = dv.getFloat64(20, true);
  const south = dv.getFloat64(28, true);
  const east = dv.getFloat64(36, true);
  const north = dv.getFloat64(44, true);
  const ver = dv.getUint8(4);
  const recSize = ver >= 2 ? 16 : 14;
  const tiles = [];
  let o = 52;
  for (let i = 0; i < tileCount; i++) {
    tiles.push({
      z: dv.getUint8(o),
      x: dv.getUint16(o + 2, true),
      y: dv.getUint16(o + 4, true),
      offset: dv.getUint32(o + 8, true),
      size: ver >= 2 ? dv.getUint32(o + 12, true) : dv.getUint16(o + 12, true),
    });
    o += recSize;
  }
  return { flags, rid, tiles, graphOff, graphSz, west, south, east, north };
}

function parseVtil(buf) {
  const dv = new DataView(buf);
  if (magic(buf, 0) !== "VTIL") throw new Error("不是 VTIL");
  const z = dv.getUint8(5);
  const extent = dv.getUint16(6, true);
  const tx = dv.getUint32(8, true);
  const ty = dv.getUint32(12, true);
  const nlay = dv.getUint16(16, true);
  let o = 20;
  const geom = { 0: [], 1: [], 2: [], 3: [], 5: [] };
  const labels = [];
  for (let i = 0; i < nlay; i++) {
    const lid = dv.getUint8(o);
    const nfeat = dv.getUint16(o + 2, true);
    const blobLen = dv.getUint32(o + 4, true);
    o += 8;
    const end = o + blobLen;
    if (lid === LAYER_LABEL) {
      for (let j = 0; j < nfeat && o < end; j++) {
        const kind = dv.getUint8(o);
        const prio = dv.getUint8(o + 1);
        const x = dv.getUint16(o + 2, true);
        const y = dv.getUint16(o + 4, true);
        const len = dv.getUint8(o + 6);
        o += 8;
        const name = decoder.decode(new Uint8Array(buf, o, len));
        o += len;
        labels.push({ kind, prio, x, y, name });
      }
    } else {
      const list = geom[lid] || (geom[lid] = []);
      for (let j = 0; j < nfeat && o < end; j++) {
        const attr = dv.getUint8(o);
        const flags = dv.getUint8(o + 1);
        const npts = dv.getUint16(o + 2, true);
        o += 4;
        const pts = new Array(npts);
        for (let k = 0; k < npts; k++) {
          pts[k] = [dv.getUint16(o, true), dv.getUint16(o + 2, true)];
          o += 4;
        }
        let name = null;
        if (flags & FLAG_NAMED) {
          const ln = dv.getUint8(o);
          o += 1;
          name = decoder.decode(new Uint8Array(buf, o, ln));
          o += ln;
        }
        list.push({ attr, flags, pts, name });
      }
    }
    o = end;
  }
  return { z, extent, tx, ty, geom, labels };
}

function parsePort(buf) {
  if (magic(buf, 0) !== "PORT") return [];
  const dv = new DataView(buf);
  const ver = dv.getUint8(4);
  const pts = [];
  if (ver >= 3) {
    const recCount = dv.getUint32(6, true);
    const pairCount = dv.getUint32(10, true);
    let o = 14 + pairCount * 16;
    for (let i = 0; i < recCount && o + 32 <= buf.byteLength && pts.length < 4000; i++) {
      pts.push({
        a: dv.getUint32(o, true),
        b: dv.getUint32(o + 4, true),
        lon: dv.getFloat64(o + 16, true),
        lat: dv.getFloat64(o + 24, true),
      });
      o += 32;
    }
  } else if (ver >= 2) {
    const recCount = dv.getUint32(6, true);
    const pairCount = dv.getUint32(10, true);
    let o = 14 + pairCount * 12;
    for (let i = 0; i < recCount; i++) {
      pts.push({
        a: dv.getUint16(o, true),
        b: dv.getUint16(o + 2, true),
        lon: dv.getFloat64(o + 12, true),
        lat: dv.getFloat64(o + 20, true),
      });
      o += 28;
    }
  } else {
    const recCount = dv.getUint32(6, true);
    let o = 10;
    for (let i = 0; i < recCount; i++) {
      pts.push({
        a: dv.getUint16(o, true),
        b: dv.getUint16(o + 2, true),
        lon: dv.getFloat64(o + 12, true),
        lat: dv.getFloat64(o + 20, true),
      });
      o += 28;
    }
  }
  return pts;
}

function parseVpor(payload, graphOff, graphSz, selfId) {
  const o0 = graphOff + graphSz;
  if (o0 + 8 > payload.byteLength) return [];
  if (magic(payload, o0) !== "VPOR") return [];
  const dv = new DataView(payload);
  const nNbr = dv.getUint16(o0 + 6, true);
  const recBase = o0 + 8 + nNbr * 8;
  const pts = [];
  for (let i = 0; i < nNbr; i++) {
    const e = o0 + 8 + i * 8;
    const nbr = dv.getUint32(e, true);
    const first = dv.getUint16(e + 4, true);
    const count = dv.getUint16(e + 6, true);
    if (selfId > nbr) continue;
    for (let j = 0; j < count; j++) {
      const r = recBase + (first + j) * 24;
      if (r + 24 > payload.byteLength) break;
      pts.push({
        a: selfId,
        b: nbr,
        lon: dv.getFloat64(r + 8, true),
        lat: dv.getFloat64(r + 16, true),
      });
    }
  }
  return pts;
}

const ELE_UNKNOWN = -32768;

function snapSectionEnd(buf, snapOff) {
  const dv = new DataView(buf);
  if (snapOff < 0 || snapOff + 20 > buf.byteLength) return snapOff;
  if (magic(buf, snapOff) !== "SNAP") return snapOff;
  const ncol = dv.getUint16(snapOff + 16, true);
  const nrow = dv.getUint16(snapOff + 18, true);
  const ncells = ncol * nrow;
  const off = snapOff + 20;
  if (off + (ncells + 1) * 4 > buf.byteLength) return snapOff;
  const nList = dv.getUint32(off + ncells * 4, true);
  const end = off + (ncells + 1) * 4 + nList * 4;
  return end <= buf.byteLength ? end : snapOff;
}

function vgrfEdgeStride(ver) {
  return ver >= 3 ? 18 : 16;
}

function vgrfHeader(buf) {
  if (!buf || buf.byteLength < 16 || magic(buf, 0) !== "VGRF") return null;
  const dv = new DataView(buf);
  return {
    ver: dv.getUint8(4),
    n: dv.getUint32(8, true),
    ecount: dv.getUint32(12, true),
  };
}

function parseVgrf(buf) {
  if (!buf || buf.byteLength < 48 || magic(buf, 0) !== "VGRF") return null;
  const dv = new DataView(buf);
  const ver = dv.getUint8(4);
  const n = dv.getUint32(8, true);
  const ecount = dv.getUint32(12, true);
  const west = dv.getFloat64(16, true);
  const south = dv.getFloat64(24, true);
  const east = dv.getFloat64(32, true);
  const north = dv.getFloat64(40, true);
  const hdr = ver >= 2 ? 52 : 48;
  const snapOff = ver >= 2 ? dv.getUint32(48, true) : 0;
  const edgeStride = vgrfEdgeStride(ver);
  let o = hdr;
  if (n > 4e6 || ecount > 8e6) return null;
  if (o + n * 8 + ecount * edgeStride > buf.byteLength) return null;
  const lon = new Float64Array(n);
  const lat = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    lon[i] = dv.getInt32(o, true) / 1e7;
    lat[i] = dv.getInt32(o + 4, true) / 1e7;
    o += 8;
  }
  const ea = new Uint32Array(ecount);
  const eb = new Uint32Array(ecount);
  const elen = new Uint32Array(ecount);
  const ecls = new Uint8Array(ecount);
  const eflags = new Uint8Array(ecount);
  const deg = new Uint16Array(n);
  for (let i = 0; i < ecount; i++) {
    const a = dv.getUint32(o, true);
    const b = dv.getUint32(o + 4, true);
    ea[i] = a;
    eb[i] = b;
    elen[i] = dv.getUint32(o + 8, true);
    ecls[i] = dv.getUint8(o + 12);
    eflags[i] = dv.getUint8(o + 13);
    if (a < n) deg[a]++;
    if (b < n) deg[b]++;
    o += edgeStride;
  }
  let ele = null;
  if (ver >= 2 && snapOff > 0) {
    const end = snapSectionEnd(buf, snapOff);
    if (end + 12 <= buf.byteLength && magic(buf, end) === "ELEV") {
      const count = dv.getUint32(end + 8, true);
      if (count === n && end + 12 + count * 2 <= buf.byteLength) {
        ele = new Int16Array(buf.slice(end + 12, end + 12 + count * 2));
      }
    }
  }
  return { west, south, east, north, n, ecount, lon, lat, ea, eb, elen, ecls, eflags, ele, deg };
}

function graphEdgeColor(cls, flags) {
  if (flags & 2) return "rgba(148,163,184,0.9)";
  const pal = [null, "#7c3aed", "#2563eb", "#0891b2", "#0d9488", "#4d7c0f", "#a16207", "#57534e"];
  return pal[cls] || "#0d9488";
}

function graphEdgeWidth(cls, z) {
  const bump = z >= 15 ? 1 : 0;
  if (cls <= 2) return 4 + bump;
  if (cls <= 4) return 3 + bump;
  if (cls <= 6) return 2;
  return 1.5;
}

function eleColor(m, minE, maxE) {
  if (m <= ELE_UNKNOWN) return "#888888";
  const t = maxE > minE ? Math.max(0, Math.min(1, (m - minE) / (maxE - minE))) : 0.5;
  const h = 220 - t * 220;
  return `hsl(${h}, 82%, 42%)`;
}

function lonLatToTilePx(lon, lat, z, tx, ty) {
  const n = 2 ** z;
  const x = (lon + 180) / 360 * n;
  const s = Math.sin(lat * Math.PI / 180);
  const y = (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * n;
  return [(x - tx) * 256, (y - ty) * 256];
}

function pathMid(pts) {
  const segs = [];
  let total = 0;
  for (let i = 0; i < pts.length - 1; i++) {
    const dx = pts[i + 1][0] - pts[i][0];
    const dy = pts[i + 1][1] - pts[i][1];
    const d = Math.hypot(dx, dy);
    segs.push({ d, i, dx, dy });
    total += d;
  }
  if (!segs.length) {
    return { x: pts[0][0], y: pts[0][1], ang: 0, len: 0 };
  }
  let acc = 0;
  const target = total / 2;
  for (const seg of segs) {
    if (acc + seg.d >= target || seg === segs[segs.length - 1]) {
      const t = seg.d ? Math.min(1, Math.max(0, (target - acc) / seg.d)) : 0;
      const a = pts[seg.i];
      const b = pts[seg.i + 1];
      let ang = Math.atan2(seg.dy, seg.dx);
      if (ang > Math.PI / 2) ang -= Math.PI;
      if (ang < -Math.PI / 2) ang += Math.PI;
      return {
        x: a[0] + (b[0] - a[0]) * t,
        y: a[1] + (b[1] - a[1]) * t,
        ang,
        len: total,
      };
    }
    acc += seg.d;
  }
  return { x: pts[0][0], y: pts[0][1], ang: 0, len: total };
}

function drawRoadNames(ctx, roads, s) {
  if (!roads) return;
  const named = roads.filter((f) => {
    const pts = f.spts || f.pts;
    return f.name && pts && pts.length >= 2;
  }).sort((a, b) => a.attr - b.attr);
  const placed = [];
  ctx.font = "11px system-ui, 'PingFang SC', 'Microsoft YaHei', sans-serif";
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  for (const f of named) {
    const m = pathMid(f.spts || f.pts);
    if (m.len * s < 8) continue;
    let clash = false;
    for (const p of placed) {
      if (p.name === f.name && Math.hypot(p.x - m.x, p.y - m.y) * s < 36) {
        clash = true;
        break;
      }
    }
    if (clash) continue;
    placed.push({ name: f.name, x: m.x, y: m.y });
    ctx.save();
    ctx.translate(m.x * s, m.y * s);
    ctx.rotate(m.ang);
    ctx.lineWidth = 3;
    ctx.strokeStyle = "rgba(255,255,255,0.92)";
    ctx.strokeText(f.name, 0, 0);
    ctx.fillStyle = "#1a1a1a";
    ctx.fillText(f.name, 0, 0);
    ctx.restore();
  }
}

function keepPointLabel(lb) {
  if (!lb || !lb.name) return false;
  if (lb.kind === 2) return true;
  if (lb.kind === 4 || lb.kind === 1) {
    return /公园|湿地|花博|森林|林场|植物园|风景名胜|自然保护区/.test(lb.name);
  }
  return false;
}

function makeLoaderFromHttp() {
  return {
    label: "服务端 /map",
    async get(rel) {
      const r = await fetch("/map/" + rel);
      if (!r.ok) throw new Error(`${rel} HTTP ${r.status}`);
      return r.arrayBuffer();
    },
    async try(rel) {
      const r = await fetch("/map/" + rel);
      if (!r.ok) return null;
      return r.arrayBuffer();
    },
  };
}

function makeLoaderFromFiles(fileList) {
  const map = new Map();
  let root = "";
  const keys = [];
  for (const f of fileList) {
    const rel = (f.webkitRelativePath || f.name).replace(/\\/g, "/");
    map.set(rel, f);
    keys.push(rel);
  }
  for (const rel of keys) {
    if (rel.endsWith("map.idx")) {
      const i = rel.lastIndexOf("/");
      root = i >= 0 ? rel.slice(0, i + 1) : "";
      break;
    }
  }
  if (!root) {
    for (const rel of keys) {
      const m = rel.match(/^(.*?)lon\d+\/lat\d+\/x\d+_y\d+\.vpk$/i)
        || rel.match(/^(.*?)x\d+\/y\d+\.vpk$/i);
      if (m) {
        root = m[1];
        break;
      }
    }
  }
  const hasIdx = keys.some((k) => k === root + "map.idx");
  const hasGrid = keys.some((k) =>
    /(?:^|\/)(?:lon\d+\/lat\d+\/x\d+_y\d+|x\d+\/y\d+)\.vpk$/i.test(k));
  if (!hasIdx && !hasGrid) {
    throw new Error("文件夹里没有 lon*/lat*/x*_y*.vpk 也没有 map.idx");
  }
  const getFile = (rel) => {
    const f = map.get(root + rel) || map.get(rel);
    if (!f) throw new Error("缺少 " + rel);
    return f.arrayBuffer();
  };
  return {
    label: root.replace(/\/$/, "") || "本地文件夹",
    keys,
    root,
    get: getFile,
    try: async (rel) => {
      const f = map.get(root + rel) || map.get(rel);
      return f ? f.arrayBuffer() : null;
    },
  };
}

function catalogFromGridKeys(keys, root) {
  const regions = [];
  const prefix = root || "";
  for (const k of keys) {
    const rel = prefix && k.startsWith(prefix) ? k.slice(prefix.length) : k;
    const m = rel.match(/^(?:lon\d+\/lat\d+\/)?x(\d+)_y(\d+)\.vpk$/i)
      || rel.match(/^x(\d+)\/y(\d+)\.vpk$/i);
    if (!m) continue;
    const ix = Number(m[1]);
    const iy = Number(m[2]);
    const west = GRID_ORIGIN_LON + ix * GRID_CELL_LON;
    const south = GRID_ORIGIN_LAT + iy * GRID_CELL_LAT;
    regions.push({
      id: ((iy << 16) | ix) >>> 0,
      ix,
      iy,
      neighbors: 0,
      west,
      south,
      east: west + GRID_CELL_LON,
      north: south + GRID_CELL_LAT,
      vpk: rel,
    });
  }
  return {
    originLon: GRID_ORIGIN_LON,
    originLat: GRID_ORIGIN_LAT,
    cellLon: GRID_CELL_LON,
    cellLat: GRID_CELL_LAT,
    regions,
  };
}

const state = {
  loader: null,
  catalog: null,
  tileIndex: new Map(),
  vpkMeta: new Map(),
  portals: [],
  portalKeys: new Set(),
  graphs: [],
  graphBlobs: [],
  eleMin: 0,
  eleMax: 0,
  bytes: 0,
  map: null,
  osm: null,
  vecLayer: null,
  elevLayer: null,
  graphLayer: null,
  gridLayer: null,
  portLayer: null,
  selected: null,
  parsedTiles: new Map(),
};

function scanVisibleInventory() {
  const landBy = { 1: 0, 2: 0, 3: 0, 4: 0, 5: 0 };
  let land = 0;
  const poi = new Set(), waterN = new Set(), roadN = new Set();
  for (const tile of state.parsedTiles.values()) {
    for (const f of (tile.geom[LAYER_LAND] || [])) {
      land++;
      landBy[f.attr] = (landBy[f.attr] || 0) + 1;
    }
    for (const f of (tile.geom[LAYER_ROAD] || [])) {
      if (f.name) roadN.add(f.name);
    }
    for (const lb of tile.labels) {
      if (!keepPointLabel(lb)) continue;
      if (lb.kind === 2) waterN.add(lb.name);
      else poi.add(lb.name);
    }
  }
  if ($("st-land")) {
    $("st-land").textContent = state.parsedTiles.size
      ? `${land}（住${landBy[1] || 0} 商${(landBy[2] || 0) + (landBy[3] || 0)} ` +
        `校${landBy[4] || 0} 园${landBy[5] || 0}）· 当前视野`
      : "缩放后按视野统计";
  }
  if ($("st-names")) {
    $("st-names").textContent = state.parsedTiles.size
      ? `路${roadN.size} 公园/林${poi.size} 水${waterN.size} · 当前视野`
      : "缩放后按视野统计";
  }
}

function neighText(mask) {
  const p = [];
  if (mask & 1) p.push("W");
  if (mask & 2) p.push("E");
  if (mask & 4) p.push("S");
  if (mask & 8) p.push("N");
  return p.join("") || "无";
}

function setSel(r) {
  state.selected = r;
  const meta = state.vpkMeta.get(r.id);
  $("sel-body").innerHTML =
    `<div><code>${r.vpk || vpkRel(r)}</code> · 网格 (${r.ix},${r.iy})</div>` +
    `<div>bbox ${r.west.toFixed(4)}..${r.east.toFixed(4)} E，` +
    `${r.south.toFixed(4)}..${r.north.toFixed(4)} N</div>` +
    `<div>瓦片 ${meta ? meta.tiles : "?"} · 图 ${meta ? (meta.graphSz / 1024).toFixed(0) : "?"} KB</div>` +
    `<div>邻接 ${neighText(r.neighbors)}</div>`;
  for (const btn of document.querySelectorAll("#region-list button")) {
    btn.classList.toggle("active", Number(btn.dataset.id) === r.id);
  }
}

function fillList(regions) {
  const box = $("region-list");
  box.innerHTML = "";
  for (const r of regions) {
    const b = document.createElement("button");
    b.dataset.id = String(r.id);
    const meta = state.vpkMeta.get(r.id);
    b.textContent = `${cellLabel(r)}  ${r.west.toFixed(2)},${r.south.toFixed(2)}  ` +
      `${meta ? meta.tiles + " tiles" : ""}`;
    b.addEventListener("click", () => {
      setSel(r);
      state.map.fitBounds([[r.south, r.west], [r.north, r.east]], { maxZoom: 14 });
    });
    box.appendChild(b);
  }
}

function parsedVtil(key, rec) {
  let tile = state.parsedTiles.get(key);
  if (tile) return tile;
  const slice = rec.buf.slice(16 + rec.offset, 16 + rec.offset + rec.size);
  tile = parseVtil(slice);
  state.parsedTiles.set(key, tile);
  if (state.parsedTiles.size > 400) {
    const oldest = state.parsedTiles.keys().next().value;
    if (oldest !== key) state.parsedTiles.delete(oldest);
  }
  return tile;
}

function makeTileXform(map, tile) {
  const n = 2 ** tile.z;
  const tx = tile.tx, ty = tile.ty, extent = tile.extent || 4096;
  return (lx, ly) => {
    const lon = ((tx + lx / extent) / n) * 360 - 180;
    const lat = Math.atan(Math.sinh(Math.PI * (1 - 2 * (ty + ly / extent) / n)))
      * 180 / Math.PI;
    const p = map.latLngToContainerPoint([lat, lon]);
    return [p.x, p.y];
  };
}

function screenifyFeats(xf, feats) {
  return (feats || []).map((f) => ({
    attr: f.attr,
    flags: f.flags,
    name: f.name,
    spts: f.pts.map((p) => xf(p[0], p[1])),
  }));
}

function fillScreenPolys(ctx, feats, colorOf) {
  for (const f of feats) {
    const pts = f.spts;
    if (!pts || pts.length < 3) continue;
    ctx.beginPath();
    ctx.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
    ctx.closePath();
    ctx.fillStyle = typeof colorOf === "function" ? colorOf(f) : colorOf;
    ctx.fill();
  }
}

function strokeScreen(ctx, feats, color, width) {
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  for (const f of feats) {
    const pts = f.spts;
    if (!pts || pts.length < 2) continue;
    ctx.beginPath();
    ctx.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i][0], pts[i][1]);
    if (f.flags & FLAG_CLOSED) ctx.closePath();
    ctx.stroke();
  }
}

const VtilCanvasLayer = L.Layer.extend({
  onAdd(map) {
    this._map = map;
    if (!map.getPane("vecPane")) {
      map.createPane("vecPane");
    }
    const pane = map.getPane("vecPane");
    pane.style.zIndex = 450;
    pane.style.pointerEvents = "none";
    this._canvas = L.DomUtil.create("canvas", "vmap-vec-canvas");
    pane.appendChild(this._canvas);
    map.on("move zoom viewreset resize", this._redraw, this);
    this._redraw();
  },
  onRemove(map) {
    map.off("move zoom viewreset resize", this._redraw, this);
    if (this._canvas) {
      L.DomUtil.remove(this._canvas);
      this._canvas = null;
    }
  },
  _redraw() {
    const map = this._map;
    const canvas = this._canvas;
    if (!map || !canvas) return;
    const size = map.getSize();
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(size.x * dpr));
    canvas.height = Math.max(1, Math.round(size.y * dpr));
    canvas.style.width = size.x + "px";
    canvas.style.height = size.y + "px";
    L.DomUtil.setPosition(canvas, map.containerPointToLayerPoint([0, 0]));
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, size.x, size.y);
    if (!$("tog-osm").checked) {
      ctx.fillStyle = STYLE.bg;
      ctx.fillRect(0, 0, size.x, size.y);
    }
    const showLand = !$("tog-land") || $("tog-land").checked;
    const showRoads = !$("tog-elev") || !$("tog-elev").checked;
    const showLabels = $("tog-lbl").checked;
    const bounds = map.getBounds().pad(0.12);
    const landFill = {
      [LAND_COM]: STYLE.landCom,
      [LAND_IND]: STYLE.landCom,
      [LAND_EDU]: STYLE.landEdu,
      [LAND_PARK]: STYLE.landPark,
    };
    const screens = [];
    const mapZ = map.getZoom();
    for (const [key, rec] of state.tileIndex) {
      const [zs, xs, ys] = key.split("/");
      const z = Number(zs), x = Number(xs), y = Number(ys);
      const tilePx = 256 * Math.pow(2, mapZ - z);
      if (tilePx < 8) continue;
      const n = 2 ** z;
      const west = x / n * 360 - 180;
      const east = (x + 1) / n * 360 - 180;
      const north = Math.atan(Math.sinh(Math.PI * (1 - 2 * y / n))) * 180 / Math.PI;
      const south = Math.atan(Math.sinh(Math.PI * (1 - 2 * (y + 1) / n))) * 180 / Math.PI;
      if (east < bounds.getWest() || west > bounds.getEast()
        || north < bounds.getSouth() || south > bounds.getNorth()) {
        continue;
      }
      let tile;
      try {
        tile = parsedVtil(key, rec);
      } catch (e) {
        continue;
      }
      const xf = makeTileXform(map, tile);
      screens.push({
        tx: tile.tx, ty: tile.ty,
        land: showLand ? screenifyFeats(xf, tile.geom[LAYER_LAND]) : [],
        forest: screenifyFeats(xf, tile.geom[LAYER_FOREST]),
        water: screenifyFeats(xf, tile.geom[LAYER_WATER]),
        waterway: screenifyFeats(xf, tile.geom[LAYER_WATERWAY]),
        road: screenifyFeats(xf, tile.geom[LAYER_ROAD]),
        labels: (tile.labels || []).map((lb) => {
          const p = xf(lb.x, lb.y);
          return { kind: lb.kind, prio: lb.prio, name: lb.name, sx: p[0], sy: p[1] };
        }),
      });
    }
    screens.sort((a, b) => (b.tx - a.tx) || (b.ty - a.ty));
    for (const t of screens) {
      fillScreenPolys(ctx, t.land, (f) => landFill[f.attr] || STYLE.landRes);
    }
    for (const t of screens) {
      fillScreenPolys(ctx, t.forest, STYLE.forest);
    }
    for (const t of screens) {
      fillScreenPolys(ctx, t.water, STYLE.water);
    }
    for (const t of screens) {
      strokeScreen(ctx, t.waterway, STYLE.waterway, 1.5);
    }
    if (showRoads) {
      for (let cls = 7; cls >= 1; cls--) {
        const st = STYLE.road[cls];
        if (!st) continue;
        for (const t of screens) {
          strokeScreen(ctx, t.road.filter((f) => f.attr === cls),
            STYLE.roadCase, st.w + 1.2);
        }
        for (const t of screens) {
          strokeScreen(ctx, t.road.filter((f) => f.attr === cls), st.fill, st.w);
        }
      }
    }
    if (showLabels) {
      const allRoads = [];
      for (const t of screens) allRoads.push(...t.road);
      drawRoadNames(ctx, allRoads, 1);
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      const placed = [];
      const labels = [];
      for (const t of screens) labels.push(...t.labels);
      labels.sort((a, b) => b.prio - a.prio);
      for (const lb of labels) {
        if (!keepPointLabel(lb)) continue;
        const x = lb.sx, y = lb.sy;
        if (x < -40 || y < -20 || x > size.x + 40 || y > size.y + 20) continue;
        let clash = false;
        for (const p of placed) {
          if (p.name === lb.name && Math.hypot(p.x - x, p.y - y) < 22) {
            clash = true;
            break;
          }
        }
        if (clash) continue;
        placed.push({ name: lb.name, x, y });
        const poi = lb.kind === 4;
        ctx.font = (poi ? "12px" : "11px")
          + " system-ui, 'PingFang SC', 'Microsoft YaHei', sans-serif";
        ctx.lineWidth = poi ? 3.5 : 3;
        ctx.strokeStyle = "rgba(255,255,255,0.92)";
        ctx.strokeText(lb.name, x, y);
        ctx.fillStyle = STYLE.lbl[lb.kind] || "#145f2d";
        ctx.fillText(lb.name, x, y);
      }
    }
    scanVisibleInventory();
  },
});

async function loadRegionVpk(region, preferSubdir) {
  if (state.vpkMeta.has(region.id)) return;
  state.vpkMeta.set(region.id, { loading: true });
  const rels = preferSubdir
    ? [vpkRel(region, true), vpkRel(region, false)]
    : [vpkRel(region, false), vpkRel(region, true)];
  let buf = null, rel = null;
  for (const c of rels) {
    buf = await state.loader.try(c);
    if (buf) { rel = c; break; }
  }
  if (!buf) throw new Error("找不到 " + rels[0]);
  state.bytes += buf.byteLength;
  const payload = buf.slice(16);
  if (magic(buf, 0) !== "VPKG") throw new Error(rel + " 不是 VPKG");
  const ridx = parseRidx(payload);
  state.vpkMeta.set(region.id, {
    rel, tiles: ridx.tiles.length, graphSz: ridx.graphSz, flags: ridx.flags,
  });
  if (ridx.graphSz > 0 && ridx.graphOff + ridx.graphSz <= payload.byteLength) {
    state.graphBlobs.push({
      rid: region.id,
      west: ridx.west,
      south: ridx.south,
      east: ridx.east,
      north: ridx.north,
      graphSz: ridx.graphSz,
      src: buf,
      off: 16 + ridx.graphOff,
      sz: ridx.graphSz,
    });
    for (const p of parseVpor(payload, ridx.graphOff, ridx.graphSz, region.id)) {
      const key = `${p.a}:${p.b}:${p.lon.toFixed(5)}:${p.lat.toFixed(5)}`;
      if (state.portalKeys.has(key)) continue;
      state.portalKeys.add(key);
      state.portals.push(p);
    }
  }
  for (const t of ridx.tiles) {
    const key = `${t.z}/${t.x}/${t.y}`;
    if (!state.tileIndex.has(key)) {
      state.tileIndex.set(key, { rid: region.id, offset: t.offset, size: t.size, buf });
    }
  }
}

function graphBlobInView(blob, bounds) {
  return !(blob.east < bounds.getWest() || blob.west > bounds.getEast()
    || blob.north < bounds.getSouth() || blob.south > bounds.getNorth());
}

function ensureGraph(blob) {
  if (Object.prototype.hasOwnProperty.call(blob, "parsed")) return blob.parsed;
  try {
    blob.parsed = parseVgrf(blob.src.slice(blob.off, blob.off + blob.sz));
  } catch (e) {
    console.warn("parseVgrf r" + blob.rid, e);
    blob.parsed = null;
  }
  return blob.parsed;
}

function graphsInView(bounds) {
  const out = [];
  for (const blob of state.graphBlobs) {
    if (!graphBlobInView(blob, bounds)) continue;
    const g = ensureGraph(blob);
    if (g) out.push(g);
  }
  return out;
}

function rebuildOverlays() {
  if (state.gridLayer) {
    state.map.removeLayer(state.gridLayer);
    state.gridLayer = null;
  }
  if (state.portLayer) {
    state.map.removeLayer(state.portLayer);
    state.portLayer = null;
  }
  if ($("tog-grid") && $("tog-grid").checked && state.catalog) {
    state.gridLayer = L.layerGroup();
    const zoom = state.map.getZoom() || 0;
    const bounds = state.map.getBounds().pad(0.15);
    const vis = [];
    for (const r of state.catalog.regions) {
      if (r.east < bounds.getWest() || r.west > bounds.getEast()
        || r.north < bounds.getSouth() || r.south > bounds.getNorth()) continue;
      vis.push(r);
      if (vis.length > 400) break;
    }
    const showName = zoom >= 12 && vis.length <= 40;
    for (const r of vis) {
      const rect = L.rectangle([[r.south, r.west], [r.north, r.east]], {
        color: "#ea580c", weight: 1, dashArray: "6 4", fill: true,
        fillColor: "#ea580c", fillOpacity: 0.04,
      });
      rect.on("click", () => setSel(r));
      if (showName) {
        rect.bindTooltip(cellLabel(r), {
          permanent: true, direction: "center", className: "vmap-grid-label",
        });
      }
      state.gridLayer.addLayer(rect);
    }
    state.gridLayer.addTo(state.map);
  }
  if ($("tog-port") && $("tog-port").checked && state.portals.length) {
    state.portLayer = L.layerGroup();
    const bounds = state.map.getBounds().pad(0.25);
    let n = 0;
    for (const p of state.portals) {
      if (p.lon < bounds.getWest() || p.lon > bounds.getEast()
        || p.lat < bounds.getSouth() || p.lat > bounds.getNorth()) continue;
      L.circleMarker([p.lat, p.lon], {
        radius: 3, color: "#7c3aed", weight: 1, fillOpacity: 0.8,
      }).bindTooltip(`portal ${p.a}–${p.b}`).addTo(state.portLayer);
      if (++n > 2000) break;
    }
    state.portLayer.addTo(state.map);
  }
}

function refreshVec() {
  if (state.vecLayer) {
    state.map.removeLayer(state.vecLayer);
    state.vecLayer = null;
  }
  if (!$("tog-vec").checked) return;
  state.vecLayer = new VtilCanvasLayer();
  state.vecLayer.addTo(state.map);
}

function percentile(sorted, p) {
  if (!sorted.length) return 0;
  const i = Math.max(0, Math.min(sorted.length - 1, Math.round((sorted.length - 1) * p)));
  return sorted[i];
}

function summarizeGraph() {
  let nodes = 0, edges = 0, failed = 0;
  for (const blob of state.graphBlobs) {
    if (blob.parsed) {
      nodes += blob.parsed.n;
      edges += blob.parsed.ecount;
      continue;
    }
    if (blob.parsed === null) {
      failed++;
      continue;
    }
    const h = vgrfHeader(blob.src.slice(blob.off, blob.off + 16));
    if (h) {
      nodes += h.n;
      edges += h.ecount;
    } else {
      failed++;
    }
  }
  const bytes = state.graphBlobs.reduce((s, b) => s + b.graphSz, 0);
  if ($("st-graph")) {
    $("st-graph").textContent = nodes
      ? `${(bytes / 1048576).toFixed(2)} MB · ${nodes} 点 ${edges} 边 · ${state.portals.length} 跨区`
      : (bytes
        ? `${(bytes / 1048576).toFixed(2)} MB 图${failed ? "（部分解析失败）" : ""}`
        : `无 VGRF · ${state.portals.length} 跨区`);
  }
}

function summarizeElev() {
  const vals = [];
  let nodes = 0;
  let parsed = 0;
  for (const blob of state.graphBlobs) {
    const g = blob.parsed;
    if (!g) continue;
    parsed++;
    nodes += g.n;
    if (!g.ele) continue;
    for (let i = 0; i < g.ele.length; i++) {
      const v = g.ele[i];
      if (v > ELE_UNKNOWN) vals.push(v);
    }
  }
  vals.sort((a, b) => a - b);
  const known = vals.length;
  const rawMin = known ? vals[0] : 0;
  const rawMax = known ? vals[vals.length - 1] : 0;
  state.eleMin = known ? percentile(vals, 0.05) : 0;
  state.eleMax = known ? Math.max(state.eleMin + 1, percentile(vals, 0.95)) : 1;
  if ($("st-elev")) {
    if (known) {
      $("st-elev").textContent = `${rawMin}–${rawMax} m · ${known} 点`;
    } else if (parsed) {
      $("st-elev").textContent = "路网无 ELEV 段";
    } else if (state.graphBlobs.length) {
      $("st-elev").textContent = "按视野解析";
    } else {
      $("st-elev").textContent = "未打包路网图";
    }
  }
  if ($("ele-min")) $("ele-min").textContent = known ? `${state.eleMin} m` : "低";
  if ($("ele-max")) $("ele-max").textContent = known ? `${state.eleMax} m` : "高";
}

const ElevCanvasLayer = L.Layer.extend({
  onAdd(map) {
    this._map = map;
    if (!map.getPane("elevPane")) {
      map.createPane("elevPane");
    }
    const pane = map.getPane("elevPane");
    pane.style.zIndex = 650;
    pane.style.pointerEvents = "none";
    this._canvas = L.DomUtil.create("canvas", "vmap-elev-canvas");
    pane.appendChild(this._canvas);
    map.on("move zoom viewreset resize", this._redraw, this);
    this._redraw();
  },
  onRemove(map) {
    map.off("move zoom viewreset resize", this._redraw, this);
    if (this._canvas) {
      L.DomUtil.remove(this._canvas);
      this._canvas = null;
    }
  },
  _redraw() {
    const map = this._map;
    const canvas = this._canvas;
    if (!map || !canvas) return;
    const size = map.getSize();
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(size.x * dpr));
    canvas.height = Math.max(1, Math.round(size.y * dpr));
    canvas.style.width = size.x + "px";
    canvas.style.height = size.y + "px";
    L.DomUtil.setPosition(canvas, map.containerPointToLayerPoint([0, 0]));
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, size.x, size.y);
    const bounds = map.getBounds().pad(0.08);
    const z = map.getZoom();
    const minE = state.eleMin, maxE = state.eleMax;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    ctx.lineWidth = z >= 15 ? 5 : z >= 13.5 ? 4 : 3;
    const labels = [];
    if (z >= 12) {
      for (const g of graphsInView(bounds)) {
        if (!g.ele) continue;
        for (let i = 0; i < g.ecount; i++) {
          const a = g.ea[i], b = g.eb[i];
          if (a >= g.n || b >= g.n) continue;
          const ea = g.ele[a], eb = g.ele[b];
          if (ea <= ELE_UNKNOWN && eb <= ELE_UNKNOWN) continue;
          const em = (ea > ELE_UNKNOWN && eb > ELE_UNKNOWN) ? (ea + eb) * 0.5
            : (ea > ELE_UNKNOWN ? ea : eb);
          const p0 = map.latLngToContainerPoint([g.lat[a], g.lon[a]]);
          const p1 = map.latLngToContainerPoint([g.lat[b], g.lon[b]]);
          if ((p0.x < -40 && p1.x < -40) || (p0.x > size.x + 40 && p1.x > size.x + 40)
            || (p0.y < -40 && p1.y < -40) || (p0.y > size.y + 40 && p1.y > size.y + 40)) {
            continue;
          }
          ctx.strokeStyle = eleColor(em, minE, maxE);
          ctx.beginPath();
          ctx.moveTo(p0.x, p0.y);
          ctx.lineTo(p1.x, p1.y);
          ctx.stroke();
          if (z >= 14 && ea > ELE_UNKNOWN && (a % 6) === 0
            && p0.x > 12 && p0.x < size.x - 12 && p0.y > 14 && p0.y < size.y - 8) {
            labels.push({ x: p0.x, y: p0.y, t: ea + "m" });
          }
        }
      }
      summarizeElev();
    }
    if (labels.length && z >= 14) {
      ctx.font = "bold 11px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "bottom";
      ctx.lineWidth = 3;
      ctx.strokeStyle = "rgba(255,255,255,0.92)";
      ctx.fillStyle = "#111";
      for (const lb of labels) {
        ctx.strokeText(lb.t, lb.x, lb.y - 3);
        ctx.fillText(lb.t, lb.x, lb.y - 3);
      }
    }
  },
});

function refreshElev() {
  if (state.elevLayer) {
    state.map.removeLayer(state.elevLayer);
    state.elevLayer = null;
  }
  if (!$("tog-elev") || !$("tog-elev").checked) return;
  if (!state.graphBlobs.length) return;
  state.elevLayer = new ElevCanvasLayer();
  state.elevLayer.addTo(state.map);
}

const GraphCanvasLayer = L.Layer.extend({
  onAdd(map) {
    this._map = map;
    if (!map.getPane("graphPane")) {
      map.createPane("graphPane");
    }
    const pane = map.getPane("graphPane");
    pane.style.zIndex = 660;
    pane.style.pointerEvents = "none";
    this._canvas = L.DomUtil.create("canvas", "vmap-graph-canvas");
    pane.appendChild(this._canvas);
    map.on("move zoom viewreset resize", this._redraw, this);
    this._redraw();
  },
  onRemove(map) {
    map.off("move zoom viewreset resize", this._redraw, this);
    if (this._canvas) {
      L.DomUtil.remove(this._canvas);
      this._canvas = null;
    }
  },
  _redraw() {
    const map = this._map;
    const canvas = this._canvas;
    if (!map || !canvas) return;
    const size = map.getSize();
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.max(1, Math.round(size.x * dpr));
    canvas.height = Math.max(1, Math.round(size.y * dpr));
    canvas.style.width = size.x + "px";
    canvas.style.height = size.y + "px";
    L.DomUtil.setPosition(canvas, map.containerPointToLayerPoint([0, 0]));
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, size.x, size.y);
    const bounds = map.getBounds().pad(0.08);
    const z = map.getZoom();
    const elevOn = $("tog-elev") && $("tog-elev").checked;
    ctx.lineCap = "round";
    ctx.lineJoin = "round";
    const lenLabels = [];
    if (z >= 12) {
      const maxCls = z >= 14 ? 7 : z >= 13 ? 4 : 3;
      for (const g of graphsInView(bounds)) {
        const step = (g.ecount > 100000 && z < 15.5) ? Math.ceil(g.ecount / 80000) : 1;
        for (let i = 0; i < g.ecount; i += step) {
          const cls = g.ecls[i];
          if (cls > maxCls) continue;
          const a = g.ea[i], b = g.eb[i];
          if (a >= g.n || b >= g.n) continue;
          const flags = g.eflags[i];
          const p0 = map.latLngToContainerPoint([g.lat[a], g.lon[a]]);
          const p1 = map.latLngToContainerPoint([g.lat[b], g.lon[b]]);
          if ((p0.x < -40 && p1.x < -40) || (p0.x > size.x + 40 && p1.x > size.x + 40)
            || (p0.y < -40 && p1.y < -40) || (p0.y > size.y + 40 && p1.y > size.y + 40)) {
            continue;
          }
          if (elevOn) {
            ctx.strokeStyle = "rgba(17, 24, 39, 0.42)";
            ctx.lineWidth = graphEdgeWidth(cls, z) + 1.4;
          } else {
            ctx.strokeStyle = graphEdgeColor(cls, flags);
            ctx.lineWidth = graphEdgeWidth(cls, z);
          }
          if (flags & 2) ctx.setLineDash([5, 4]);
          else ctx.setLineDash([]);
          ctx.beginPath();
          ctx.moveTo(p0.x, p0.y);
          ctx.lineTo(p1.x, p1.y);
          ctx.stroke();
          if (!elevOn && z >= 15 && g.elen[i] >= 1200 && (a % 7) === 0
            && p0.x > 8 && p1.x > 8 && p0.x < size.x - 8 && p1.x < size.x - 8) {
            lenLabels.push({
              x: (p0.x + p1.x) * 0.5,
              y: (p0.y + p1.y) * 0.5,
              t: Math.round(g.elen[i] / 100) + "m",
            });
          }
        }
        ctx.setLineDash([]);
        if (z < 13.2) continue;
        for (let i = 0; i < g.n; i++) {
          const deg = g.deg ? g.deg[i] : 0;
          const junction = deg >= 6;
          if (!junction && z < 14.0) continue;
          if (!junction && deg < 4 && z < 15.5) continue;
          const p = map.latLngToContainerPoint([g.lat[i], g.lon[i]]);
          if (p.x < -6 || p.y < -6 || p.x > size.x + 6 || p.y > size.y + 6) continue;
          const r = junction ? (z >= 15 ? 3.4 : 2.8) : (z >= 16 ? 2.0 : 1.5);
          ctx.beginPath();
          ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
          ctx.fillStyle = junction ? "#111" : "#334155";
          ctx.fill();
          ctx.lineWidth = 1.2;
          ctx.strokeStyle = "#fff";
          ctx.stroke();
        }
      }
    }
    if (lenLabels.length && z >= 15 && !elevOn) {
      ctx.setLineDash([]);
      ctx.font = "10px system-ui, sans-serif";
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      ctx.lineWidth = 3;
      ctx.strokeStyle = "rgba(255,255,255,0.9)";
      ctx.fillStyle = "#0f766e";
      for (const lb of lenLabels) {
        ctx.strokeText(lb.t, lb.x, lb.y);
        ctx.fillText(lb.t, lb.x, lb.y);
      }
    }
    summarizeGraph();
  },
});

function refreshGraph() {
  if (state.graphLayer) {
    state.map.removeLayer(state.graphLayer);
    state.graphLayer = null;
  }
  if (!$("tog-graph") || !$("tog-graph").checked) return;
  if (!state.graphBlobs.length) return;
  state.graphLayer = new GraphCanvasLayer();
  state.graphLayer.addTo(state.map);
}

async function runPool(items, limit, fn, onProgress) {
  let next = 0;
  let done = 0;
  const n = items.length;
  const workers = Array.from({ length: Math.min(limit, Math.max(1, n)) }, async () => {
    for (;;) {
      const i = next++;
      if (i >= n) return;
      await fn(items[i], i);
      done++;
      if (done === n || done % 6 === 0) {
        if ($("src-fmt")) $("src-fmt").textContent = `VREG ${n} 区 · ${done}/${n}`;
        if (onProgress) onProgress(done, n);
        await new Promise((r) => setTimeout(r, 0));
      }
    }
  });
  await Promise.all(workers);
  return done;
}

function applyDefaultView() {
  if (!state.map) return;
  /* 先把加载到的包整片铺满 —— 内置演示集正好是北京那 64 块，
     于是默认视野就是这批瓦片的中心。包太大（全国）则退回坐标默认。 */
  const rs = state.catalog && state.catalog.regions;
  if (rs && rs.length && rs.length <= FIT_MAX_CELLS) {
    let w = Infinity;
    let s = Infinity;
    let e = -Infinity;
    let n = -Infinity;
    for (const r of rs) {
      if (r.west < w) w = r.west;
      if (r.south < s) s = r.south;
      if (r.east > e) e = r.east;
      if (r.north > n) n = r.north;
    }
    if (Number.isFinite(w)) {
      state.map.fitBounds([[s, w], [n, e]], { maxZoom: 15 });
      return;
    }
  }
  const parsed = parseCoord($("goto-coord") && $("goto-coord").value);
  if (parsed) {
    state.map.setView([parsed.lat, parsed.lon], 14);
    return;
  }
  state.map.setView(DEMO_CENTER, 12);
}

function regionsInView(pad) {
  if (!state.catalog || !state.map) return [];
  const bounds = state.map.getBounds().pad(pad || 0);
  const out = [];
  for (const r of state.catalog.regions) {
    if (r.east < bounds.getWest() || r.west > bounds.getEast()
      || r.north < bounds.getSouth() || r.south > bounds.getNorth()) continue;
    out.push(r);
  }
  return out;
}

function pickRegionsToLoad(max) {
  const vis = regionsInView(0.2);
  if (vis.length <= max) return vis;
  const c = state.map.getCenter();
  vis.sort((a, b) => {
    const ac = Math.abs((a.south + a.north) / 2 - c.lat)
      + Math.abs((a.west + a.east) / 2 - c.lng);
    const bc = Math.abs((b.south + b.north) / 2 - c.lat)
      + Math.abs((b.west + b.east) / 2 - c.lng);
    return ac - bc;
  });
  return vis.slice(0, max);
}

let visLoadGen = 0;
async function ensureVisibleVpks() {
  if (!state.catalog || !state.loader || !state.map) return;
  const gen = ++visLoadGen;
  const want = pickRegionsToLoad(32).filter((r) => !state.vpkMeta.has(r.id));
  if (want.length === 0) {
    fillList(pickRegionsToLoad(80));
    refreshVec();
    return;
  }
  $("src-fmt").textContent =
    `GRID ${state.catalog.regions.length} 区 · 加载视野 ${want.length} 格`;
  await runPool(want, 6, async (r) => {
    try {
      await loadRegionVpk(r, true);
    } catch (e) {
      console.warn(e);
      state.vpkMeta.set(r.id, { rel: vpkRel(r), tiles: 0, graphSz: 0, missing: true });
    }
  }, (done) => {
    if (done === 4 || done % 12 === 0) refreshVec();
  });
  if (gen !== visLoadGen) return;
  $("src-fmt").textContent = `GRID ${state.catalog.regions.length} 区`;
  $("st-regions").textContent = String(state.catalog.regions.length);
  $("st-tiles").textContent = String(state.tileIndex.size);
  $("st-bytes").textContent = `${(state.bytes / 1048576).toFixed(1)} MB`;
  fillList(pickRegionsToLoad(80));
  refreshVec();
}

let portalsLoading = null;
async function loadPortalsLazy() {
  if (!state.loader || state.portals.length) return;
  if (portalsLoading) return portalsLoading;
  portalsLoading = (async () => {
    try {
      const portBuf = await state.loader.try("route.port");
      if (portBuf) {
        state.portals = parsePort(portBuf);
        if ($("st-graph") && $("st-graph").textContent) {
          summarizeGraph();
        }
      }
    } catch (e) {
      console.warn("route.port", e);
    } finally {
      portalsLoading = null;
    }
  })();
  return portalsLoading;
}

async function openPack(loader, catalog) {
  state.loader = loader;
  state.tileIndex.clear();
  state.parsedTiles.clear();
  state.vpkMeta.clear();
  state.graphs = [];
  state.graphBlobs = [];
  state.portals = [];
  state.portalKeys = new Set();
  state.bytes = 0;
  $("src-dir").textContent = loader.label;
  $("src-fmt").textContent = catalog ? "读取网格目录…" : "读取 map.idx…";

  if (catalog) {
    state.catalog = catalog;
  } else {
    const idxBuf = await loader.get("map.idx");
    state.bytes += idxBuf.byteLength;
    state.catalog = parseVreg(idxBuf);
  }
  const nreg = state.catalog.regions.length;
  $("src-fmt").textContent = `GRID ${nreg} 区`;

  applyDefaultView();

  $("st-regions").textContent = `${nreg}`;
  await ensureVisibleVpks();
  if (state.tileIndex.size === 0) {
    $("src-fmt").textContent = `GRID ${nreg} 区 · 视野内没有瓦片（检查坐标是否在广州包内）`;
  }
  summarizeGraph();
  summarizeElev();
  rebuildOverlays();
  refreshVec();
  refreshElev();
  refreshGraph();
  loadPortalsLazy();
}

function parseCoord(text) {
  const nums = String(text || "").match(/[+-]?\d+(?:\.\d+)?/g);
  if (!nums || nums.length < 2) return null;
  const a = Number(nums[0]);
  const b = Number(nums[1]);
  if (!Number.isFinite(a) || !Number.isFinite(b)) return null;
  let lat;
  let lon;
  if (Math.abs(a) <= 90 && Math.abs(b) > 90 && Math.abs(b) <= 180) {
    lat = a;
    lon = b;
  } else if (Math.abs(b) <= 90 && Math.abs(a) > 90 && Math.abs(a) <= 180) {
    lon = a;
    lat = b;
  } else {
    lat = a;
    lon = b;
  }
  if (lat < -90 || lat > 90 || lon < -180 || lon > 180) return null;
  return { lat, lon };
}

function gotoCoord() {
  const msg = $("goto-msg");
  const parsed = parseCoord($("goto-coord").value);
  if (!parsed || !state.map) {
    if (msg) msg.textContent = "请输入纬度, 经度（例如 23.131127, 113.284718）";
    return;
  }
  const zoom = Math.max(state.map.getZoom() || 0, 14);
  state.map.setView([parsed.lat, parsed.lon], zoom);
  if (state.gotoMarker) state.map.removeLayer(state.gotoMarker);
  state.gotoMarker = L.circleMarker([parsed.lat, parsed.lon], {
    radius: 7,
    color: "#2563eb",
    weight: 2,
    fillColor: "#60a5fa",
    fillOpacity: 0.85,
  }).addTo(state.map);
  if (msg) {
    msg.textContent = `已跳到 ${parsed.lat.toFixed(6)}, ${parsed.lon.toFixed(6)}`;
  }
}

function initMap() {
  if (typeof L === "undefined") {
    $("src-fmt").textContent = "Leaflet 未加载（/static/leaflet/leaflet.js）";
    return;
  }
  state.map = L.map("map", { zoomSnap: 0.25 });
  state.map.createPane("elevPane");
  state.map.getPane("elevPane").style.zIndex = 450;
  state.map.getPane("elevPane").style.pointerEvents = "none";
  state.map.createPane("graphPane");
  state.map.getPane("graphPane").style.zIndex = 460;
  state.map.getPane("graphPane").style.pointerEvents = "none";
  state.osm = L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    attribution: "&copy; OpenStreetMap",
    maxZoom: 19,
  });
  state.map.setView(DEMO_CENTER, 12);
  state.map.on("moveend", () => {
    if (!state.catalog) return;
    clearTimeout(state._visTimer);
    state._visTimer = setTimeout(() => {
      ensureVisibleVpks().then(() => {
        rebuildOverlays();
        refreshVec();
        refreshElev();
        refreshGraph();
      });
    }, 220);
  });

  $("tog-osm").addEventListener("change", () => {
    if ($("tog-osm").checked) state.osm.addTo(state.map);
    else state.map.removeLayer(state.osm);
    refreshVec();
  });
  $("tog-vec").addEventListener("change", refreshVec);
  $("tog-grid").addEventListener("change", rebuildOverlays);
  $("tog-lbl").addEventListener("change", refreshVec);
  if ($("tog-land")) $("tog-land").addEventListener("change", refreshVec);
  if ($("tog-elev")) {
    $("tog-elev").addEventListener("change", () => {
      refreshVec();
      refreshElev();
      refreshGraph();
    });
  }
  if ($("tog-graph")) {
    $("tog-graph").addEventListener("change", refreshGraph);
  }
  $("tog-port").addEventListener("change", () => {
    if ($("tog-port").checked) {
      loadPortalsLazy().then(rebuildOverlays);
    } else {
      rebuildOverlays();
    }
  });
  if ($("goto-btn")) $("goto-btn").addEventListener("click", gotoCoord);
  if ($("goto-coord")) {
    $("goto-coord").addEventListener("keydown", (ev) => {
      if (ev.key === "Enter") {
        ev.preventDefault();
        gotoCoord();
      }
    });
  }
}

async function tryServer() {
  try {
    const info = await fetch("/api/info").then((r) => r.json());
    $("src-dir").textContent = info.map_dir || "—";
    if (info.grid) {
      const cat = await fetch("/api/catalog").then((r) => r.json());
      await openPack(makeLoaderFromHttp(), cat);
      return;
    }
    if (!info.has_idx) {
      $("src-fmt").textContent = "服务端无 lon*/lat*/x*_y*.vpk 也无 map.idx";
      return;
    }
    await openPack(makeLoaderFromHttp());
  } catch (e) {
    $("src-fmt").textContent = String(e.message || e);
  }
}

initMap();

$("open-btn").addEventListener("click", () => $("dir-input").click());
$("dir-input").addEventListener("change", async (ev) => {
  try {
    const loader = makeLoaderFromFiles(ev.target.files);
    const cat = catalogFromGridKeys(loader.keys, loader.root);
    await openPack(loader, cat.regions.length ? cat : null);
  } catch (e) {
    $("src-fmt").textContent = String(e.message || e);
  }
});
$("reload-btn").addEventListener("click", tryServer);

tryServer();
