// ================================================================
// GLOBALS
// ================================================================
var M = 'auto'; // parse mode: auto | frame | proto | bop
var S = null;   // edit state
var EF = 'hex'; // export format: hex | hexc | base64 | carray

/*  State shape:
    S = {
      mode: 'frame'|'proto'|'bop',
      frame: null | { token, route, flags, msgId },
      wrapFrame: false,   // user toggle: wrap in frame on export
      hasBop: boolean,
      bopPath: [{ fn, fields? }],  // path to BattleOp through nested protos (auto-detected)
                                    // fields = editable sibling fields at that nesting level (lvl>0)
      genFields: [{ fn, wt, t, ... }],  // outermost non-BattleOp proto fields
      origBytes: Uint8Array,
      en: [{ hd, itm, seq, tick, emp, ei, fs }]
    }
*/

function setM(el) {
  document.querySelectorAll('.mbtn').forEach(function (b) { b.classList.remove('on'); });
  el.classList.add('on');
  M = el.dataset.m;
}

function setEF(el) {
  document.querySelectorAll('.fmt-btn').forEach(function (b) { b.classList.remove('on'); });
  el.classList.add('on');
  EF = el.dataset.f;
}

// ================================================================
// SNAPPY DECOMPRESSION (for ByteString fields)
// ================================================================
function snappyDecompress(input) {
  if (!input || input.length < 3) return null;
  var pos = 0;
  // Read uncompressed length (varint)
  var len = 0, shift = 0;
  while (pos < input.length && shift < 35) {
    var b = input[pos++];
    len |= (b & 0x7f) << shift;
    shift += 7;
    if ((b & 0x80) === 0) break;
  }
  len = len >>> 0;
  if (len === 0 || len > 20 * input.length || len > 10 * 1024 * 1024) return null;
  var output = new Uint8Array(len);
  var outPos = 0;
  while (pos < input.length && outPos < len) {
    var tag = input[pos++];
    var type = tag & 3;
    if (type === 0) {
      var n = (tag >> 2) & 0x3f;
      var litLen;
      if (n < 60) litLen = n + 1;
      else if (n === 60) { litLen = input[pos++] + 1; }
      else if (n === 61) { litLen = (input[pos] | (input[pos+1] << 8)) + 1; pos += 2; }
      else if (n === 62) { litLen = (input[pos] | (input[pos+1] << 8) | (input[pos+2] << 16)) + 1; pos += 3; }
      else { litLen = (input[pos] | (input[pos+1] << 8) | (input[pos+2] << 16) | ((input[pos+3] << 24) >>> 0)) + 1; pos += 4; }
      if (pos + litLen > input.length || outPos + litLen > len) return null;
      output.set(input.subarray(pos, pos + litLen), outPos);
      pos += litLen;
      outPos += litLen;
    } else if (type === 1) {
      var copyLen = ((tag >> 2) & 7) + 4;
      var offset = ((tag >> 5) << 8) | input[pos++];
      if (offset === 0 || offset > outPos) return null;
      for (var i = 0; i < copyLen; i++) { output[outPos] = output[outPos - offset]; outPos++; }
    } else if (type === 2) {
      var copyLen = ((tag >> 2) & 0x3f) + 1;
      var offset = input[pos] | (input[pos+1] << 8); pos += 2;
      if (offset === 0 || offset > outPos) return null;
      for (var i = 0; i < copyLen; i++) { output[outPos] = output[outPos - offset]; outPos++; }
    } else {
      var copyLen = ((tag >> 2) & 0x3f) + 1;
      var offset = input[pos] | (input[pos+1] << 8) | (input[pos+2] << 16) | ((input[pos+3] << 24) >>> 0); pos += 4;
      if (offset === 0 || offset > outPos) return null;
      for (var i = 0; i < copyLen; i++) { output[outPos] = output[outPos - offset]; outPos++; }
    }
  }
  if (outPos !== len) return null;
  return output;
}

// ================================================================
// BINARY HELPERS
// ================================================================
function pib(r, f) {
  r = r.trim();
  if (!r) return null;
  if (f === 'base64') {
    var b = atob(r.replace(/\s+/g, ''));
    var a = new Uint8Array(b.length);
    for (var i = 0; i < b.length; i++) a[i] = b.charCodeAt(i);
    return a;
  }
  if (f === 'carray') {
    var m = r.match(/0x[0-9a-fA-F]{1,2}/g);
    return m ? new Uint8Array(m.map(function (s) { return parseInt(s, 16); })) : null;
  }
  var c = r.replace(/[^0-9a-fA-F]/g, '');
  if (c.length < 2 || c.length % 2) return null;
  var o = new Uint8Array(c.length / 2);
  for (var i = 0; i < o.length; i++) o[i] = parseInt(c.substr(i * 2, 2), 16);
  return o;
}

function r32b(b, o) { return ((b[o] << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]) >>> 0; }
function r32l(b, o) { return ((b[o + 3] << 24) | (b[o + 2] << 16) | (b[o + 1] << 8) | b[o]) >>> 0; }
function ri32(b, o) { var v = r32l(b, o); return v > 0x7FFFFFFF ? v - 0x100000000 : v; }
function ri64(b, o) {
  var lo = r32l(b, o), hi = r32l(b, o + 4);
  if (hi === 0xFFFFFFFF) return lo - 0x100000000;
  if (hi === 0) return lo;
  if (hi > 0x7FFFFFFF) return -(0x100000000 * (0xFFFFFFFF - hi) + (0x100000000 - lo));
  return hi * 0x100000000 + lo;
}
function w32b(b, o, v) { v = v >>> 0; b[o] = v >>> 24; b[o + 1] = (v >>> 16) & 0xFF; b[o + 2] = (v >>> 8) & 0xFF; b[o + 3] = v & 0xFF; }
function w32l(b, o, v) { v = v >>> 0; b[o] = v & 0xFF; b[o + 1] = (v >>> 8) & 0xFF; b[o + 2] = (v >>> 16) & 0xFF; b[o + 3] = v >>> 24; }
function wi32(b, o, v) { if (v < 0) v += 0x100000000; w32l(b, o, v); }
function wi64(b, o, v) {
  if (v >= 0 && v < 0x100000000) { w32l(b, o, v); w32l(b, o + 4, 0); }
  else if (v < 0 && v >= -0x100000000) { w32l(b, o, (v + 0x100000000) >>> 0); w32l(b, o + 4, 0xFFFFFFFF); }
  else { var lo = v >>> 0, hi = ((v - lo) / 0x100000000) >>> 0; w32l(b, o, lo); w32l(b, o + 4, hi); }
}

function th(n, p) { return '0x' + n.toString(16).padStart(p || 8, '0').toUpperCase(); }
function tb(b) { return b.toString(16).padStart(2, '0').toUpperCase(); }
function bh(a) { return Array.from(a).map(tb).join(' '); }
function es(s) { return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;'); }

// Parse hex-or-decimal string → unsigned 32-bit
function phd(s) {
  s = s.trim();
  if (s.startsWith('0x') || s.startsWith('0X')) return parseInt(s, 16) >>> 0;
  return parseInt(s, 10) >>> 0;
}

// Format bytes in selected export format
function fmtBytes(bytes) {
  if (EF === 'hexc') return Array.from(bytes).map(tb).join('');
  if (EF === 'base64') return btoa(String.fromCharCode.apply(null, bytes));
  if (EF === 'carray') return '{ ' + Array.from(bytes).map(function (b) { return '0x' + tb(b); }).join(', ') + ' }';
  return bh(bytes); // default: spaced hex
}

// ================================================================
// PROTOBUF WIRE FORMAT
// ================================================================
function dv(buf, p) {
  var r = 0, s = 0;
  while (p < buf.length) {
    var b = buf[p++];
    r |= (b & 0x7F) << s;
    if (!(b & 0x80)) return { v: r >>> 0, p: p };
    s += 7;
    if (s > 35) throw 'VTL';
  }
  throw 'VTR';
}

function ev(v) {
  var o = []; v = v >>> 0;
  do { o.push((v & 0x7F) | (v > 0x7F ? 0x80 : 0)); v >>>= 7; } while (v);
  return o;
}

function dv64(buf, p) {
  var lo = 0, hi = 0, s = 0, st = p;
  while (p < buf.length) {
    var b = buf[p++];
    if (s < 28) lo |= (b & 0x7F) << s;
    else if (s === 28) { lo |= (b & 0x0F) << 28; hi |= (b & 0x70) >> 4; }
    else hi |= (b & 0x7F) << (s - 32);
    if (!(b & 0x80)) { lo = lo >>> 0; hi = hi >>> 0; return { lo: lo, hi: hi, raw: buf.slice(st, p), p: p }; }
    s += 7;
    if (s > 63) throw 'V64';
  }
  throw 'V64T';
}

function ev64(lo, hi) {
  var o = []; lo = lo >>> 0; hi = hi >>> 0;
  while (hi > 0 || lo > 0x7F) {
    o.push((lo & 0x7F) | 0x80);
    lo = ((lo >>> 7) | (hi << 25)) >>> 0;
    hi >>>= 7;
  }
  o.push(lo & 0x7F);
  return o;
}

function ivn1(v) {
  if (!v.raw || v.raw.length < 2) return false;
  for (var i = 0; i < v.raw.length - 1; i++) if (v.raw[i] !== 0xFF) return false;
  return v.raw[v.raw.length - 1] === 0x01;
}

function dpb(buf, s, e) {
  var fs = [], p = s;
  while (p < e) {
    var ts = p, tg;
    try { tg = dv(buf, p); } catch (x) { break; }
    var fn = tg.v >>> 3, wt = tg.v & 7;
    p = tg.p;
    if (fn === 0 || fn > 536870911) break;
    var f = { fn: fn, wt: wt, off: ts };
    try {
      if (wt === 0) {
        var v = dv64(buf, p); f.t = 'vi'; f.lo = v.lo; f.hi = v.hi;
        f.v = v.hi === 0 ? v.lo : null; f.rv = v.raw; f.n1 = ivn1(v); p = v.p;
      } else if (wt === 1) {
        if (p + 8 > e) throw 'T'; f.t = 'f64'; f.b = buf.slice(p, p + 8); p += 8;
      } else if (wt === 2) {
        var ln = dv(buf, p); p = ln.p;
        if (p + ln.v > e) throw 'T';
        f.t = 'ld'; f.l = ln.v; f.rb = buf.slice(p, p + ln.v);
        try { if (f.l > 2) { var n = dpb(f.rb, 0, f.rb.length); if (n.length > 0 && !n.some(function (x) { return x.err; })) f.nf = n; } } catch (x) { }
        if (!f.nf && f.l > 4) { try { var sd = snappyDecompress(f.rb); if (sd) { var sn = dpb(sd, 0, sd.length); if (sn.length > 0 && !sn.some(function(x){return x.err;})) { f.nf = sn; f.snappy = true; f.rb = sd; f.l = sd.length; } } } catch(x){} }
        p += ln.v;
      } else if (wt === 5) {
        if (p + 4 > e) throw 'T'; f.t = 'f32'; f.b = buf.slice(p, p + 4); f.v = r32l(buf, p); p += 4;
      } else {
        f.t = '?'; f.err = 'wire ' + wt; fs.push(f); return fs;
      }
    } catch (x) { f.err = String(x); fs.push(f); return fs; }
    fs.push(f);
  }
  return fs;
}

// ================================================================
// ENTITY BLOB
// ================================================================
function decEnt(raw) {
  if (raw.length < 16) return { tn: 'unk', sz: raw.length, tp: 0, raw: new Uint8Array(raw), uuid: new Uint8Array(8), uh: '00 00 00 00 00 00 00 00' };
  var sz = r32l(raw, 0), tp = r32l(raw, 4), uuid = raw.slice(8, 16);
  var i = { raw: new Uint8Array(raw), sz: sz, tp: tp, tn: tp === 1 ? 'hero' : tp === 2 ? 'spell' : 't' + tp, uuid: uuid, uh: bh(uuid) };
  if (tp === 1 && raw.length >= 17) i.ab = raw[16];
  else if (tp === 2 && raw.length >= 47) {
    i.mk = r32l(raw, 16); i.f1 = ri32(raw, 20);
    i.vA = ri64(raw, 24); i.vB = ri64(raw, 32);
    i.fC = raw[40]; i.fD = raw[41]; i.fE = raw[42]; i.sn = ri32(raw, 43);
  }
  return i;
}

function encEnt(i) {
  if (i.tp === 1) {
    var b = new Uint8Array(17);
    w32l(b, 0, 17); w32l(b, 4, 1); b.set(i.uuid, 8); b[16] = i.ab ? 1 : 0;
    return b;
  }
  if (i.tp === 2) {
    var b = new Uint8Array(47);
    w32l(b, 0, 47); w32l(b, 4, 2); b.set(i.uuid, 8);
    w32l(b, 16, i.mk !== undefined ? i.mk : 82);
    wi32(b, 20, i.f1 !== undefined ? i.f1 : -1);
    wi64(b, 24, i.vA !== undefined ? i.vA : 0);
    wi64(b, 32, i.vB !== undefined ? i.vB : 0);
    b[40] = i.fC !== undefined ? i.fC : 0;
    b[41] = i.fD !== undefined ? i.fD : 0;
    b[42] = i.fE !== undefined ? i.fE : 0;
    wi32(b, 43, i.sn !== undefined ? i.sn : -1);
    return b;
  }
  // Unknown type: return raw bytes if available
  if (i.raw) return new Uint8Array(i.raw);
  // Fallback: empty 16-byte blob
  var b = new Uint8Array(16);
  w32l(b, 0, 16); w32l(b, 4, i.tp || 0); b.set(i.uuid || new Uint8Array(8), 8);
  return b;
}

// ================================================================
// PROTO ENCODE
// ================================================================
function etag(fn, wt) { return ev((fn << 3) | wt); }

function encProtoEntry(itm, entB, seq, tick, empty) {
  var o = [];
  // field 1: item_id
  o.push.apply(o, etag(1, 0)); o.push.apply(o, ev(itm));
  // field 2: entity_info wrapper { field 1: entity_blob }
  var inn = [];
  inn.push.apply(inn, etag(1, 2)); inn.push.apply(inn, ev(entB.length));
  for (var i = 0; i < entB.length; i++) inn.push(entB[i]);
  o.push.apply(o, etag(2, 2)); o.push.apply(o, ev(inn.length));
  o.push.apply(o, inn);
  // field 4: seq
  o.push.apply(o, etag(4, 0)); o.push.apply(o, ev(seq));
  // field 5: tick
  o.push.apply(o, etag(5, 0));
  if (empty) o.push(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01);
  else o.push.apply(o, ev(tick));
  return new Uint8Array(o);
}

function encBopEntry(hdr, pb) {
  var bl = 4 + pb.length;
  var o = new Uint8Array(4 + bl);
  w32l(o, 0, bl); w32l(o, 4, hdr); o.set(pb, 8);
  return o;
}

// ================================================================
// BATTLEOP DECODE
// ================================================================
function decBop(buf) {
  var es = [], p = 0;
  while (p + 8 <= buf.length) {
    var bl = r32l(buf, p);
    if (bl < 4 || p + 4 + bl > buf.length) break;
    var hd = r32l(buf, p + 4), fs = [];
    try { fs = dpb(buf, p + 8, p + 4 + bl); } catch (x) { }
    var itm = null, seq = null, tick = null, emp = false, eb = null;
    for (var i = 0; i < fs.length; i++) {
      var f = fs[i];
      if (f.fn === 1 && f.t === 'vi') itm = f.v;
      if (f.fn === 2 && f.t === 'ld') eb = f.rb;
      if (f.fn === 4 && f.t === 'vi') seq = f.v;
      if (f.fn === 5 && f.t === 'vi') { tick = f.v; emp = f.n1; }
    }
    var ei = null;
    if (eb && eb.length > 2) {
      var inf = dpb(eb, 0, eb.length);
      for (var j = 0; j < inf.length; j++) {
        if (inf[j].fn === 1 && inf[j].t === 'ld') { ei = decEnt(inf[j].rb); break; }
      }
    }
    es.push({ bl: bl, hd: hd, fs: fs, itm: itm, seq: seq, tick: tick, emp: emp, eb: eb, ei: ei, raw: buf.slice(p, p + 4 + bl) });
    p += 4 + bl;
  }
  return { es: es, rem: p < buf.length ? buf.slice(p) : null };
}

// ================================================================
// BATTLEOP AUTO-DISCOVERY
// ================================================================
function scoreBop(buf) {
  if (!buf || buf.length < 8) return 0;
  try {
    var bop = decBop(buf);
    if (!bop.es || bop.es.length === 0) return 0;
    var score = 0;
    for (var i = 0; i < bop.es.length; i++) {
      var e = bop.es[i];
      if (e.itm !== null) score += 1;
      if (e.seq !== null) score += 1;
      if (e.tick !== null || e.emp) score += 1;
      if (e.ei) score += 2;
    }
    if (bop.rem && bop.rem.length > 0) score -= 2;
    return score;
  } catch (x) { return 0; }
}

// Recursively search all ld fields (and their nested protos) for BattleOp data
function discoverBopDeep(fields, depth) {
  if (depth > 8) return null;
  var best = null;
  for (var i = 0; i < fields.length; i++) {
    if (fields[i].t !== 'ld') continue;
    // Try this field directly as BattleOp
    var sc = scoreBop(fields[i].rb);
    if (sc >= 3 && (!best || sc > best.score)) {
      best = { score: sc, path: [i] };
    }
    // Recurse into nested proto fields
    if (fields[i].nf && fields[i].nf.length > 0) {
      var deeper = discoverBopDeep(fields[i].nf, depth + 1);
      if (deeper && (!best || deeper.score > best.score)) {
        best = { score: deeper.score, path: [i].concat(deeper.path) };
      }
    }
  }
  return best;
}

function discoverBop(protoFields) {
  return discoverBopDeep(protoFields, 0);
}

// Convert a parsed proto field to an editable genField (recursively for nested proto)
function makeGenField(pf) {
  var gf = { fn: pf.fn, wt: pf.wt, t: pf.t };
  if (pf.t === 'vi') { gf.lo = pf.lo; gf.hi = pf.hi; gf.n1 = pf.n1; gf.v = (pf.v !== null) ? pf.v : (pf.hi > 0 ? pf.hi * 0x100000000 + pf.lo : 0); }
  else if (pf.t === 'ld') {
    gf.rb = new Uint8Array(pf.rb);
    if (pf.nf && pf.nf.length > 0) {
      var subs = [];
      for (var k = 0; k < pf.nf.length; k++) {
        if (!pf.nf[k].err) subs.push(makeGenField(pf.nf[k]));
      }
      if (subs.length > 0) gf.sub = subs;
    }
  }
  else if (pf.t === 'f32') { gf.b = new Uint8Array(pf.b); gf.v = pf.v; }
  else if (pf.t === 'f64') { gf.b = new Uint8Array(pf.b); }
  return gf;
}

// Walk discovery path to extract bopBuf and build bopPath with editable sibling fields
function walkBopPath(protoFields, discoveryPath) {
  var bopPathArr = [];
  var curFields = protoFields;
  var bopBuf = null;
  for (var lvl = 0; lvl < discoveryPath.length; lvl++) {
    var idx = discoveryPath[lvl];
    var field = curFields[idx];
    var entry = { fn: field.fn };
    // For levels > 0, build editable fields from siblings
    if (lvl > 0) {
      var siblings = [];
      for (var fi = 0; fi < curFields.length; fi++) {
        if (fi === idx) continue;
        if (curFields[fi].err) continue;
        siblings.push(makeGenField(curFields[fi]));
      }
      entry.fields = siblings;
    }
    bopPathArr.push(entry);
    if (lvl < discoveryPath.length - 1) {
      curFields = field.nf;
    } else {
      bopBuf = field.rb;
    }
  }
  return { bopBuf: bopBuf, bopPath: bopPathArr };
}

// ================================================================
// FRAME DECODE
// ================================================================
function decFrames(buf) {
  var fs = [], o = 0;
  while (o + 20 <= buf.length) {
    var fl = r32b(buf, o);
    if (fl < 16 || o + 4 + fl > buf.length) { if (!fs.length) return null; break; }
    var pl = buf.slice(o + 20, o + 4 + fl);
    var pf = [];
    try { pf = dpb(pl, 0, pl.length); } catch (x) { }
    fs.push({
      fl: fl, tok: r32b(buf, o + 4), rt: r32b(buf, o + 8),
      fg: r32b(buf, o + 12), mid: r32b(buf, o + 16),
      pl: pl, pf: pf, raw: buf.slice(o, o + 4 + fl)
    });
    o += 4 + fl;
  }
  return fs.length ? fs : null;
}

// ================================================================
// HEX DUMP
// ================================================================
function rhx(bytes, hl) {
  var ls = [];
  for (var i = 0; i < bytes.length; i += 16) {
    var rw = bytes.slice(i, Math.min(i + 16, bytes.length));
    var hx = [], as = [];
    for (var j = 0; j < 16; j++) {
      if (j < rw.length) {
        var c = (i + j) < hl ? 'hh' : 'hp';
        hx.push('<span class="' + c + '">' + tb(rw[j]) + '</span>');
        var ch = rw[j];
        as.push('<span class="' + ((i + j) < hl ? 'hh' : 'ha') + '">' + (ch >= 0x20 && ch < 0x7F ? es(String.fromCharCode(ch)) : '.') + '</span>');
      } else { hx.push('  '); as.push(' '); }
      if (j === 7) hx.push(' ');
    }
    ls.push('<span class="ho">' + i.toString(16).padStart(6, '0') + '</span>  ' + hx.join(' ') + '  ' + as.join(''));
  }
  return ls.join('\n');
}

// Annotated hex dump with multiple color regions: [{start, end, cls}]
function rhxRegions(bytes, regions) {
  var ls = [];
  for (var i = 0; i < bytes.length; i += 16) {
    var rw = bytes.slice(i, Math.min(i + 16, bytes.length));
    var hx = [], as = [];
    for (var j = 0; j < 16; j++) {
      if (j < rw.length) {
        var pos = i + j, cls = 'hp';
        for (var r = 0; r < regions.length; r++) {
          if (pos >= regions[r].s && pos < regions[r].e) { cls = regions[r].c; break; }
        }
        hx.push('<span class="' + cls + '">' + tb(rw[j]) + '</span>');
        var ch = rw[j];
        as.push('<span class="' + cls + '">' + (ch >= 0x20 && ch < 0x7F ? es(String.fromCharCode(ch)) : '.') + '</span>');
      } else { hx.push('  '); as.push(' '); }
      if (j === 7) hx.push(' ');
    }
    ls.push('<span class="ho">' + i.toString(16).padStart(6, '0') + '</span>  ' + hx.join(' ') + '  ' + as.join(''));
  }
  return ls.join('\n');
}

function rpf(fs) {
  if (!fs || !fs.length) return '<div style="color:var(--dim);font-size:10px;padding:3px 6px">(empty)</div>';
  var h = '';
  for (var i = 0; i < fs.length; i++) {
    var f = fs[i];
    if (f.err) { h += '<div class="pf"><span class="pfn" style="color:var(--ac3)">ERR</span><span class="pvl">' + es(f.err) + '</span></div>'; continue; }
    var fn = 'f' + f.fn, ts, vh;
    if (f.t === 'vi') {
      ts = 'vi';
      var d = f.v !== null ? '' + f.v : 'lo=0x' + f.lo.toString(16) + ' hi=0x' + f.hi.toString(16);
      var x = f.n1 ? ' <span style="color:var(--dim)">(-1)</span>' : f.v !== null ? ' <span style="color:var(--dim)">[0x' + f.v.toString(16).toUpperCase() + ']</span>' : '';
      vh = '<span class="pvl">' + d + x + '</span>';
    } else if (f.t === 'f64') {
      ts = 'f64'; vh = '<span class="pvl" style="color:var(--pr)">' + bh(f.b) + '</span>';
    } else if (f.t === 'f32') {
      ts = 'f32'; vh = '<span class="pvl">' + f.v + '</span>';
    } else if (f.t === 'ld') {
      ts = 'b[' + f.l + ']';
      vh = f.nf ? '<span class="pvl" style="color:var(--dim)">[' + (f.snappy ? 'snappy ' : 'nest ') + f.nf.length + ']</span>' :
        '<span class="pvl" style="color:var(--pr)">' + (f.l > 40 ? bh(f.rb).substr(0, 120) + '...' : bh(f.rb)) + '</span>';
    } else { ts = f.t; vh = '<span class="pvl">?</span>'; }
    h += '<div class="pf"><span class="pfn">' + fn + '</span><span class="pty">' + ts + '</span>' + vh + '</div>';
    if (f.nf) h += '<div class="np">' + rpf(f.nf) + '</div>';
  }
  return h;
}

// ================================================================
// VALIDATION
// ================================================================
function validate() {
  if (!S) return { warns: [], errs: [] };
  var warns = [], errs = [];

  // Seq check
  for (var i = 0; i < S.en.length; i++) {
    if (S.en[i].seq !== i)
      errs.push('Entry #' + i + ': seq is ' + S.en[i].seq + ', expected ' + i);
  }

  // Tick ordering (strict increase for non-empty)
  var lastTick = -1, lastIdx = -1;
  for (var i = 0; i < S.en.length; i++) {
    if (!S.en[i].emp) {
      if (S.en[i].tick < 0)
        warns.push('Entry #' + i + ': tick is negative (' + S.en[i].tick + ')');
      else if (lastTick >= 0 && S.en[i].tick <= lastTick)
        warns.push('Entry #' + i + ': tick ' + S.en[i].tick + ' must be > entry #' + lastIdx + ' tick ' + lastTick);
      lastTick = S.en[i].tick;
      lastIdx = i;
    }
  }

  // Entity checks
  for (var i = 0; i < S.en.length; i++) {
    var ei = S.en[i].ei;
    if (!ei) {
      warns.push('Entry #' + i + ': missing entity info');
    } else {
      if (ei.tp !== 1 && ei.tp !== 2)
        warns.push('Entry #' + i + ': unknown entity type ' + ei.tp);
    }
  }

  // No entries (only an error if we're in BattleOp mode)
  if (S.hasBop && S.en.length === 0) errs.push('No BattleOp entries present');

  // Generic field checks
  if (!S.hasBop && S.genFields) {
    for (var i = 0; i < S.genFields.length; i++) {
      var gf = S.genFields[i];
      if (gf.fn < 1 || gf.fn > 536870911)
        errs.push('Field #' + i + ': invalid field number ' + gf.fn);
    }
  }

  return { warns: warns, errs: errs };
}

// ================================================================
// BUILD / ENCODE
// ================================================================
function buildBop() {
  var chunks = [];
  for (var i = 0; i < S.en.length; i++) {
    var e = S.en[i];
    var eb = encEnt(e.ei);
    var pb = encProtoEntry(e.itm, eb, e.seq, e.tick, e.emp);
    chunks.push(encBopEntry(e.hd, pb));
  }
  var tot = 0;
  for (var i = 0; i < chunks.length; i++) tot += chunks[i].length;
  var out = new Uint8Array(tot), off = 0;
  for (var i = 0; i < chunks.length; i++) { out.set(chunks[i], off); off += chunks[i].length; }
  return out;
}

// Encode an array of proto fields to bytes
function encodeFieldsToBytes(fields) {
  var o = [];
  for (var i = 0; i < fields.length; i++) {
    var f = fields[i];
    if (f.t === 'vi') {
      o.push.apply(o, etag(f.fn, 0));
      if (f.n1) { o.push(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01); }
      else if (f.hi > 0) { o.push.apply(o, ev64(f.lo, f.hi)); }
      else { o.push.apply(o, ev(f.v)); }
    } else if (f.t === 'ld') {
      var rb = f.sub ? encodeFieldsToBytes(f.sub) : f.rb;
      o.push.apply(o, etag(f.fn, 2));
      o.push.apply(o, ev(rb.length));
      for (var j = 0; j < rb.length; j++) o.push(rb[j]);
    } else if (f.t === 'f32') {
      o.push.apply(o, etag(f.fn, 5));
      for (var j = 0; j < 4; j++) o.push(f.b[j]);
    } else if (f.t === 'f64') {
      o.push.apply(o, etag(f.fn, 1));
      for (var j = 0; j < 8; j++) o.push(f.b[j]);
    }
  }
  return new Uint8Array(o);
}

// Decode bytes to display string for a given encoding
function decodeBytes(rb, enc) {
  if (enc === 'utf8') {
    try { return new TextDecoder('utf-8').decode(rb); } catch(e) { return '(decode error)'; }
  }
  if (enc === 'utf16') {
    try { return new TextDecoder('utf-16le').decode(rb); } catch(e) { return '(decode error)'; }
  }
  if (enc === 'ascii') {
    var s = '';
    for (var i = 0; i < rb.length; i++) s += rb[i] >= 0x20 && rb[i] < 0x7F ? String.fromCharCode(rb[i]) : '.';
    return s;
  }
  return bh(rb);
}

// Encode a display string back to bytes for a given encoding
function encodeStr(str, enc) {
  if (enc === 'utf8') return new TextEncoder().encode(str);
  if (enc === 'utf16') {
    var buf = new ArrayBuffer(str.length * 2);
    var view = new Uint16Array(buf);
    for (var i = 0; i < str.length; i++) view[i] = str.charCodeAt(i);
    return new Uint8Array(buf);
  }
  if (enc === 'ascii') {
    var out = new Uint8Array(str.length);
    for (var i = 0; i < str.length; i++) out[i] = str.charCodeAt(i) & 0xFF;
    return out;
  }
  return pib(str, 'hex') || new Uint8Array(0);
}

// Get field array for a given level: 'gen' = outermost genFields, number = bopPath[lvl].fields
function getFA(lvl) {
  return lvl === 'gen' ? S.genFields : S.bopPath[lvl].fields;
}

// Resolve a field path [i] or [i,j,...] to { fa: parentArray, idx: lastIndex }
function resolveFA(lvl, path) {
  var fa = getFA(lvl);
  for (var p = 0; p < path.length - 1; p++) fa = fa[path[p]].sub;
  return { fa: fa, idx: path[path.length - 1] };
}

// Get the field array at a parent path ([] = top level, [i] = field i's sub, etc.)
function resolveFAParent(lvl, parentPath) {
  var fa = getFA(lvl);
  for (var p = 0; p < parentPath.length; p++) fa = fa[parentPath[p]].sub;
  return fa;
}

function setFieldFn(lvl, path, val) {
  var r = resolveFA(lvl, path);
  r.fa[r.idx].fn = val;
  renderEd();
}

// Parse an LD field's raw bytes as proto sub-fields
function parseAsSub(lvl, path) {
  var r = resolveFA(lvl, path);
  var f = r.fa[r.idx];
  if (f.t !== 'ld' || !f.rb || f.rb.length < 2) return;
  try {
    var parsed = dpb(f.rb, 0, f.rb.length);
    if (parsed.length > 0 && !parsed.some(function(x) { return x.err; })) {
      var subs = [];
      for (var k = 0; k < parsed.length; k++) subs.push(makeGenField(parsed[k]));
      if (subs.length > 0) f.sub = subs;
    }
  } catch(e) {}
  renderEd();
}

// Flatten sub-fields back to raw bytes
function flattenToRaw(lvl, path) {
  var r = resolveFA(lvl, path);
  var f = r.fa[r.idx];
  if (f.sub) {
    f.rb = encodeFieldsToBytes(f.sub);
    delete f.sub;
  }
  renderEd();
}

function buildFull() {
  var payload = buildBop();
  // Build from deepest nesting level to outermost
  for (var lvl = S.bopPath.length - 1; lvl >= 0; lvl--) {
    var level = S.bopPath[lvl];
    var siblings = lvl === 0 ? S.genFields : level.fields;
    var allFields = siblings.slice();
    allFields.push({ fn: level.fn, t: 'ld', rb: payload });
    allFields.sort(function(a, b) { return a.fn - b.fn; });
    payload = encodeFieldsToBytes(allFields);
  }
  return payload;
}

// Encode generic proto fields (non-BattleOp packets)
function buildGenericProto() {
  return encodeFieldsToBytes(S.genFields);
}

function buildFrame() {
  var pb = S.hasBop ? buildFull() : buildGenericProto();
  var fl = 16 + pb.length; // frame_len = 4 header fields (16 bytes) + payload
  var out = new Uint8Array(4 + fl);
  w32b(out, 0, fl);
  w32b(out, 4, S.frame.token);
  w32b(out, 8, S.frame.route);
  w32b(out, 12, S.frame.flags);
  w32b(out, 16, S.frame.msgId);
  out.set(pb, 20);
  return out;
}

// ================================================================
// EDITOR RENDER
// ================================================================
function renderEd() {
  if (!S) return;
  var o = document.getElementById('results'), h = '';
  var v = validate();

  // ── Card 1: Transport Frame ──
  h += '<div class="card"><div class="chd" onclick="tog(\'ef\')"><span class="cidx frame">TRANSPORT</span>';
  h += '<span class="csum">';
  if (S.frame) {
    h += '<span>tok=' + th(S.frame.token) + '</span>';
    h += '<span>route=' + th(S.frame.route) + '</span>';
    h += '<span>msgId=' + th(S.frame.msgId) + '</span>';
  } else {
    h += '<span style="color:var(--dim)">' + (S.wrapFrame ? 'Frame wrapper enabled' : 'No frame (proto only)') + '</span>';
  }
  h += '</span><span class="tarr open" id="ef-a">&#9654;</span></div>';
  h += '<div class="coll open" id="ef"><div class="cbod">';

  if (!S.frame) {
    h += '<div class="chk-row"><input type="checkbox" id="wrapFrameChk" ' + (S.wrapFrame ? 'checked' : '') + ' onchange="toggleFrameWrap(this.checked)"><label for="wrapFrameChk">Wrap output in transport frame</label></div>';
    if (S.wrapFrame) {
      h += '<div class="fgrid" style="margin-top:8px">';
      h += hfbox('Token', S._wf.token, 'S._wf.token=phd(this.value);renderEd()');
      h += hfbox('Route', S._wf.route, 'S._wf.route=phd(this.value);renderEd()');
      h += hfbox('Flags', S._wf.flags, 'S._wf.flags=phd(this.value);renderEd()');
      h += hfbox('MsgId', S._wf.msgId, 'S._wf.msgId=phd(this.value);renderEd()');
      h += '</div>';
    }
  } else {
    h += '<div class="fgrid">';
    h += hfbox('Token', S.frame.token, 'S.frame.token=phd(this.value);renderEd()');
    h += hfbox('Route', S.frame.route, 'S.frame.route=phd(this.value);renderEd()');
    h += hfbox('Flags', S.frame.flags, 'S.frame.flags=phd(this.value);renderEd()');
    h += hfbox('MsgId', S.frame.msgId, 'S.frame.msgId=phd(this.value);renderEd()');
    h += '</div>';
    h += '<div style="font-size:9px;color:var(--dim);margin-top:4px">frame_len will be auto-computed on export. All values are BE u32.</div>';
  }
  h += '</div></div></div>';

  // ── Card 2: Outer Protobuf ──
  if (S.hasBop) {
    // BattleOp mode: show other proto fields + BattleOp path indicator
    var pathStr = S.bopPath.map(function(p) { return 'f' + p.fn; }).join(' \u2192 ');
    h += '<div class="card"><div class="chd" onclick="tog(\'eo\')"><span class="cidx">OUTER PROTO</span>';
    h += '<span class="csum"><span>' + S.genFields.length + ' fields + BattleOp (' + pathStr + ')</span></span>';
    h += '<span class="tarr open" id="eo-a">&#9654;</span></div>';
    h += '<div class="coll open" id="eo"><div class="cbod">';
    h += '<div style="font-size:10px;color:var(--ac);margin-bottom:8px">BattleOp auto-detected at: <b>' + pathStr + '</b>';
    h += ' <span style="color:var(--dim)">(depth ' + S.bopPath.length + ')</span>';
    h += '</div>';

    // Outermost level fields
    h += '<div style="font-size:11px;color:var(--ac);margin-bottom:4px;font-weight:bold">Outer level <input type="number" value="' + S.bopPath[0].fn + '" onchange="S.bopPath[0].fn=+this.value;renderEd()" style="width:40px;display:inline;font-size:10px" title="BattleOp field number at this level"> = BattleOp</div>';
    h += renderGenericFields('gen');

    // Nested intermediate levels
    for (var nlvl = 1; nlvl < S.bopPath.length; nlvl++) {
      h += '<div style="margin-top:12px;padding-top:8px;border-top:1px solid var(--bd)">';
      h += '<div style="font-size:11px;color:var(--ac2);margin-bottom:4px;font-weight:bold">Nested level ' + nlvl + ' (inside f' + S.bopPath[nlvl - 1].fn + ') <input type="number" value="' + S.bopPath[nlvl].fn + '" onchange="S.bopPath[' + nlvl + '].fn=+this.value;renderEd()" style="width:40px;display:inline;font-size:10px" title="BattleOp field number at this level"> = BattleOp</div>';
      h += renderGenericFields(nlvl);
      h += '</div>';
    }

    h += '</div></div></div>';
  } else {
    // Generic proto mode: show all decoded fields
    h += '<div class="card"><div class="chd" onclick="tog(\'eo\')"><span class="cidx">PROTO FIELDS</span>';
    h += '<span class="csum"><span>' + S.genFields.length + ' fields</span></span>';
    h += '<span class="tarr open" id="eo-a">&#9654;</span></div>';
    h += '<div class="coll open" id="eo"><div class="cbod">';
    h += renderGenericFields('gen');
    h += '</div></div></div>';
  }

  // ── Card 3: BattleOp Entries (only if hasBop) ──
  if (S.hasBop) {
    h += '<div class="card"><div class="chd" onclick="tog(\'eb\')"><span class="cidx">BATTLEOP</span>';
    h += '<span class="csum"><span>' + S.en.length + ' entries</span></span>';
    h += '<span class="tarr open" id="eb-a">&#9654;</span></div>';
    h += '<div class="coll open" id="eb"><div class="cbod">';

    // Summary table
    h += '<table class="bt"><thead><tr><th>#</th><th>Tick</th><th>~Sec</th><th>Item</th><th>Hdr</th><th>Type</th><th>UUID</th><th>Detail</th><th style="width:160px">Actions</th></tr></thead><tbody>';
    for (var i = 0; i < S.en.length; i++) {
      var e = S.en[i];
      var tc = e.emp ? 'se' : 'sf', tv = e.emp ? 'EMPTY' : e.tick;
      var ts = e.emp ? '-' : (e.tick * .04).toFixed(1) + 's';
      var ei = e.ei, tn = ei ? ei.tn : '?';
      var bg = tn === 'hero' ? 'eh' : tn === 'spell' ? 'es' : 'eu';
      var det = '';
      if (tn === 'hero') det = ei.ab ? '<span style="color:var(--ac)">auto:ON</span>' : 'auto:OFF';
      else if (tn === 'spell') det = 'vA=' + ei.vA + ' vB=' + ei.vB;

      var warn = '';
      if (!e.emp && i > 0) {
        for (var pi = i - 1; pi >= 0; pi--) {
          if (!S.en[pi].emp) {
            if (e.tick <= S.en[pi].tick) warn = ' <span class="warn">&#9888;</span>';
            break;
          }
        }
      }
      var uuid_short = ei ? ei.uh.replace(/ /g, '').substring(0, 8) + '..' : '?';

      h += '<tr>';
      h += '<td class="si">' + i + '</td>';
      h += '<td class="' + tc + '">' + tv + warn + '</td>';
      h += '<td style="color:var(--dim)">' + ts + '</td>';
      h += '<td>' + e.itm + '</td>';
      h += '<td style="color:var(--dim)">' + e.hd + '</td>';
      h += '<td><span class="et ' + bg + '">' + tn + '</span></td>';
      h += '<td style="font-size:10px;color:var(--dim)">' + uuid_short + '</td>';
      h += '<td style="font-size:10px">' + det + '</td>';
      h += '<td>';
      h += '<button class="bmove" onclick="moveE(' + i + ',-1)" ' + (i === 0 ? 'disabled' : '') + ' title="Move up">&#9650;</button> ';
      h += '<button class="bmove" onclick="moveE(' + i + ',1)" ' + (i === S.en.length - 1 ? 'disabled' : '') + ' title="Move down">&#9660;</button> ';
      h += '<button class="bk bs" onclick="cloneE(' + i + ')">Clone</button> ';
      h += '<button class="bd bs" onclick="delE(' + i + ')">Del</button>';
      h += '</td></tr>';
    }
    h += '</tbody></table>';

    h += '<div class="add-row">';
    h += '<button class="ba" onclick="addE(\'hero\')">+ Hero</button>';
    h += '<button class="ba" onclick="addE(\'spell\')">+ Spell</button>';
    h += '<button class="ba" style="background:var(--ac4)" onclick="addE(\'clone\')">+ Clone Last</button>';
    h += '</div>';

    h += '<div class="stit click" onclick="tog(\'ee\')">Per-Entry Editor &#9660;</div>';
    h += '<div class="coll" id="ee">';
    for (var i = 0; i < S.en.length; i++) {
      h += renderEntryEditor(i);
    }
    h += '</div>';
    h += '</div></div></div>'; // end BattleOp card body
  }

  // ── Export & Preview card ──
  h += '<div class="card"><div class="chd" onclick="tog(\'exs\')"><span class="cidx exp">EXPORT</span>';
  h += '<span class="csum"><span>Export &amp; Hex Preview</span></span>';
  h += '<span class="tarr" id="exs-a">&#9654;</span></div>';
  h += '<div class="coll" id="exs"><div class="cbod">';

  h += '<div class="fmt-tog">';
  h += '<button class="fmt-btn' + (EF === 'hex' ? ' on' : '') + '" data-f="hex" onclick="setEF(this)">Hex (spaced)</button>';
  h += '<button class="fmt-btn' + (EF === 'hexc' ? ' on' : '') + '" data-f="hexc" onclick="setEF(this)">Hex (compact)</button>';
  h += '<button class="fmt-btn' + (EF === 'base64' ? ' on' : '') + '" data-f="base64" onclick="setEF(this)">Base64</button>';
  h += '<button class="fmt-btn' + (EF === 'carray' ? ' on' : '') + '" data-f="carray" onclick="setEF(this)">C Array</button>';
  h += '</div>';

  h += '<div class="exp-row">';
  if (S.hasBop)
    h += '<button class="bx" onclick="doExport(\'bop\')">Export BattleOp</button>';
  h += '<button class="bx" onclick="doExport(\'proto\')">Export Proto</button>';
  if (S.frame || S.wrapFrame)
    h += '<button class="bx" style="background:var(--ac4);color:#0a0c10" onclick="doExport(\'frame\')">Export Full Packet</button>';
  h += '</div>';
  h += '<div id="expout"></div>';

  h += '<div class="stit click" onclick="tog(\'hprev\')">Hex Dump Preview &#9660;</div>';
  h += '<div class="coll" id="hprev"><div id="hprev-content"></div></div>';

  h += '</div></div></div>'; // end export card

  // ── Card 4: Validation ──
  h += '<div class="card"><div class="chd" onclick="tog(\'ev\')"><span class="cidx val">VALIDATION</span>';
  var totalIssues = v.errs.length + v.warns.length;
  if (totalIssues === 0) h += '<span class="csum"><span style="color:var(--ac)">All checks passed</span></span>';
  else h += '<span class="csum"><span style="color:var(--ac3)">' + v.errs.length + ' errors</span><span style="color:var(--ac4)">' + v.warns.length + ' warnings</span></span>';
  h += '<span class="tarr' + (totalIssues > 0 ? ' open' : '') + '" id="ev-a">&#9654;</span></div>';
  h += '<div class="coll' + (totalIssues > 0 ? ' open' : '') + '" id="ev"><div class="cbod"><div class="vlist">';

  if (v.errs.length === 0 && v.warns.length === 0) {
    h += '<div class="vitem vok"><span class="vicon">&#10003;</span> Packet structure is valid</div>';
  }
  for (var i = 0; i < v.errs.length; i++)
    h += '<div class="vitem verr"><span class="vicon">&#10007;</span> ' + es(v.errs[i]) + '</div>';
  for (var i = 0; i < v.warns.length; i++)
    h += '<div class="vitem vwarn"><span class="vicon">&#9888;</span> ' + es(v.warns[i]) + '</div>';

  h += '</div></div></div></div>';

  o.innerHTML = h;
}

// Render a single entry's editor card
function renderEntryEditor(i) {
  var e = S.en[i], ei = e.ei;
  var tn = ei ? ei.tn : '?';
  var bg = tn === 'hero' ? 'eh' : tn === 'spell' ? 'es' : 'eu';
  var h = '';

  h += '<div class="ecard"><div class="ehd"><span class="ei">#' + i + '</span>';
  h += '<span class="et ' + bg + '">' + tn + '</span>';
  h += '<div class="eact">';
  h += '<button class="bk bs" onclick="cloneE(' + i + ')">Clone After</button>';
  h += '<button class="bd bs" onclick="delE(' + i + ')">Delete</button>';
  h += '</div></div>';

  // Entry framing
  h += erow('Header', e.hd, 'S.en[' + i + '].hd=+this.value');
  h += erow('Item ID', e.itm, 'S.en[' + i + '].itm=+this.value');
  h += '<div class="er"><label>Seq (f4)</label><input type="number" value="' + e.seq + '" disabled style="opacity:.5" title="Auto-managed (0,1,2...)"></div>';
  h += erow('Tick (f5)', e.emp ? -1 : e.tick, 'setTick(' + i + ',+this.value)');
  h += '<div class="er"><label style="min-width:70px"></label><span class="ihelp">Tick -1 = empty/unscheduled. ~25 ticks/sec (0.04s per tick)</span></div>';

  if (ei) {
    // UUID
    h += '<div class="er"><label>UUID</label><input type="text" class="w" value="' + ei.uh.replace(/ /g, '') + '" onchange="setUuid(' + i + ',this.value)"></div>';

    // Entity type switcher
    h += '<div class="er"><label>Ent Type</label><select onchange="changeType(' + i + ',this.value)">';
    h += '<option value="hero"' + (tn === 'hero' ? ' selected' : '') + '>Hero (1)</option>';
    h += '<option value="spell"' + (tn === 'spell' ? ' selected' : '') + '>Spell (2)</option>';
    h += '</select></div>';

    if (tn === 'hero') {
      h += '<div class="er"><label>AutoBattle</label><select onchange="S.en[' + i + '].ei.ab=+this.value;renderEd()">';
      h += '<option value="0"' + (ei.ab ? '' : ' selected') + '>OFF (0)</option>';
      h += '<option value="1"' + (ei.ab ? ' selected' : '') + '>ON (1)</option>';
      h += '</select></div>';
    } else if (tn === 'spell') {
      h += erow('Marker', ei.mk, 'S.en[' + i + '].ei.mk=+this.value');
      h += erow('field1', ei.f1, 'S.en[' + i + '].ei.f1=+this.value');
      h += erow('valueA', ei.vA, 'S.en[' + i + '].ei.vA=+this.value');
      h += erow('valueB', ei.vB, 'S.en[' + i + '].ei.vB=+this.value');
      h += '<div class="er"><label>Flags</label>';
      h += 'C:<input type="number" value="' + ei.fC + '" onchange="S.en[' + i + '].ei.fC=+this.value" style="width:45px"> ';
      h += 'D:<input type="number" value="' + ei.fD + '" onchange="S.en[' + i + '].ei.fD=+this.value" style="width:45px"> ';
      h += 'E:<input type="number" value="' + ei.fE + '" onchange="S.en[' + i + '].ei.fE=+this.value" style="width:45px"></div>';
      h += erow('Sentinel', ei.sn, 'S.en[' + i + '].ei.sn=+this.value');
    }

    // Raw entity hex (collapsible)
    h += '<div class="stit click" onclick="tog(\'rh' + i + '\')">Raw Entity Hex &#9660;</div>';
    h += '<div class="coll" id="rh' + i + '">';
    var entBytes = encEnt(ei);
    h += '<textarea class="raw-hex-area" onchange="setRawEnt(' + i + ',this.value)">' + bh(entBytes) + '</textarea>';
    h += '<div style="font-size:9px;color:var(--dim)">' + entBytes.length + ' bytes | Type ' + ei.tp + ' | Size field: ' + r32l(entBytes, 0) + '</div>';
    h += '</div>';

    // Raw proto fields (collapsible)
    if (e.fs && e.fs.length) {
      h += '<div class="stit click" onclick="tog(\'rf' + i + '\')">Raw Proto Fields &#9660;</div>';
      h += '<div class="coll" id="rf' + i + '">' + rpf(e.fs) + '</div>';
    }
  }

  h += '</div>';
  return h;
}

// Render generic proto fields editor for a given level + path
// lvl = 'gen' | bopPath index;  parentPath = [] for top level, [i] for sub-fields, etc.
function renderGenericFields(lvl, parentPath, depth) {
  if (!parentPath) parentPath = [];
  if (!depth) depth = 0;
  if (depth > 5) return '<span style="color:var(--dim);font-size:10px">(max nesting depth)</span>';

  var fa = parentPath.length === 0 ? getFA(lvl) : resolveFAParent(lvl, parentPath);
  var lq = typeof lvl === 'string' ? "'" + lvl + "'" : lvl;
  var ppStr = JSON.stringify(parentPath);
  var h = '';
  var types = { vi: 'Varint', ld: 'Bytes', f32: 'Fixed32', f64: 'Fixed64' };

  h += '<table class="bt"><thead><tr><th>Field</th><th>Wire</th><th>Value</th><th>Hex</th><th style="width:80px">Actions</th></tr></thead><tbody>';
  for (var i = 0; i < fa.length; i++) {
    var f = fa[i];
    var fp = JSON.stringify(parentPath.concat([i]));
    h += '<tr>';
    h += '<td><input type="number" value="' + f.fn + '" onchange="setFieldFn(' + lq + ',' + fp + ',+this.value)" style="width:50px"></td>';
    h += '<td style="color:var(--dim)">' + (types[f.t] || f.t) + '</td>';

    if (f.t === 'vi') {
      var hexDisp = f.n1 ? '0xFFFFFFFFFFFFFFFF' : (f.hi > 0 ? '0x' + f.hi.toString(16).toUpperCase().padStart(8,'0') + f.lo.toString(16).toUpperCase().padStart(8,'0') : th(f.v));
      h += '<td><input type="text" value="' + (f.n1 ? -1 : f.v) + '" onchange="setGenVI(' + lq + ',' + fp + ',this.value)" style="width:130px"></td>';
      h += '<td style="font-size:10px;color:var(--dim)">' + hexDisp + '</td>';
    } else if (f.t === 'ld') {
      if (f.sub && f.sub.length > 0) {
        h += '<td colspan="2"><span style="font-size:10px;color:var(--ac)">[nested proto, ' + f.sub.length + ' fields]</span>';
        h += ' <button class="bs" style="font-size:9px;padding:1px 4px" onclick="flattenToRaw(' + lq + ',' + fp + ')">raw hex</button></td>';
      } else {
        var enc = f.enc || 'hex';
        h += '<td colspan="2">';
        h += '<select onchange="setLdEnc(' + lq + ',' + fp + ',this.value)" style="font-size:9px;margin-bottom:2px;padding:1px 2px">';
        h += '<option value="hex"' + (enc === 'hex' ? ' selected' : '') + '>Hex</option>';
        h += '<option value="utf8"' + (enc === 'utf8' ? ' selected' : '') + '>UTF-8</option>';
        h += '<option value="utf16"' + (enc === 'utf16' ? ' selected' : '') + '>UTF-16LE (wchar)</option>';
        h += '<option value="ascii"' + (enc === 'ascii' ? ' selected' : '') + '>ASCII</option>';
        h += '</select>';
        if (enc === 'hex') {
          h += '<textarea class="raw-hex-area" style="height:30px" onchange="setGenLD(' + lq + ',' + fp + ',this.value)">' + bh(f.rb) + '</textarea>';
        } else {
          h += '<textarea class="raw-hex-area" style="height:30px" onchange="setGenLDStr(' + lq + ',' + fp + ',this.value,\'' + enc + '\')">' + es(decodeBytes(f.rb, enc)) + '</textarea>';
        }
        h += '<span style="font-size:9px;color:var(--dim)">' + f.rb.length + ' bytes</span>';
        if (f.rb.length >= 2) h += ' <button class="bs" style="font-size:9px;padding:1px 4px" onclick="parseAsSub(' + lq + ',' + fp + ')">parse</button>';
        h += '</td>';
      }
    } else if (f.t === 'f32') {
      h += '<td><input type="text" value="' + bh(f.b) + '" onchange="setGenF(' + lq + ',' + fp + ',this.value,4)" style="width:100px"></td>';
      h += '<td style="font-size:10px;color:var(--dim)">u32=' + f.v + '</td>';
    } else if (f.t === 'f64') {
      h += '<td><input type="text" value="' + bh(f.b) + '" onchange="setGenF(' + lq + ',' + fp + ',this.value,8)" style="width:180px"></td>';
      h += '<td></td>';
    }

    h += '<td><button class="bd bs" onclick="delGenField(' + lq + ',' + fp + ')">Del</button></td>';
    h += '</tr>';

    // Render nested sub-fields for LD with sub
    if (f.t === 'ld' && f.sub && f.sub.length > 0) {
      h += '<tr><td colspan="5" style="padding:0 0 0 20px;border-top:none">';
      h += renderGenericFields(lvl, parentPath.concat([i]), depth + 1);
      h += '</td></tr>';
    }
  }
  h += '</tbody></table>';

  // Add field
  h += '<div class="add-row">';
  h += '<button class="ba" onclick="addGenField(' + lq + ',' + ppStr + ',\'vi\')">+ Varint</button>';
  h += '<button class="ba" onclick="addGenField(' + lq + ',' + ppStr + ',\'ld\')">+ Bytes</button>';
  h += '<button class="ba" style="background:var(--ac4)" onclick="addGenField(' + lq + ',' + ppStr + ',\'f32\')">+ Fixed32</button>';
  h += '<button class="ba" style="background:var(--pr)" onclick="addGenField(' + lq + ',' + ppStr + ',\'f64\')">+ Fixed64</button>';
  h += '</div>';

  return h;
}

// Generic field edit operations (lvl = 'gen' or bopPath index; path = [i] or [i,j,...])
function setGenVI(lvl, path, val) {
  var r = resolveFA(lvl, path);
  var f = r.fa[r.idx];
  val = val.trim();
  if (val === '-1') {
    f.v = 0; f.n1 = true; f.lo = 0; f.hi = 0;
  } else {
    var n;
    if (val.match(/^0[xX]/)) n = parseInt(val, 16);
    else n = parseFloat(val);
    f.n1 = false; f.v = n;
    if (n > 0xFFFFFFFF) {
      f.hi = Math.floor(n / 0x100000000) >>> 0;
      f.lo = (n - f.hi * 0x100000000) >>> 0;
    } else {
      f.lo = n >>> 0; f.hi = 0;
    }
  }
  renderEd();
}

function setGenLD(lvl, path, hexStr) {
  var r = resolveFA(lvl, path);
  var bytes = pib(hexStr, 'hex');
  if (bytes) {
    r.fa[r.idx].rb = bytes;
    delete r.fa[r.idx].sub;
  }
  renderEd();
}

// Update LD field from a string in the given encoding
function setGenLDStr(lvl, path, str, enc) {
  var r = resolveFA(lvl, path);
  var bytes = encodeStr(str, enc);
  r.fa[r.idx].rb = bytes;
  delete r.fa[r.idx].sub;
  renderEd();
}

// Change display encoding for an LD field
function setLdEnc(lvl, path, enc) {
  var r = resolveFA(lvl, path);
  r.fa[r.idx].enc = enc;
  renderEd();
}

function setGenF(lvl, path, hexStr, sz) {
  var r = resolveFA(lvl, path);
  var f = r.fa[r.idx];
  var bytes = pib(hexStr, 'hex');
  if (bytes && bytes.length === sz) {
    f.b = bytes;
    if (sz === 4) f.v = r32l(bytes, 0);
  }
  renderEd();
}

function delGenField(lvl, path) {
  var r = resolveFA(lvl, path);
  r.fa.splice(r.idx, 1);
  renderEd();
}

function addGenField(lvl, parentPath, type) {
  var fa = resolveFAParent(lvl, parentPath);
  var maxFn = 0;
  for (var i = 0; i < fa.length; i++) {
    if (fa[i].fn > maxFn) maxFn = fa[i].fn;
  }
  var gf = { fn: maxFn + 1, t: type };
  if (type === 'vi') { gf.wt = 0; gf.v = 0; gf.lo = 0; gf.hi = 0; gf.n1 = false; }
  else if (type === 'ld') { gf.wt = 2; gf.rb = new Uint8Array(0); }
  else if (type === 'f32') { gf.wt = 5; gf.b = new Uint8Array(4); gf.v = 0; }
  else if (type === 'f64') { gf.wt = 1; gf.b = new Uint8Array(8); }
  fa.push(gf);
  renderEd();
}

// Helper: hex field box for frame editor
function hfbox(label, val, onch) {
  return '<div class="fbox"><div class="fl">' + label + '</div>' +
    '<div class="hex-inp"><input type="text" value="' + th(val) + '" onchange="' + onch + '"></div>' +
    '<div style="font-size:9px;color:var(--dim);margin-top:2px">dec: ' + val + '</div></div>';
}

// Helper: entry row
function erow(lbl, val, onch) {
  return '<div class="er"><label>' + lbl + '</label><input type="number" value="' + val + '" onchange="' + onch + ';renderEd()"></div>';
}

// ================================================================
// EDIT OPERATIONS
// ================================================================
function setTick(i, v) {
  if (v < 0) { S.en[i].emp = true; S.en[i].tick = 0; }
  else { S.en[i].emp = false; S.en[i].tick = v; }
  renderEd();
}

function setUuid(i, hex) {
  hex = hex.replace(/\s/g, '');
  if (hex.length !== 16) return;
  var u = new Uint8Array(8);
  for (var j = 0; j < 8; j++) u[j] = parseInt(hex.substr(j * 2, 2), 16);
  S.en[i].ei.uuid = u;
  S.en[i].ei.uh = bh(u);
  renderEd();
}

function setRawEnt(i, hexStr) {
  var bytes = pib(hexStr, 'hex');
  if (!bytes || bytes.length < 16) return;
  S.en[i].ei = decEnt(bytes);
  S.en[i].ei.uuid = new Uint8Array(S.en[i].ei.uuid);
  renderEd();
}

function changeType(i, newType) {
  var old = S.en[i].ei;
  var uuid = old ? new Uint8Array(old.uuid) : new Uint8Array(8);
  var uh = old ? old.uh : bh(uuid);
  if (newType === 'hero') {
    S.en[i].ei = { tn: 'hero', tp: 1, sz: 17, uuid: uuid, uh: uh, ab: 0 };
  } else {
    S.en[i].ei = { tn: 'spell', tp: 2, sz: 47, uuid: uuid, uh: uh, mk: 82, f1: -1, vA: 0, vB: 0, fC: 0, fD: 0, fE: 0, sn: -1 };
  }
  renderEd();
}

function reseq() { for (var i = 0; i < S.en.length; i++) S.en[i].seq = i; }

function moveE(idx, dir) {
  var ni = idx + dir;
  if (ni < 0 || ni >= S.en.length) return;
  var tmp = S.en[idx];
  S.en[idx] = S.en[ni];
  S.en[ni] = tmp;
  reseq();
  renderEd();
}

function cloneE(idx) {
  var src = S.en[idx];
  var nei = null;
  if (src.ei) {
    nei = JSON.parse(JSON.stringify(src.ei));
    nei.uuid = new Uint8Array(src.ei.uuid);
  }
  var prevT = src.emp ? 0 : src.tick;
  var newT = prevT + 25;
  if (idx + 1 < S.en.length) {
    var nx = S.en[idx + 1];
    if (!nx.emp && nx.tick > prevT) {
      newT = Math.floor((prevT + nx.tick) / 2);
      if (newT <= prevT) newT = prevT + 1;
    }
  }
  S.en.splice(idx + 1, 0, { hd: src.hd, itm: src.itm, seq: 0, tick: newT, emp: false, ei: nei, fs: [] });
  reseq();
  renderEd();
}

function delE(idx) {
  if (S.en.length <= 1) return;
  S.en.splice(idx, 1);
  reseq();
  renderEd();
}

function addE(type) {
  var last = S.en.length > 0 ? S.en[S.en.length - 1] : null;
  var lt = last ? (last.emp ? 0 : last.tick) : 0;
  var newTick = lt + 25;
  var hd = last ? last.hd : 2502;
  var itm = last ? last.itm : 0;

  if (type === 'clone' && last) {
    cloneE(S.en.length - 1);
    return;
  }

  var nei;
  if (type === 'hero') {
    nei = { tn: 'hero', tp: 1, sz: 17, uuid: new Uint8Array(8), uh: '00 00 00 00 00 00 00 00', ab: 0 };
    // Copy UUID from last if available
    if (last && last.ei) { nei.uuid = new Uint8Array(last.ei.uuid); nei.uh = last.ei.uh; }
  } else {
    nei = { tn: 'spell', tp: 2, sz: 47, uuid: new Uint8Array(8), uh: '00 00 00 00 00 00 00 00', mk: 82, f1: -1, vA: 0, vB: 0, fC: 0, fD: 0, fE: 0, sn: -1 };
    if (last && last.ei) { nei.uuid = new Uint8Array(last.ei.uuid); nei.uh = last.ei.uh; }
  }

  S.en.push({ hd: hd, itm: itm, seq: S.en.length, tick: newTick, emp: false, ei: nei, fs: [] });
  renderEd();
}

function toggleFrameWrap(checked) {
  S.wrapFrame = checked;
  if (checked && !S._wf) {
    S._wf = { token: 0, route: 0x00000299, flags: 0, msgId: 0x00001940 };
  }
  if (checked) {
    S.frame = S._wf;
  } else {
    S.frame = null;
  }
  renderEd();
}

// ================================================================
// EXPORT
// ================================================================
function doExport(level) {
  var bytes;
  if (level === 'bop') bytes = buildBop();
  else if (level === 'proto') bytes = S.hasBop ? buildFull() : buildGenericProto();
  else if (level === 'frame') bytes = buildFrame();
  else return;

  var labels = { bop: 'BattleOp', proto: 'Full Proto', frame: 'Full Packet (with Frame)' };
  showExp(labels[level] + ' — ' + bytes.length + ' bytes', bytes, level);
}

function showExp(lbl, bytes, level) {
  var formatted = fmtBytes(bytes);
  var el = document.getElementById('expout');
  var h = '<div class="obox"><div class="ol">' + lbl + '</div>';
  h += '<textarea readonly onclick="this.select()">' + formatted + '</textarea>';
  h += '<div style="margin-top:4px;display:flex;gap:6px;align-items:center">';
  h += '<button class="bp bs" onclick="cpExp()">Copy</button>';
  h += '<span class="byte-ct">' + bytes.length + ' bytes</span>';
  h += '</div></div>';
  el.innerHTML = h;

  // Also render hex preview
  renderHexPreview(bytes, level);
}

function renderHexPreview(bytes, level) {
  var el = document.getElementById('hprev-content');
  if (!el) return;

  var regions = [];
  if (level === 'frame') {
    regions.push({ s: 0, e: 4, c: 'hf' });    // frame_len
    regions.push({ s: 4, e: 8, c: 'hh' });    // token
    regions.push({ s: 8, e: 12, c: 'ha' });   // route
    regions.push({ s: 12, e: 16, c: 'hp' });  // flags
    regions.push({ s: 16, e: 20, c: 'hf' });  // msgId
    // rest is proto payload (default color)
  } else if (level === 'proto') {
    regions.push({ s: 0, e: Math.min(6, bytes.length), c: 'hh' }); // outer proto fields
  }

  var h = '<div class="hx">' + (regions.length ? rhxRegions(bytes, regions) : rhx(bytes, 0)) + '</div>';

  // Legend
  if (level === 'frame') {
    h += '<div style="display:flex;gap:12px;margin-top:6px;font-size:9px;flex-wrap:wrap">';
    h += '<span><span class="hf">&#9632;</span> frame_len/msgId</span>';
    h += '<span><span class="hh">&#9632;</span> token</span>';
    h += '<span><span class="ha">&#9632;</span> route</span>';
    h += '<span><span class="hp">&#9632;</span> flags/proto</span>';
    h += '</div>';
  }

  el.innerHTML = h;
}

function cpExp() {
  var t = document.querySelector('#expout textarea');
  if (t) { t.select(); document.execCommand('copy'); }
}

// ================================================================
// TOGGLE
// ================================================================
function tog(id) {
  document.getElementById(id).classList.toggle('open');
  var a = document.getElementById(id + '-a');
  if (a) a.classList.toggle('open');
}

// ================================================================
// AUTO-DETECT & PARSE
// ================================================================
function detect(buf) {
  // Frame: first 4 bytes (BE) = frame_len; must fit in buffer, rest is trailing
  if (buf.length >= 20) {
    var fl = r32b(buf, 0);
    if (fl >= 16 && 4 + fl <= buf.length) return 'frame';
  }
  // BattleOp raw: LE body_len + header 2502
  if (buf.length >= 8) {
    var bl = r32l(buf, 0), hd = r32l(buf, 4);
    if (bl > 4 && bl < buf.length && hd === 2502) return 'bop';
  }
  return 'proto';
}

function doParse() {
  var raw = document.getElementById('inp').value, fmt = document.getElementById('inFmt').value;
  var out = document.getElementById('results');
  var bytes = pib(raw, fmt);
  if (!bytes || !bytes.length) { out.innerHTML = '<div class="err">Could not parse input.</div>'; return; }

  var mode = M === 'auto' ? detect(bytes) : M;
  var bopBuf = null, bopPathArr = null, bestBopOuterIdx = -1;
  var frame = null;
  var protoFields = [];

  if (mode === 'proto') {
    protoFields = dpb(bytes, 0, bytes.length);
    var discovery = discoverBop(protoFields);
    if (discovery) {
      bestBopOuterIdx = discovery.path[0];
      var walked = walkBopPath(protoFields, discovery.path);
      bopBuf = walked.bopBuf;
      bopPathArr = walked.bopPath;
    }
  } else if (mode === 'bop') {
    bopBuf = bytes;
    bopPathArr = [{ fn: 4 }];
  } else if (mode === 'frame') {
    var frames = decFrames(bytes);
    if (frames && frames.length > 0) {
      var fr = frames[0];
      frame = { token: fr.tok, route: fr.rt, flags: fr.fg, msgId: fr.mid };
      protoFields = fr.pf;
      var discovery = discoverBop(protoFields);
      if (discovery) {
        bestBopOuterIdx = discovery.path[0];
        var walked = walkBopPath(protoFields, discovery.path);
        bopBuf = walked.bopBuf;
        bopPathArr = walked.bopPath;
      }
    }
  }

  // Build genFields from outermost protoFields, excluding the BattleOp path entry
  var otherFields = [];
  for (var i = 0; i < protoFields.length; i++) {
    if (i === bestBopOuterIdx) continue;
    if (protoFields[i].err) continue;
    otherFields.push(makeGenField(protoFields[i]));
  }

  // No BattleOp discovered — generic proto editor
  if (!bopBuf) {
    S = { mode: mode, frame: frame, wrapFrame: false, _wf: null, hasBop: false, origBytes: bytes, en: [], genFields: otherFields };
    renderEd();
    return;
  }

  var bop = decBop(bopBuf);

  // Build edit state (BattleOp mode)
  S = { mode: mode, frame: frame, wrapFrame: false, _wf: null, bopPath: bopPathArr, hasBop: true, origBytes: bytes, en: [], genFields: otherFields };

  for (var i = 0; i < bop.es.length; i++) {
    var e = bop.es[i];
    var ei = e.ei;
    var nei = null;
    if (ei) {
      if (ei.tn === 'hero') {
        nei = { tn: 'hero', tp: 1, sz: ei.sz, uuid: new Uint8Array(ei.uuid), uh: ei.uh, ab: ei.ab || 0 };
      } else if (ei.tn === 'spell') {
        nei = { tn: 'spell', tp: 2, sz: ei.sz, uuid: new Uint8Array(ei.uuid), uh: ei.uh, mk: ei.mk, f1: ei.f1, vA: ei.vA, vB: ei.vB, fC: ei.fC, fD: ei.fD, fE: ei.fE, sn: ei.sn };
      } else {
        nei = { tn: ei.tn, tp: ei.tp, sz: ei.sz, uuid: new Uint8Array(ei.uuid), uh: ei.uh, raw: new Uint8Array(ei.raw) };
      }
    }
    S.en.push({ hd: e.hd, itm: e.itm, seq: e.seq, tick: e.tick || 0, emp: e.emp, ei: nei, fs: e.fs });
  }
  reseq();
  renderEd();
}

function doClear() {
  document.getElementById('inp').value = '';
  document.getElementById('results').innerHTML = '';
  S = null;
}

// ================================================================
// EVENT HANDLERS
// ================================================================
document.getElementById('inp').addEventListener('paste', function () { setTimeout(doParse, 80); });
document.getElementById('inp').addEventListener('keydown', function (e) {
  if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); doParse(); }
});
