// ─── State ───────────────────────────────────────────────────────────────────
var ws = null, reconnectDelay = 1000;
var packets = [], maxPackets = 5000, selectedIdx = -1;
var dirFilter = 'all', msgIdFilter = [];
var filteredPackets = [];

// ─── Message schema ──────────────────────────────────────────────────────────
var msgDefs = {}, msgSchemas = {}, msgEnums = {}, msgOverrides = {};
var schemaLoaded = false, _lastProtoMap = null;

// ─── Pre-computed hex lookup ─────────────────────────────────────────────────
var _HL = new Array(256), _HU = new Array(256);
for (var _i = 0; _i < 256; _i++) {
  _HL[_i] = ('0' + _i.toString(16)).slice(-2);
  _HU[_i] = _HL[_i].toUpperCase();
}

// ─── Schema loading ──────────────────────────────────────────────────────────
function applyOverrides() {
  var count = 0;
  for (var msgName in msgOverrides) {
    if (msgName.charAt(0) === '_') continue;
    var schema = msgSchemas[msgName];
    if (!schema || !schema.fields) continue;
    var overrides = msgOverrides[msgName];
    for (var i = 0; i < schema.fields.length; i++) {
      var f = schema.fields[i];
      var ov = overrides[f.name];
      if (!ov) continue;
      if (ov.num !== undefined) f.n = ov.num;
      if (ov.snappy !== undefined) f.snappy = ov.snappy;
      if (ov.sub !== undefined) f.sub = ov.sub;
      if (ov.type !== undefined) f.t = ov.type;
      count++;
    }
  }
  if (count > 0) console.log('[schema] Applied ' + count + ' field overrides');
}

function loadMsgSchema() {
  var host = location.host || 'localhost:6969';
  Promise.all([
    fetch('http://' + host + '/api/msgdump').then(function (r) { return r.json(); }),
    fetch('http://' + host + '/overrides.json').then(function (r) { return r.json(); }).catch(function () { return {}; }),
    fetch('http://' + host + '/api/protomap?all=1').then(function (r) { return r.json(); }).catch(function () { return null; })
  ]).then(function (results) {
    var data = results[0];
    msgDefs = data.defs || {};
    msgSchemas = data.schemas || {};
    msgEnums = data.enums || {};
    msgOverrides = results[1] || {};
    var protomap = results[2];
    if (protomap && protomap.results) {
      _lastProtoMap = protomap.results;
      applyProtoMap(protomap.results);
    }
    applyOverrides();
    schemaLoaded = true;
    console.log('[schema] Loaded ' + Object.keys(msgDefs).length + ' defs, '
      + Object.keys(msgSchemas).length + ' schemas, '
      + Object.keys(msgEnums).length + ' enums');
    renderPacketList();
    populateCraftDropdown();
  }).catch(function (e) {
    console.warn('[schema] Failed to load:', e);
    setTimeout(loadMsgSchema, 5000);
  });
}

function applyProtoMap(pmResults) {
  var patched = 0, inferred = 0;
  for (var className in pmResults) {
    var schema = msgSchemas[className];
    if (!schema || !schema.fields || schema.fields.length === 0) continue;
    var pm = pmResults[className];
    for (var i = 0; i < schema.fields.length; i++) {
      var f = schema.fields[i];
      if (pm[f.name] !== undefined && pm[f.name] > 0) { f.n = pm[f.name]; patched++; }
    }
    var anchors = [];
    for (var i = 0; i < schema.fields.length; i++) {
      if (pm[schema.fields[i].name] !== undefined && pm[schema.fields[i].name] > 0)
        anchors.push({ idx: i, num: pm[schema.fields[i].name] });
    }
    var prevIdx = -1, prevNum = 0;
    for (var a = 0; a <= anchors.length; a++) {
      var nextIdx = a < anchors.length ? anchors[a].idx : schema.fields.length;
      var nextNum = a < anchors.length ? anchors[a].num : null;
      var unknowns = [];
      for (var i = prevIdx + 1; i < nextIdx; i++) {
        if (!pm[schema.fields[i].name] || pm[schema.fields[i].name] <= 0) unknowns.push(i);
      }
      if (unknowns.length > 0 && nextNum !== null) {
        var startNum = prevNum + 1, endNum = nextNum - 1, slots = endNum - startNum + 1;
        if (slots === unknowns.length) {
          for (var u = 0; u < unknowns.length; u++) { schema.fields[unknowns[u]].n = startNum + u; inferred++; }
        } else if (slots > unknowns.length) {
          for (var u = 0; u < unknowns.length; u++) { schema.fields[unknowns[u]].n = endNum - unknowns.length + 1 + u; inferred++; }
        }
      } else if (unknowns.length > 0 && nextNum === null && prevNum > 0) {
        for (var u = 0; u < unknowns.length; u++) { schema.fields[unknowns[u]].n = prevNum + 1 + u; inferred++; }
      }
      if (a < anchors.length) { prevIdx = anchors[a].idx; prevNum = anchors[a].num; }
    }
  }
  console.log('[protomap] Patched ' + patched + ' field numbers, inferred ' + inferred + ' from gaps');
}

function getMsgName(msgId) { return msgDefs[String(msgId)] || ''; }
function getMsgSchema(msgId) {
  var name = getMsgName(msgId);
  return (name && msgSchemas[name]) ? msgSchemas[name] : null;
}

// ─── WebSocket ──────────────────────────────────────────────────────────────
function connect() {
  var host = location.host || 'localhost:6969';
  ws = new WebSocket('ws://' + host + '/ws');
  ws.onopen = function () {
    reconnectDelay = 1000;
    setConnected(true);
    // Clear decode cache on reconnect (DLL may have been re-injected with new code)
    decodeCache = {};
    _decodeCacheKeys = [];
    if (!schemaLoaded) loadMsgSchema();
  };
  ws.onclose = function () {
    setConnected(false);
    setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 1.5, 10000);
  };
  ws.onerror = function () { ws.close(); };
  ws.onmessage = function (evt) {
    try {
      var msg = JSON.parse(evt.data);
      if (msg.type === 'packet') onPacket(msg);
    } catch (e) {}
  };
}

function setConnected(on) {
  var dot = document.getElementById('connDot');
  var txt = document.getElementById('connText');
  if (on) { dot.classList.add('connected'); txt.textContent = 'Connected'; }
  else { dot.classList.remove('connected'); txt.textContent = 'Disconnected'; }
}

// ─── Packet handling (incremental filter) ────────────────────────────────────
var _renderScheduled = false;

function passesFilter(p) {
  if (dirFilter !== 'all' && p.dir !== dirFilter) return false;
  if (msgIdFilter.length > 0 && msgIdFilter.indexOf(p.msgId) === -1) return false;
  return true;
}

function onPacket(pkt) {
  packets.push(pkt);
  if (packets.length > maxPackets) {
    // Trim from front — must rebuild filtered since indices shift
    packets = packets.slice(-maxPackets);
    applyFilter();
  } else {
    // Incremental: just check new packet
    if (passesFilter(pkt)) filteredPackets.push(pkt);
  }
  if (!_renderScheduled) {
    _renderScheduled = true;
    requestAnimationFrame(flushRender);
  }
}

function flushRender() {
  _renderScheduled = false;
  var cb = document.getElementById('autoScroll');
  var shouldScroll = cb && cb.checked && _poolEl && !_userScrollIntent;
  renderPacketList();
  if (shouldScroll) _poolEl.scrollTop = _poolEl.scrollHeight;
  updateCount();
}

function clearPackets() {
  packets = [];
  filteredPackets = [];
  selectedIdx = -1;
  renderPacketList();
  showEmpty();
  updateCount();
}

function updateCount() {
  var el = document.getElementById('pktCount');
  if (el) el.textContent = filteredPackets.length + ' / ' + packets.length + ' packets';
}

// ─── Filtering ──────────────────────────────────────────────────────────────
function setDirFilter(el) {
  var btns = el.parentNode.children;
  for (var i = 0; i < btns.length; i++) btns[i].classList.remove('on');
  el.classList.add('on');
  dirFilter = el.dataset.d;
  applyFilter();
  _invalidatePool();
  if (_poolEl) _poolEl.scrollTop = 0;
  selectedIdx = -1;
  showEmpty();
  renderPacketList();
  updateCount();
}

function applyFilter() {
  filteredPackets = [];
  for (var i = 0; i < packets.length; i++) {
    if (passesFilter(packets[i])) filteredPackets.push(packets[i]);
  }
}

(function () {
  var inp = document.getElementById('filterMsgId');
  if (!inp) return;
  var tid = 0;
  inp.addEventListener('input', function () {
    clearTimeout(tid);
    tid = setTimeout(function () {
      var val = inp.value.trim();
      if (val === '') { msgIdFilter = []; }
      else { msgIdFilter = val.split(',').map(function (s) { return parseInt(s.trim(), 10); }).filter(function (n) { return !isNaN(n); }); }
      applyFilter();
      _invalidatePool();
      if (_poolEl) _poolEl.scrollTop = 0;
      renderPacketList();
      updateCount();
    }, 120);
  });
})();

// ─── Packet list: DOM pool (no innerHTML, reuses elements) ──────────────────
var ROW_HEIGHT = 24;
var _pool = [], _poolEl = null, _topSp = null, _botSp = null;

(function initPool() {
  _poolEl = document.getElementById('pktList');
  if (!_poolEl) return;
  _topSp = document.createElement('div');
  _botSp = document.createElement('div');
  _poolEl.appendChild(_topSp);
  _poolEl.appendChild(_botSp);
  var raf = false;
  _poolEl.addEventListener('scroll', function () {
    if (raf) return;
    raf = true;
    requestAnimationFrame(function () { raf = false; renderPacketList(); });
  });
  // Auto-disable auto-scroll on user wheel, re-enable at bottom
  _poolEl.addEventListener('wheel', function () {
    _userScrollIntent = true;
    clearTimeout(_userScrollTimer);
    _userScrollTimer = setTimeout(function () { _userScrollIntent = false; }, 300);
  }, { passive: true });
  _poolEl.addEventListener('scroll', function () {
    if (!_userScrollIntent) return;
    var cb = document.getElementById('autoScroll');
    if (!cb) return;
    var atBottom = _poolEl.scrollHeight - _poolEl.scrollTop - _poolEl.clientHeight < ROW_HEIGHT * 2;
    cb.checked = atBottom;
  });
})();
var _userScrollIntent = false, _userScrollTimer = 0;

function _growPool(n) {
  while (_pool.length < n) {
    var r = document.createElement('div');
    r.className = 'pkt-row';
    var c0 = document.createElement('span'); c0.className = 'pr-seq';
    var c1 = document.createElement('span'); c1.className = 'pr-dir';
    var c2 = document.createElement('span'); c2.className = 'pr-mid';
    var c3 = document.createElement('span'); c3.className = 'pr-name';
    var c4 = document.createElement('span'); c4.className = 'pr-len';
    r.appendChild(c0); r.appendChild(c1); r.appendChild(c2); r.appendChild(c3); r.appendChild(c4);
    r._c = [c0, c1, c2, c3, c4];
    r._di = -1; r._sel = false;
    (function (row) {
      var downIdx = -1;
      row.addEventListener('mousedown', function () { downIdx = row._di; });
      row.addEventListener('click', function () { if (downIdx >= 0) selectPacket(downIdx); downIdx = -1; });
    })(r);
    _poolEl.insertBefore(r, _botSp);
    _pool.push(r);
  }
}

function _invalidatePool() {
  for (var i = 0; i < _pool.length; i++) { _pool[i]._di = -1; _pool[i]._sel = false; }
}

function renderPacketList() {
  if (!_poolEl) return;
  var total = filteredPackets.length;
  var viewH = _poolEl.clientHeight || 400;
  var scrollTop = _poolEl.scrollTop;
  // Clamp start so it never exceeds total
  var rawStart = Math.floor(scrollTop / ROW_HEIGHT) - 2;
  var start = Math.max(0, Math.min(rawStart, total > 0 ? total - 1 : 0));
  var end = Math.min(total, start + Math.ceil(viewH / ROW_HEIGHT) + 4);
  var count = end - start;
  if (count < 0) count = 0;

  _growPool(count);
  _topSp.style.height = (start * ROW_HEIGHT) + 'px';
  _botSp.style.height = (Math.max(0, (total - end) * ROW_HEIGHT)) + 'px';

  for (var i = 0; i < count; i++) {
    var row = _pool[i];
    var di = start + i;
    var sel = (di === selectedIdx);
    row.style.display = '';
    // Skip update if row data unchanged
    if (row._di === di && row._sel === sel) continue;
    var p = filteredPackets[di];
    row._di = di; row._sel = sel;
    row.className = sel ? 'pkt-row selected' : 'pkt-row';
    row._c[0].textContent = p.seq;
    var send = p.dir === 'send';
    row._c[1].textContent = send ? '\u2192' : '\u2190';
    row._c[1].className = send ? 'pr-dir dir-send' : 'pr-dir dir-recv';
    row._c[2].textContent = p.msgId;
    row._c[3].textContent = getMsgName(p.msgId);
    row._c[4].textContent = p.len;
  }
  // Hide excess pool rows
  for (var i = count; i < _pool.length; i++) { _pool[i].style.display = 'none'; _pool[i]._di = -1; }
}

function selectPacket(idx) {
  var prev = selectedIdx;
  selectedIdx = idx;
  // Invalidate _di on changed rows so the skip condition fails and they re-render
  for (var i = 0; i < _pool.length; i++) {
    var r = _pool[i];
    if (r._di === prev || r._di === idx) { r._di = -1; }
  }
  renderPacketList();
  showDetail(filteredPackets[idx]);
}

// ─── Detail view ────────────────────────────────────────────────────────────
function showEmpty() {
  var e = document.getElementById('detailEmpty');
  var c = document.getElementById('detailContent');
  if (e) e.style.display = '';
  if (c) c.style.display = 'none';
  activeSchema = null;
}

function showDetail(pkt) {
  var e = document.getElementById('detailEmpty');
  var c = document.getElementById('detailContent');
  if (e) e.style.display = 'none';
  if (c) c.style.display = '';

  var name = getMsgName(pkt.msgId);
  var schema = getMsgSchema(pkt.msgId);

  // Header
  var h = document.getElementById('detailHeader');
  var dirLabel = pkt.dir === 'send' ? 'SEND \u2192' : 'RECV \u2190';
  var dirCls = pkt.dir === 'send' ? 'dir-send' : 'dir-recv';
  h.innerHTML = '<span class="' + dirCls + '" style="font-weight:700">' + dirLabel + '</span>'
    + ' <span style="color:var(--ac)">msgId: ' + pkt.msgId + '</span>'
    + (name ? ' <span style="color:var(--ac4);font-weight:600">' + escHtml(name) + '</span>' : '')
    + ' <span style="color:var(--dim)">len: ' + pkt.len + '</span>'
    + ' <span style="color:var(--dim)">seq: #' + pkt.seq + '</span>';

  // Hex dump (lazy: first 512 bytes, expand on click)
  var hexDiv = document.getElementById('detailHex');
  if (hexDiv) renderHexDump(hexToBytes(pkt.data), hexDiv);

  // Clear schema BEFORE doParse
  activeSchema = null;

  // Protocol editor parse
  try {
    if (typeof M !== 'undefined') M = (pkt.dir === 'send') ? 'frame' : 'proto';
    var inp = document.getElementById('inp');
    if (inp && typeof doParse === 'function') {
      inp.value = pkt.data.replace(/(..)/g, '$1 ').trim();
      doParse();
    }
  } catch (ex) {
    var res = document.getElementById('results');
    if (res) res.innerHTML = '';
  }

  activeSchema = schema;
  activeSchemaName = schema ? (getMsgName(pkt.msgId) || '') : '';
  if (schema) applySchemaOverlay();

  var btnResend = document.getElementById('btnResend');
  var btnEditSend = document.getElementById('btnEditSend');
  if (btnResend) btnResend.textContent = pkt.dir === 'send' ? 'Resend' : 'Replay Recv';
  if (btnEditSend) btnEditSend.textContent = pkt.dir === 'send' ? 'Edit & Send' : 'Edit & Replay';

  // Show Analyze Cells button only for GuildExplore packets
  var btnCells = document.getElementById('btnAnalyzeCells');
  if (btnCells) btnCells.style.display = (name && name.indexOf('GuildExplore') !== -1) ? '' : 'none';

  fetchDecode(pkt);
}

// ─── Schema field name overlay ──────────────────────────────────────────────
var activeSchema = null, activeSchemaName = '', activeSchemaLvl = 'gen';

(function () {
  var orig = (typeof renderEd === 'function') ? renderEd : null;
  if (!orig) return;
  renderEd = function () { orig(); if (activeSchema) applySchemaOverlay(); };
})();

function applySchemaOverlay() {
  var results = document.getElementById('results');
  if (!results || !activeSchema || !activeSchema.fields || activeSchema.fields.length === 0) return;
  var tables = results.querySelectorAll('table.bt');
  if (tables.length === 0) return;
  annotateTable(tables[0], activeSchema.fields, true);
}

function getEnumName(enumTypeName, val) {
  var e = msgEnums[enumTypeName];
  if (!e) return null;
  for (var name in e) { if (e[name] === val) return name; }
  return null;
}

function annotateTable(table, schemaFields, isTopLevel) {
  if (!schemaFields || schemaFields.length === 0) return;
  var fieldMap = {};
  for (var i = 0; i < schemaFields.length; i++) fieldMap[schemaFields[i].n] = schemaFields[i];

  var tbody = table.querySelector('tbody');
  if (!tbody) return;
  var presentFields = {};
  var rows = tbody.children;

  for (var r = 0; r < rows.length; r++) {
    var row = rows[r];
    var fnInput = row.querySelector(':scope > td:first-child input[type=number]');
    if (!fnInput) continue;
    var fn = parseInt(fnInput.value, 10);
    var sf = fieldMap[fn];
    if (!sf) continue;
    presentFields[fn] = true;

    var label = document.createElement('span');
    label.className = 'schema-label';
    label.textContent = sf.name;
    fnInput.parentNode.appendChild(label);

    var cells = row.querySelectorAll(':scope > td');
    if (cells.length > 1) {
      var typeLabel = document.createElement('span');
      typeLabel.className = 'schema-type-label';
      var typeText = sf.t === 'enum' && sf.sub ? sf.sub : sf.t;
      if (sf.snappy) typeText += '+snappy';
      typeLabel.textContent = ' (' + typeText + (sf.r ? '[]' : '') + ')';
      cells[1].appendChild(typeLabel);
    }

    var subName = sf.sub;
    if (!subName && sf.t === 'bytes') {
      var cap = sf.name.charAt(0).toUpperCase() + sf.name.slice(1);
      var base = activeSchemaName.replace(/^CS|^SC/, '').replace(/Msg$/, '');
      var prefixes = [base];
      var parts = base.replace(/([A-Z])/g, ' $1').trim().split(' ');
      for (var pi = parts.length - 1; pi >= 2; pi--) prefixes.push(parts.slice(0, pi).join(''));
      prefixes.push('');
      var found = false;
      for (var pi = 0; pi < prefixes.length && !found; pi++) {
        var pf = prefixes[pi];
        var tries = pf ? [pf + cap + 'Msg', pf + cap] : [cap + 'Msg', cap];
        for (var ci = 0; ci < tries.length; ci++) {
          if (msgSchemas[tries[ci]]) { subName = tries[ci]; found = true; break; }
        }
      }
    }
    if (subName) {
      var nextRow = rows[r + 1];
      if (nextRow) {
        var nestedTable = nextRow.querySelector('table.bt');
        if (nestedTable) {
          if (sf.t === 'enum' && msgEnums[subName]) annotateEnumTable(nestedTable, subName);
          else if (msgSchemas[subName]) annotateTable(nestedTable, msgSchemas[subName].fields, false);
        }
      }
    }
  }

  if (!isTopLevel) return;
  var missingFields = [];
  for (var i = 0; i < schemaFields.length; i++) {
    if (!presentFields[schemaFields[i].n]) missingFields.push(schemaFields[i]);
  }
  if (missingFields.length === 0) return;

  var oldMissing = table.parentNode.querySelector('.missing-fields');
  if (oldMissing) oldMissing.remove();

  var div = document.createElement('div');
  div.className = 'missing-fields';
  var header = document.createElement('div');
  header.className = 'missing-fields-header';
  header.textContent = 'Optional fields not in packet (' + missingFields.length + ')';
  div.appendChild(header);

  for (var i = 0; i < missingFields.length; i++) {
    var mf = missingFields[i];
    var mfRow = document.createElement('div');
    mfRow.className = 'missing-field-row';
    mfRow.innerHTML = '<span class="mf-num">' + mf.n + '</span>'
      + '<span class="mf-name">' + escHtml(mf.name) + '</span>'
      + '<span class="mf-type">' + escHtml(mf.t + (mf.r ? '[]' : '')) + '</span>'
      + '<button class="mf-add" data-fn="' + mf.n + '" data-t="' + escHtml(mf.t) + '" data-r="' + (mf.r ? '1' : '0') + '" data-sub="' + escHtml(mf.sub || '') + '">+ Add</button>';
    div.appendChild(mfRow);
  }

  var addRow = table.parentNode.querySelector('.add-row');
  if (addRow) addRow.parentNode.insertBefore(div, addRow);
  else table.parentNode.appendChild(div);

  div.querySelectorAll('.mf-add').forEach(function (btn) {
    btn.addEventListener('click', function () {
      addSchemaField(activeSchemaLvl, [], parseInt(this.dataset.fn, 10), this.dataset.t, false, '');
    });
  });
}

function annotateEnumTable(table, enumTypeName) {
  var tbody = table.querySelector('tbody');
  if (!tbody) return;
  var enumDef = msgEnums[enumTypeName];
  if (!enumDef) return;
  var rows = tbody.children;
  for (var r = 0; r < rows.length; r++) {
    var row = rows[r];
    var fnInput = row.querySelector(':scope > td:first-child input[type=number]');
    if (!fnInput) continue;
    var label = document.createElement('span');
    label.className = 'schema-label'; label.textContent = 'value';
    fnInput.parentNode.appendChild(label);
    var cells = row.querySelectorAll(':scope > td');
    if (cells.length > 2) {
      var valInput = cells[2].querySelector('input');
      if (valInput) {
        var constName = getEnumName(enumTypeName, parseInt(valInput.value, 10));
        if (constName) {
          var el = document.createElement('span');
          el.className = 'schema-enum-label'; el.textContent = constName;
          valInput.parentNode.appendChild(el);
        }
      }
      var tl = document.createElement('span');
      tl.className = 'schema-type-label'; tl.textContent = ' (' + enumTypeName + ')';
      cells[1].appendChild(tl);
    }
  }
}

function schemaTypeToWire(t) {
  if (t === 'int32' || t === 'uint32' || t === 'int64' || t === 'uint64' ||
      t === 'bool' || t === 'enum' || t === 'sint32' || t === 'sint64') return 'vi';
  if (t === 'float' || t === 'fixed32' || t === 'sfixed32') return 'f32';
  if (t === 'double' || t === 'fixed64' || t === 'sfixed64') return 'f64';
  return 'ld';
}

function addSchemaField(lvl, parentPath, fn, schemaType, isRepeated, sub) {
  if (!S) return;
  var fa = parentPath.length === 0 ? getFA(lvl) : resolveFAParent(lvl, parentPath);
  var wt = schemaTypeToWire(schemaType);
  var gf = { fn: fn, t: wt };
  if (wt === 'vi') { gf.wt = 0; gf.v = 0; gf.lo = 0; gf.hi = 0; gf.n1 = false; }
  else if (wt === 'ld') { gf.wt = 2; gf.rb = new Uint8Array(0); }
  else if (wt === 'f32') { gf.wt = 5; gf.b = new Uint8Array(4); gf.v = 0; }
  else if (wt === 'f64') { gf.wt = 1; gf.b = new Uint8Array(8); }
  fa.push(gf);
  fa.sort(function (a, b) { return a.fn - b.fn; });
  renderEd();
}

// ─── Decoded fields: lazy DOM tree ──────────────────────────────────────────
// No innerHTML for decoded tree. Collapsed nodes don't render children until
// expanded. Large arrays load in batches of 50.

var _decodeCacheKeys = [], _decodeCacheMax = 50;
var decodeCache = {};

function _dcKey(pkt) { return pkt.msgId + ':' + pkt.data.length + ':' + pkt.data; }
function _dcSet(key, val) {
  if (decodeCache[key]) return;
  decodeCache[key] = val;
  _decodeCacheKeys.push(key);
  while (_decodeCacheKeys.length > _decodeCacheMax) delete decodeCache[_decodeCacheKeys.shift()];
}

function toggleDecoded() {
  var body = document.getElementById('decodedBody');
  var toggle = document.getElementById('decodedToggle');
  if (body.style.display === 'none') { body.style.display = ''; toggle.innerHTML = '&#9660;'; }
  else { body.style.display = 'none'; toggle.innerHTML = '&#9654;'; }
}

function fetchDecode(pkt) {
  var panel = document.getElementById('detailDecoded');
  var body = document.getElementById('decodedBody');
  if (!panel || !body) return;
  var name = getMsgName(pkt.msgId);
  if (!name) { panel.style.display = 'none'; return; }
  panel.style.display = '';

  var key = _dcKey(pkt);
  if (decodeCache[key]) { renderDecoded(body, decodeCache[key]); return; }

  body.innerHTML = '';
  var ld = document.createElement('div'); ld.className = 'df-loading'; ld.textContent = 'Decoding...';
  body.appendChild(ld);
  body.style.display = '';
  document.getElementById('decodedToggle').innerHTML = '&#9660;';

  var host = location.host || 'localhost:6969';
  var hexData = pkt.data;
  if (pkt.dir === 'send' && hexData.length > 40) hexData = hexData.substring(40);

  fetch('http://' + host + '/api/decode?msgId=' + pkt.msgId + '&data=' + hexData)
    .then(function (r) { return r.json(); })
    .then(function (data) {
      if (data.error) { body.innerHTML = ''; var e = document.createElement('div'); e.className = 'df-error'; e.textContent = data.error; body.appendChild(e); return; }
      _dcSet(key, data);
      renderDecoded(body, data);
    })
    .catch(function (e) { body.innerHTML = ''; var el = document.createElement('div'); el.className = 'df-error'; el.textContent = 'Decode failed: ' + e; body.appendChild(el); });
}

function renderDecoded(container, data) {
  container.innerHTML = '';
  if (!data || !data.fields) {
    var e = document.createElement('div'); e.className = 'df-error'; e.textContent = 'No fields';
    container.appendChild(e); return;
  }
  _buildFields(container, data.fields, 0);
}

function _buildFields(parent, fields, depth) {
  if (depth > 10) { parent.appendChild(document.createTextNode('[too deep]')); return; }
  var keys = Object.keys(fields);
  var hasFlags = {}, valueKeys = [];
  for (var i = 0; i < keys.length; i++) {
    var k = keys[i];
    if (k.length > 3 && k.substring(0, 3) === 'has' && k.charAt(3) >= 'A' && k.charAt(3) <= 'Z')
      hasFlags[k.substring(3, 4).toLowerCase() + k.substring(4)] = fields[k];
    else valueKeys.push(k);
  }
  var frag = document.createDocumentFragment();
  for (var i = 0; i < valueKeys.length; i++) {
    _buildRow(frag, valueKeys[i], fields[valueKeys[i]], hasFlags[valueKeys[i]], depth);
  }
  parent.appendChild(frag);
}

function _buildRow(parent, key, value, hasFlag, depth) {
  var isObj = value !== null && typeof value === 'object';
  var isArr = Array.isArray(value);
  var isCollapsible = isObj && (isArr ? value.length > 0 : (!value._list && Object.keys(value).length > 0));
  var autoCollapse = isCollapsible && (depth >= 1 || (isArr && value.length > 5) || key.charAt(0) === '_');

  var row = document.createElement('div');
  row.className = isCollapsible ? 'df-row df-collapsible' : 'df-row';

  if (isCollapsible) {
    var tog = document.createElement('span');
    tog.className = 'df-toggle' + (autoCollapse ? '' : ' open');
    tog.textContent = '\u25b6';
    row.appendChild(tog);

    var nm = document.createElement('span');
    nm.className = 'df-name';
    nm.textContent = key;
    row.appendChild(nm);

    // Inline summary showing count/preview
    var sum = document.createElement('span'); sum.className = 'df-count';
    if (isArr) {
      sum.textContent = '[' + value.length + ' items]';
    } else {
      var objKeys = Object.keys(value).filter(function(k) {
        return !(k.length > 3 && k.substring(0, 3) === 'has' && k.charAt(3) >= 'A' && k.charAt(3) <= 'Z');
      });
      sum.textContent = '{' + objKeys.length + ' fields}';
    }
    row.appendChild(sum);

    var sub = document.createElement('div');
    sub.className = 'df-sub';
    sub.style.display = autoCollapse ? 'none' : '';
    sub._ld = value; sub._dd = depth; sub._ok = false;
    row.appendChild(sub);

    if (!autoCollapse) _renderSub(sub);

    (function (t, s) {
      function toggle() {
        var open = t.classList.toggle('open');
        s.style.display = open ? '' : 'none';
        if (open && !s._ok) _renderSub(s);
      }
      t.addEventListener('click', toggle);
      nm.addEventListener('click', toggle);
    })(tog, sub);
  } else {
    var has = document.createElement('span'); has.className = 'df-has';
    if (hasFlag === true) has.textContent = '\u2713';
    row.appendChild(has);
    var nm = document.createElement('span'); nm.className = 'df-name'; nm.textContent = key;
    row.appendChild(nm);
    _appendVal(row, value);
  }
  parent.appendChild(row);
}

var _BATCH = 50;

function _renderSub(sub) {
  sub._ok = true;
  var data = sub._ld, depth = sub._dd;

  if (Array.isArray(data)) {
    var hdr = document.createElement('div'); hdr.className = 'df-list-header';
    hdr.textContent = '[' + data.length + ' items]'; sub.appendChild(hdr);

    var loaded = 0;
    var moreBtn = null;

    function loadBatch() {
      var end = Math.min(data.length, loaded + _BATCH);
      var frag = document.createDocumentFragment();
      for (var i = loaded; i < end; i++) {
        _buildRow(frag, '[' + i + ']', data[i], undefined, depth + 1);
      }
      if (moreBtn) sub.insertBefore(frag, moreBtn);
      else sub.appendChild(frag);
      loaded = end;

      if (loaded < data.length) {
        if (!moreBtn) {
          moreBtn = document.createElement('div');
          moreBtn.className = 'df-row df-more';
          var sp = document.createElement('span'); sp.className = 'df-val null';
          moreBtn.appendChild(sp);
          moreBtn.style.cursor = 'pointer';
          moreBtn.addEventListener('click', loadBatch);
          sub.appendChild(moreBtn);
        }
        moreBtn.firstChild.textContent = 'Load more (' + (data.length - loaded) + ' remaining)';
      } else if (moreBtn) {
        moreBtn.remove(); moreBtn = null;
      }
    }
    loadBatch();
  } else if (typeof data === 'object' && data !== null) {
    if (data._list) { sub.textContent = '[list: ' + (data.count || 0) + ' items]'; }
    else _buildFields(sub, data, depth + 1);
  }
}

function _appendVal(parent, v) {
  var span = document.createElement('span');
  if (v === null || v === undefined) { span.className = 'df-val null'; span.textContent = 'null'; }
  else if (typeof v === 'boolean') { span.className = 'df-val num'; span.textContent = String(v); }
  else if (typeof v === 'number') { span.className = 'df-val num'; span.textContent = String(v); }
  else if (typeof v === 'string') {
    if (v.charAt(0) === '<' && v.charAt(v.length - 1) === '>') {
      span.className = 'df-val null'; span.textContent = v;
    } else { span.className = 'df-val str'; span.textContent = '"' + v + '"'; }
  } else { span.className = 'df-val'; span.textContent = String(v); }
  parent.appendChild(span);
}

// ─── Export Diagnostic ───────────────────────────────────────────────────────
function exportDiagnostic() {
  if (selectedIdx < 0 || !filteredPackets[selectedIdx]) { alert('Select a packet first'); return; }
  var pkt = filteredPackets[selectedIdx];
  var name = getMsgName(pkt.msgId);
  var schema = getMsgSchema(pkt.msgId);

  var diag = {
    _info: 'Packet diagnostic — paste this to Claude for override generation',
    msgId: pkt.msgId, msgName: name || '(unknown)', dir: pkt.dir, len: pkt.len, dataHex: pkt.data
  };

  if (schema && schema.fields) {
    diag.schema = schema.fields.map(function (f) {
      var entry = { n: f.n, name: f.name, type: f.t };
      if (f.r) entry.repeated = true;
      if (f.sub) entry.sub = f.sub;
      if (f.snappy) entry.snappy = true;
      return entry;
    });
  }

  var protoHex = pkt.data;
  if (pkt.dir === 'send' && protoHex.length > 40) {
    diag.transportHeader = protoHex.substring(0, 40);
    protoHex = protoHex.substring(40);
  }
  diag.protoHex = protoHex;

  if (typeof S !== 'undefined' && S && S.genFields) {
    diag.parsedFields = serializeFields(S.genFields);
    if (S.frame) diag.frame = S.frame;
  }

  if (schema && schema.fields && typeof S !== 'undefined' && S && S.genFields) {
    var fieldMap = {};
    for (var fi = 0; fi < schema.fields.length; fi++) fieldMap[schema.fields[fi].n] = schema.fields[fi];
    diag.overlay = S.genFields.map(function (gf) {
      var sf = fieldMap[gf.fn];
      var row = { wireFieldNum: gf.fn, wireType: gf.t || gf.wt };
      if (gf.t === 'vi') row.value = gf.hi ? (gf.hi * 4294967296 + gf.lo) : (gf.v !== undefined ? gf.v : gf.lo);
      else if (gf.t === 'f32') row.value = gf.v;
      else if (gf.t === 'ld' && gf.rb) { row.hexBytes = bytesToHex(gf.rb); row.length = gf.rb.length; }
      if (gf.t === 'ld' && gf.sub && gf.sub.length > 0) row.subFields = serializeFields(gf.sub);
      if (sf) {
        row.schemaName = sf.name; row.schemaType = sf.t;
        if (sf.r) row.schemaRepeated = true;
        if (sf.sub) row.schemaSub = sf.sub;
        if (sf.snappy) row.schemaSnappy = true;
        var expectedWire = schemaTypeToWire(sf.t);
        if (expectedWire !== (gf.t || '')) row.wireTypeMismatch = expectedWire;
      } else { row.schemaMatch = 'NONE — no schema field matches field number ' + gf.fn; }
      return row;
    });
    var wireNums = {};
    S.genFields.forEach(function (gf) { wireNums[gf.fn] = true; });
    var unmatchedSchema = schema.fields.filter(function (sf) { return !wireNums[sf.n]; });
    if (unmatchedSchema.length > 0) {
      diag.unmatchedSchemaFields = unmatchedSchema.map(function (sf) {
        return { n: sf.n, name: sf.name, type: sf.t, repeated: sf.r || false };
      });
    }
  }

  var key = _dcKey(pkt);
  if (decodeCache[key]) diag.decoded = decodeCache[key];
  if (_lastProtoMap && name && _lastProtoMap[name]) diag.protomap = _lastProtoMap[name];
  if (name && msgOverrides[name]) diag.currentOverrides = msgOverrides[name];

  var json = JSON.stringify(diag, null, 2);
  if (navigator.clipboard) {
    navigator.clipboard.writeText(json).then(function () { showToast('Diagnostic copied to clipboard'); }).catch(function () { showDiagPopup(json); });
  } else { showDiagPopup(json); }
}

function serializeFields(fields) {
  if (!fields) return [];
  return fields.map(function (gf) {
    var entry = { fieldNum: gf.fn, wireType: gf.t || gf.wt };
    if (gf.t === 'vi') {
      entry.value = gf.hi ? (gf.hi * 4294967296 + gf.lo) : (gf.v !== undefined ? gf.v : gf.lo);
      entry.negative = gf.n1 || false;
    } else if (gf.t === 'f32') { entry.value = gf.v; }
    else if (gf.t === 'f64') { entry.value = gf.b ? Array.from(gf.b) : null; }
    else if (gf.t === 'ld') {
      if (gf.sub && gf.sub.length > 0) { entry.subFields = serializeFields(gf.sub); }
      else if (gf.rb) {
        var bytes = gf.rb, isText = true;
        for (var i = 0; i < Math.min(bytes.length, 32); i++) {
          if (bytes[i] < 0x20 && bytes[i] !== 0x0a && bytes[i] !== 0x0d && bytes[i] !== 0x09) { isText = false; break; }
        }
        if (isText && bytes.length > 0 && bytes.length < 512) {
          try { entry.asString = new TextDecoder().decode(bytes); } catch (e) {}
        }
        entry.hexBytes = bytesToHex(bytes); entry.length = bytes.length;
      }
    }
    return entry;
  });
}

function showToast(msg) {
  var t = document.createElement('div'); t.className = 'diag-toast'; t.textContent = msg;
  document.body.appendChild(t);
  setTimeout(function () { t.classList.add('show'); }, 10);
  setTimeout(function () { t.classList.remove('show'); setTimeout(function () { t.remove(); }, 300); }, 2000);
}

function showDiagPopup(json) {
  var overlay = document.createElement('div'); overlay.className = 'diag-overlay';
  overlay.innerHTML = '<div class="diag-popup">'
    + '<div class="diag-popup-header">Diagnostic Export <button class="bc" onclick="this.closest(\'.diag-overlay\').remove()">Close</button></div>'
    + '<textarea class="diag-textarea" readonly>' + escHtml(json) + '</textarea></div>';
  document.body.appendChild(overlay);
  var ta = overlay.querySelector('textarea'); ta.focus(); ta.select();
}

// ─── Hex utilities ──────────────────────────────────────────────────────────
function hexToBytes(hex) {
  var len = hex.length >> 1;
  var bytes = new Uint8Array(len);
  for (var i = 0; i < len; i++) bytes[i] = parseInt(hex.substr(i << 1, 2), 16);
  return bytes;
}

function bytesToHex(bytes) {
  var out = '';
  for (var i = 0; i < bytes.length; i++) out += _HL[bytes[i]];
  return out;
}

function _hexLine(bytes, off) {
  var hex = '', ascii = '';
  for (var i = 0; i < 16; i++) {
    if (off + i < bytes.length) {
      var b = bytes[off + i];
      hex += _HU[b] + ' ';
      // Escape <, >, & for safe innerHTML
      if (b === 60) ascii += '&lt;';
      else if (b === 62) ascii += '&gt;';
      else if (b === 38) ascii += '&amp;';
      else ascii += (b > 31 && b < 127) ? String.fromCharCode(b) : '.';
    } else { hex += '   '; ascii += ' '; }
    if (i === 7) hex += ' ';
  }
  return '<span class=ho>' + ('00000' + off.toString(16)).slice(-6) + '</span>  ' + hex + ' <span class=ha>' + ascii + '</span>';
}

function renderHexDump(bytes, container) {
  container.innerHTML = '';
  var maxInit = 512; // show first 512 bytes initially
  var hx = document.createElement('div'); hx.className = 'hx';
  var lines = [];
  var limit = Math.min(bytes.length, maxInit);
  for (var off = 0; off < limit; off += 16) lines.push(_hexLine(bytes, off));
  hx.innerHTML = lines.join('\n');
  container.appendChild(hx);

  if (bytes.length > maxInit) {
    var btn = document.createElement('button'); btn.className = 'bc';
    btn.textContent = 'Show all (' + bytes.length + ' bytes)';
    btn.style.marginTop = '4px';
    btn.addEventListener('click', function () {
      var all = [];
      for (var off = 0; off < bytes.length; off += 16) all.push(_hexLine(bytes, off));
      hx.innerHTML = all.join('\n');
      btn.remove();
    });
    container.appendChild(btn);
  }
}

function escHtml(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// ─── Packet sending ─────────────────────────────────────────────────────────
function randomToken() { return (Math.random() * 0xFFFFFFFF) >>> 0; }

function u8ToHex(arr) {
  var out = '';
  for (var i = 0; i < arr.length; i++) out += _HL[arr[i]];
  return out;
}

function getEditorProtoBytes() {
  if (!S || typeof buildGenericProto !== 'function') return null;
  try { return S.hasBop ? buildFull() : buildGenericProto(); } catch (e) { return null; }
}

function buildSendFrame(msgId) {
  var proto = getEditorProtoBytes();
  if (!proto) return null;
  var fl = 16 + proto.length;
  var out = new Uint8Array(4 + fl);
  w32b(out, 0, fl);
  w32b(out, 4, randomToken());
  w32b(out, 8, S.frame ? S.frame.route : 0x000002A8);
  w32b(out, 12, 0);
  w32b(out, 16, msgId);
  out.set(proto, 20);
  return out;
}

function sendRawToServer(type, msgId, hexData) {
  if (!ws || ws.readyState !== 1) { alert('Not connected'); return; }
  ws.send(JSON.stringify({ type: type, msgId: msgId, data: hexData.replace(/\s+/g, '').toLowerCase() }));
}

var craftType = 'send';

function setCraftType(el) {
  var btns = el.parentNode.children;
  for (var i = 0; i < btns.length; i++) btns[i].classList.remove('on');
  el.classList.add('on');
  craftType = el.dataset.ct;
}

function sendCraftedPacket() {
  var msgId = parseInt(document.getElementById('craftMsgId').value, 10);
  var hex = document.getElementById('craftData').value;
  if (isNaN(msgId)) { alert('Invalid msgId'); return; }
  sendRawToServer(craftType, msgId, hex);
}

function resendPacket() {
  if (selectedIdx < 0 || !filteredPackets[selectedIdx]) return;
  var pkt = filteredPackets[selectedIdx];
  if (pkt.dir === 'send') {
    var frame = buildSendFrame(pkt.msgId);
    if (!frame) { alert('No editor state — select packet first'); return; }
    sendRawToServer('send', pkt.msgId, u8ToHex(frame));
  } else {
    var proto = getEditorProtoBytes();
    if (!proto) { alert('No editor state — select packet first'); return; }
    sendRawToServer('recv', pkt.msgId, u8ToHex(proto));
  }
}

function editAndSend() {
  if (selectedIdx < 0 || !filteredPackets[selectedIdx]) return;
  var pkt = filteredPackets[selectedIdx];
  if (pkt.dir === 'send') {
    var frame = buildSendFrame(pkt.msgId);
    if (!frame) { alert('No editor state'); return; }
    document.getElementById('craftMsgId').value = pkt.msgId;
    document.getElementById('craftData').value = u8ToHex(frame);
    switchCraftMode('send');
  } else {
    var proto = getEditorProtoBytes();
    if (!proto) { alert('No editor state'); return; }
    document.getElementById('craftMsgId').value = pkt.msgId;
    document.getElementById('craftData').value = u8ToHex(proto);
    switchCraftMode('recv');
  }
  document.querySelector('.crafter').scrollIntoView({ behavior: 'smooth' });
}

function switchCraftMode(mode) {
  craftType = mode;
  var btns = document.querySelectorAll('.crafter .mtog .mbtn');
  btns.forEach(function (b) {
    if (b.dataset.ct === mode) b.classList.add('on');
    else b.classList.remove('on');
  });
}

// ─── Schema-based packet crafter ────────────────────────────────────────────
var _craftSchemaName = '';
var _craftFieldEls = []; // { field, input, subEls }

function populateCraftDropdown() {
  var sel = document.getElementById('craftSchema');
  if (!sel) return;
  // Clear existing options except the first placeholder
  while (sel.options.length > 1) sel.remove(1);
  // Collect CS* message names, sorted
  var names = [];
  for (var name in msgSchemas) {
    if (name.substring(0, 2) === 'CS') names.push(name);
  }
  names.sort();
  for (var i = 0; i < names.length; i++) {
    var opt = document.createElement('option');
    var schema = msgSchemas[names[i]];
    var id = schema && schema.msgId ? ' (' + schema.msgId + ')' : '';
    opt.value = names[i];
    opt.textContent = names[i] + id;
    sel.appendChild(opt);
  }
}

function selectCraftSchema(name) {
  var container = document.getElementById('craftFields');
  if (!container) return;
  container.innerHTML = '';
  _craftFieldEls = [];
  _craftSchemaName = name;

  if (!name || !msgSchemas[name]) { container.style.display = 'none'; return; }
  container.style.display = '';

  var schema = msgSchemas[name];
  if (schema.msgId && schema.msgId > 0) {
    document.getElementById('craftMsgId').value = schema.msgId;
  }
  switchCraftMode('send');
  _buildCraftFields(container, name, _craftFieldEls, 0);
}

function _buildCraftFields(container, schemaName, entries, depth) {
  var schema = msgSchemas[schemaName];
  if (!schema || !schema.fields) return;
  var overrides = msgOverrides[schemaName] || {};
  var fields = schema.fields;

  for (var i = 0; i < fields.length; i++) {
    var f = fields[i];
    var ov = overrides[f.name];
    var fieldNum = (ov && ov.num !== undefined) ? ov.num : f.n;
    var fieldType = (ov && ov.type !== undefined) ? ov.type : f.t;
    var fieldSub = (ov && ov.sub !== undefined) ? ov.sub : f.sub;

    var row = document.createElement('div');
    row.className = 'cf-row';

    var num = document.createElement('span');
    num.className = 'cf-num'; num.textContent = fieldNum;
    row.appendChild(num);

    var nm = document.createElement('span');
    nm.className = 'cf-name'; nm.textContent = f.name;
    nm.title = f.name;
    row.appendChild(nm);

    var tp = document.createElement('span');
    tp.className = 'cf-type';
    tp.textContent = fieldType + (f.r ? '[]' : '');
    row.appendChild(tp);

    var entry = { field: f, fieldNum: fieldNum, type: fieldType, sub: fieldSub, repeated: f.r };

    // Check if this is a message/bytes with a known sub-schema we can expand
    var subSchema = (fieldSub && depth < 3) ? msgSchemas[fieldSub] : null;
    var canExpand = subSchema && subSchema.fields && subSchema.fields.length > 0;

    if (fieldType === 'bool') {
      var cb = document.createElement('input');
      cb.type = 'checkbox'; cb.className = 'cf-check';
      row.appendChild(cb);
      entry.input = cb;
    } else if ((fieldType === 'message' || fieldType === 'bytes') && canExpand) {
      // Expandable sub-message with known schema
      var toggleBtn = document.createElement('button');
      toggleBtn.className = 'bc cf-expand';
      toggleBtn.textContent = '\u25b6 ' + fieldSub;
      row.appendChild(toggleBtn);

      // Hex input as fallback (hidden when builder is open)
      var inp = document.createElement('input');
      inp.type = 'text'; inp.className = 'cf-input';
      inp.placeholder = 'hex proto (or expand \u25b6 to build)';
      row.appendChild(inp);
      entry.input = inp;

      var subDiv = document.createElement('div');
      subDiv.className = 'cf-sub';
      subDiv.style.display = 'none';
      entry.subEntries = [];
      entry.useBuilder = false;

      container.appendChild(row);

      (function(e, sd, tb, ip, sn) {
        tb.addEventListener('click', function() {
          var open = sd.style.display === 'none';
          sd.style.display = open ? '' : 'none';
          ip.style.display = open ? 'none' : '';
          tb.textContent = (open ? '\u25bc ' : '\u25b6 ') + e.sub;
          e.useBuilder = open;
          if (open && e.subEntries.length === 0) {
            _buildCraftFields(sd, sn, e.subEntries, depth + 1);
          }
        });
      })(entry, subDiv, toggleBtn, inp, fieldSub);

      container.appendChild(subDiv);
      entries.push(entry);
      continue;
    } else if (fieldType === 'message' || fieldType === 'bytes') {
      var inp = document.createElement('input');
      inp.type = 'text'; inp.className = 'cf-input';
      inp.placeholder = fieldSub ? fieldSub + ' (hex proto)' : 'hex bytes';
      row.appendChild(inp);
      if (fieldSub) {
        var subLbl = document.createElement('span');
        subLbl.className = 'cf-sub-label'; subLbl.textContent = fieldSub;
        row.appendChild(subLbl);
      }
      entry.input = inp;
    } else if (fieldType === 'string') {
      var inp = document.createElement('input');
      inp.type = 'text'; inp.className = 'cf-input';
      inp.placeholder = 'string value';
      row.appendChild(inp);
      entry.input = inp;
    } else {
      var inp = document.createElement('input');
      inp.type = 'text'; inp.className = 'cf-input';
      inp.placeholder = f.r ? 'comma-separated values' : '0';
      row.appendChild(inp);
      entry.input = inp;
    }

    entries.push(entry);
    container.appendChild(row);
  }
}

function _buildProtoFromEntries(entries) {
  var parts = [];
  for (var i = 0; i < entries.length; i++) {
    var entry = entries[i];

    // Sub-message builder mode
    if (entry.subEntries && entry.useBuilder) {
      var subProto = _buildProtoFromEntries(entry.subEntries);
      if (subProto.length > 0) {
        var tag = _encodeVarint((entry.fieldNum << 3) | 2);
        var lenBytes = _encodeVarint(subProto.length);
        parts.push(tag); parts.push(lenBytes); parts.push(subProto);
      }
      continue;
    }

    var val = entry.input ? (entry.type === 'bool' ? entry.input.checked : entry.input.value.trim()) : '';
    if (val === '' || val === false) continue;

    var fn = entry.fieldNum;
    var wt = schemaTypeToWire(entry.type);

    if (entry.repeated && entry.type !== 'message' && entry.type !== 'bytes' && entry.type !== 'string') {
      var vals = String(val).split(',');
      for (var v = 0; v < vals.length; v++) {
        var sv = vals[v].trim();
        if (sv === '') continue;
        _appendProtoField(parts, fn, wt, entry.type, sv);
      }
    } else {
      _appendProtoField(parts, fn, wt, entry.type, val);
    }
  }

  var totalLen = 0;
  for (var i = 0; i < parts.length; i++) totalLen += parts[i].length;
  var result = new Uint8Array(totalLen);
  var off = 0;
  for (var i = 0; i < parts.length; i++) { result.set(parts[i], off); off += parts[i].length; }
  return result;
}

function sendCraftedSchema() {
  if (!_craftSchemaName) { alert('Select a CS* message first'); return; }
  var msgId = parseInt(document.getElementById('craftMsgId').value, 10);
  if (isNaN(msgId)) { alert('Invalid msgId'); return; }

  var proto = _buildProtoFromEntries(_craftFieldEls);

  // Build transport frame and send
  var fl = 16 + proto.length;
  var frame = new Uint8Array(4 + fl);
  _w32(frame, 0, fl);
  _w32(frame, 4, randomToken());
  _w32(frame, 8, 0x000002A8);
  _w32(frame, 12, 0);
  _w32(frame, 16, msgId);
  frame.set(proto, 20);

  sendRawToServer('send', msgId, u8ToHex(frame));
  showToast('Sent ' + _craftSchemaName + ' (' + proto.length + ' bytes proto)');
}

function _w32(buf, off, val) {
  buf[off] = (val >>> 24) & 0xff;
  buf[off+1] = (val >>> 16) & 0xff;
  buf[off+2] = (val >>> 8) & 0xff;
  buf[off+3] = val & 0xff;
}

function _appendProtoField(parts, fn, wt, type, val) {
  if (wt === 'vi') {
    // Varint
    var num;
    if (type === 'bool') num = val ? 1 : 0;
    else num = parseInt(val, 10) || 0;
    var tag = _encodeVarint((fn << 3) | 0);
    var vbytes = _encodeVarint(num);
    parts.push(tag); parts.push(vbytes);
  } else if (wt === 'ld') {
    // Length-delimited
    var data;
    if (type === 'string') {
      data = new TextEncoder().encode(val);
    } else {
      // hex bytes
      var hex = val.replace(/\s+/g, '');
      data = new Uint8Array(hex.length >> 1);
      for (var i = 0; i < data.length; i++) data[i] = parseInt(hex.substr(i * 2, 2), 16);
    }
    var tag = _encodeVarint((fn << 3) | 2);
    var lenBytes = _encodeVarint(data.length);
    parts.push(tag); parts.push(lenBytes); parts.push(data);
  } else if (wt === 'f32') {
    // Fixed 32
    var tag = _encodeVarint((fn << 3) | 5);
    var buf = new ArrayBuffer(4);
    new Float32Array(buf)[0] = parseFloat(val) || 0;
    parts.push(tag); parts.push(new Uint8Array(buf));
  } else if (wt === 'f64') {
    // Fixed 64
    var tag = _encodeVarint((fn << 3) | 1);
    var buf = new ArrayBuffer(8);
    new Float64Array(buf)[0] = parseFloat(val) || 0;
    parts.push(tag); parts.push(new Uint8Array(buf));
  }
}

function _encodeVarint(val) {
  if (val < 0) val = val >>> 0; // treat as unsigned
  var bytes = [];
  do {
    var b = val & 0x7f;
    val = val >>> 7;
    if (val > 0) b |= 0x80;
    bytes.push(b);
  } while (val > 0);
  return new Uint8Array(bytes);
}

// ─── Cell Analyzer (SCGuildExplore) ──────────────────────────────────────────
// Decoded JSON structure from /api/decode:
//   cell[i].position      → integer (packed grid position)
//   cell[i].interact      → null (no interactable) OR object:
//     .interactId          → integer
//     .hp                  → integer (monsters/destructibles)
//     .touchTimes          → integer (collectibles)
//     .state, .robTimes, .strength, .copyId, .castle, .rank, ...
//   cell[i].visible        → null OR array of player objects on the cell

function _findCellsInDecoded(data) {
  if (!data || !data.fields) return null;
  var f = data.fields;
  var map = f.map || f.Map;
  if (!map) return null;
  // map is a snappy-decoded sub-message: { _type, _fields: { cell: [...] } }
  var src = map._fields || map;
  var cells = src.cell || src.Cell;
  return Array.isArray(cells) ? cells : null;
}

function analyzeCells() {
  if (selectedIdx < 0) { alert('Select a packet first'); return; }
  var pkt = filteredPackets[selectedIdx];
  var key = _dcKey(pkt);
  var data = decodeCache[key];
  if (!data || !data.fields) { alert('No decoded data available — wait for decode to complete'); return; }

  var cells = _findCellsInDecoded(data);
  if (!cells || cells.length === 0) { alert('No cells found in decoded data'); return; }

  var withHp = [], withTouch = [], other = [];

  for (var i = 0; i < cells.length; i++) {
    var c = cells[i];
    if (!c || typeof c !== 'object') continue;

    var pos = c.position;
    var interact = c.interact;

    // Skip cells without interact — they're player cells (have visible[] instead)
    if (!interact || typeof interact !== 'object') {
      // Still count as "other" (player/static cells)
      other.push({ id: 0, pos: pos, hp: 0, touch: 0, hasPlayer: !!(c.visible) });
      continue;
    }

    var id = interact.interactId || 0;
    var hp = interact.hp || 0;
    var touch = interact.touchTimes || 0;

    var entry = { id: id, pos: pos, hp: hp, touch: touch };

    if (typeof hp === 'number' && hp > 0) withHp.push(entry);
    else if (typeof touch === 'number' && touch > 0) withTouch.push(entry);
    else other.push(entry);
  }

  _showCellAnalysis(cells.length, withHp, withTouch, other);
}

function _getMsgIdByName(name) {
  for (var id in msgDefs) { if (msgDefs[id] === name) return parseInt(id, 10); }
  return 0;
}

// Look up protobuf field number from loaded schema + protomap + overrides
function _getFieldNum(schemaName, fieldName) {
  var ov = (msgOverrides[schemaName] || {})[fieldName];
  if (ov && ov.num !== undefined) return ov.num;
  var pm = _lastProtoMap && _lastProtoMap[schemaName];
  if (pm && pm[fieldName] > 0) return pm[fieldName];
  var schema = msgSchemas[schemaName];
  if (schema && schema.fields) {
    for (var i = 0; i < schema.fields.length; i++) {
      if (schema.fields[i].name === fieldName && schema.fields[i].n > 0) return schema.fields[i].n;
    }
  }
  return 0;
}

// Concat Uint8Array parts into a single Uint8Array
function _concatParts(parts) {
  var totalLen = 0;
  for (var i = 0; i < parts.length; i++) totalLen += parts[i].length;
  var result = new Uint8Array(totalLen);
  var off = 0;
  for (var i = 0; i < parts.length; i++) { result.set(parts[i], off); off += parts[i].length; }
  return result;
}

// Build CSGuildExploreEvent proto using runtime schema field numbers
function _buildExploreEventProto(pos, interactId) {
  // Inner: GuildExploreMoveMsg
  var mn = 'GuildExploreMoveMsg';
  var moveParts = [];
  _appendProtoField(moveParts, _getFieldNum(mn, 'mapType'), 'vi', 'int32', '1');
  _appendProtoField(moveParts, _getFieldNum(mn, 'sourcePosition'), 'vi', 'int32', String(pos));
  _appendProtoField(moveParts, _getFieldNum(mn, 'targetPosition'), 'vi', 'int32', '0');
  _appendProtoField(moveParts, _getFieldNum(mn, 'interactId'), 'vi', 'int32', String(interactId));
  var moveBytes = _concatParts(moveParts);

  // Outer: CSGuildExploreEvent
  var en = 'CSGuildExploreEvent';
  var parts = [];
  var moveFieldNum = _getFieldNum(en, 'move');
  var tag = _encodeVarint((moveFieldNum << 3) | 2);
  var len = _encodeVarint(moveBytes.length);
  parts.push(tag); parts.push(len); parts.push(moveBytes);
  _appendProtoField(parts, _getFieldNum(en, 'eventPos'), 'vi', 'int32', String(pos));

  return _concatParts(parts);
}

function collectCell(pos, interactId, btn) {
  var msgId = _getMsgIdByName('CSGuildExploreEvent');
  if (!msgId) { alert('CSGuildExploreEvent msgId not found in defs'); return; }

  var proto = _buildExploreEventProto(pos, interactId);
  var fl = 16 + proto.length;
  var frame = new Uint8Array(4 + fl);
  _w32(frame, 0, fl);
  _w32(frame, 4, randomToken());
  _w32(frame, 8, 0x000002A8);
  _w32(frame, 12, 0);
  _w32(frame, 16, msgId);
  frame.set(proto, 20);

  sendRawToServer('send', msgId, u8ToHex(frame));
  if (btn) { btn.textContent = 'Sent'; btn.disabled = true; btn.style.opacity = '0.5'; }
  showToast('Collect id:' + interactId + ' pos:' + pos);
}

function collectAllCells(btn) {
  var overlay = btn.closest('.diag-overlay');
  if (!overlay || !overlay._cellData) return;
  var list = overlay._cellData.withTouch;
  if (list.length === 0) { alert('No collectible cells'); return; }
  var msgId = _getMsgIdByName('CSGuildExploreEvent');
  if (!msgId) { alert('CSGuildExploreEvent msgId not found'); return; }

  var delay = 0;
  for (var i = 0; i < list.length; i++) {
    (function(e, idx) {
      setTimeout(function() {
        var proto = _buildExploreEventProto(e.pos, e.id);
        var fl = 16 + proto.length;
        var frame = new Uint8Array(4 + fl);
        _w32(frame, 0, fl);
        _w32(frame, 4, randomToken());
        _w32(frame, 8, 0x000002A8);
        _w32(frame, 12, 0);
        _w32(frame, 16, msgId);
        frame.set(proto, 20);
        sendRawToServer('send', msgId, u8ToHex(frame));
        // Disable corresponding row button
        var rows = overlay.querySelectorAll('.ca-collect-btn');
        if (rows[idx]) { rows[idx].textContent = 'Sent'; rows[idx].disabled = true; rows[idx].style.opacity = '0.5'; }
      }, delay);
    })(list[i], i);
    delay += 200; // 200ms between each to avoid flood
  }
  btn.textContent = 'Sending ' + list.length + '...';
  btn.disabled = true;
  setTimeout(function() { btn.textContent = 'Done'; showToast('Collected ' + list.length + ' cells'); }, delay);
}

// Build CSGuildExploreCrushKill proto: field1=interactId, field2=eventPos
function _buildCrushKillProto(pos, interactId) {
  var sn = 'CSGuildExploreCrushKill';
  var parts = [];
  _appendProtoField(parts, _getFieldNum(sn, 'interactId'), 'vi', 'int32', String(interactId));
  _appendProtoField(parts, _getFieldNum(sn, 'eventPos'), 'vi', 'int32', String(pos));
  return _concatParts(parts);
}

function killCell(pos, interactId, btn) {
  var msgId = _getMsgIdByName('CSGuildExploreCrushKill');
  if (!msgId) { alert('CSGuildExploreCrushKill msgId not found'); return; }

  var proto = _buildCrushKillProto(pos, interactId);
  var fl = 16 + proto.length;
  var frame = new Uint8Array(4 + fl);
  _w32(frame, 0, fl);
  _w32(frame, 4, randomToken());
  _w32(frame, 8, 0x000002A8);
  _w32(frame, 12, 0);
  _w32(frame, 16, msgId);
  frame.set(proto, 20);

  sendRawToServer('send', msgId, u8ToHex(frame));
  if (btn) { btn.textContent = 'Sent'; btn.disabled = true; btn.style.opacity = '0.5'; }
  showToast('Kill id:' + interactId + ' pos:' + pos);
}

function killAllCells(btn) {
  var overlay = btn.closest('.diag-overlay');
  if (!overlay || !overlay._cellData) return;
  var list = overlay._cellData.withHp;
  if (list.length === 0) { alert('No HP cells'); return; }
  var msgId = _getMsgIdByName('CSGuildExploreCrushKill');
  if (!msgId) { alert('CSGuildExploreCrushKill msgId not found'); return; }

  var delay = 0;
  for (var i = 0; i < list.length; i++) {
    (function(e, idx) {
      setTimeout(function() {
        var proto = _buildCrushKillProto(e.pos, e.id);
        var fl = 16 + proto.length;
        var frame = new Uint8Array(4 + fl);
        _w32(frame, 0, fl);
        _w32(frame, 4, randomToken());
        _w32(frame, 8, 0x000002A8);
        _w32(frame, 12, 0);
        _w32(frame, 16, msgId);
        frame.set(proto, 20);
        sendRawToServer('send', msgId, u8ToHex(frame));
        var rows = overlay.querySelectorAll('.ca-kill-btn');
        if (rows[idx]) { rows[idx].textContent = 'Sent'; rows[idx].disabled = true; rows[idx].style.opacity = '0.5'; }
      }, delay);
    })(list[i], i);
    delay += 200;
  }
  btn.textContent = 'Killing ' + list.length + '...';
  btn.disabled = true;
  setTimeout(function() { btn.textContent = 'Done'; showToast('Killed ' + list.length + ' cells'); }, delay);
}

function _cellListHtml(title, color, list, extraCol, actions) {
  // actions: 'collect', 'kill', 'both', or falsy
  if (list.length === 0) return '<div class="ca-section"><div class="ca-title" style="color:' + color + '">' + title + ' (0)</div></div>';
  var html = '<div class="ca-section"><div class="ca-title" style="color:' + color + '">' + title + ' (' + list.length + ')</div>';
  html += '<table class="ca-table"><thead><tr><th>InteractId</th><th>Position</th><th>' + extraCol + '</th>'
    + (actions ? '<th></th>' : '') + '</tr></thead><tbody>';
  for (var i = 0; i < list.length; i++) {
    var e = list[i];
    var extra = extraCol === 'HP' ? e.hp : (extraCol === 'TouchTimes' ? e.touch : '—');
    html += '<tr><td>' + e.id + '</td><td>' + e.pos + '</td><td>' + extra + '</td>';
    if (actions) {
      html += '<td style="display:flex;gap:3px">';
      if (actions === 'kill' || actions === 'both') {
        html += '<button class="bp ca-kill-btn" style="padding:1px 8px;font-size:10px;background:var(--ac3)" onclick="killCell(' + e.pos + ',' + e.id + ',this)">Kill</button>';
      }
      if (actions === 'collect' || actions === 'both') {
        html += '<button class="bp ca-collect-btn" style="padding:1px 8px;font-size:10px" onclick="collectCell(' + e.pos + ',' + e.id + ',this)">Collect</button>';
      }
      html += '</td>';
    }
    html += '</tr>';
  }
  html += '</tbody></table></div>';
  return html;
}

// ─── Interact type classification ────────────────────────────────────────────
var _TREE_IDS = [10183, 10283, 10383, 10483, 10583, 10683];
var _STONE_IDS = [];
var _GOLD_IDS = [];
(function() {
  var stoneRanges = [[10121,10124],[10221,10224],[10321,10324],[10421,10424],[10521,10524],[10621,10624],[10721,10722]];
  for (var r = 0; r < stoneRanges.length; r++) for (var i = stoneRanges[r][0]; i <= stoneRanges[r][1]; i++) _STONE_IDS.push(i);
  var goldRanges = [[10101,10104],[10201,10204],[10301,10304],[10401,10404],[10501,10504],[10601,10604],[10701,10703]];
  for (var r = 0; r < goldRanges.length; r++) for (var i = goldRanges[r][0]; i <= goldRanges[r][1]; i++) _GOLD_IDS.push(i);
})();

var _GOLD_CART_IDS = [10181, 10281, 10381, 10481, 10581, 10681];
var _LESSER_ESSENCE_IDS = [];
var _GREATER_ESSENCE_IDS = [10186, 10286, 10386, 10486, 10586, 10686];
(function() {
  var ranges = [[10151,10152],[10251,10252],[10351,10352],[10451,10452],[10551,10552],[10651,10652]];
  for (var r = 0; r < ranges.length; r++) for (var i = ranges[r][0]; i <= ranges[r][1]; i++) _LESSER_ESSENCE_IDS.push(i);
})();

function _isTreeId(id) { return _TREE_IDS.indexOf(id) !== -1; }
function _isStoneId(id) { return _STONE_IDS.indexOf(id) !== -1; }
function _isGoldId(id) { return _GOLD_IDS.indexOf(id) !== -1; }
function _isGoldCartId(id) { return _GOLD_CART_IDS.indexOf(id) !== -1; }
function _isLesserEssenceId(id) { return _LESSER_ESSENCE_IDS.indexOf(id) !== -1; }
function _isGreaterEssenceId(id) { return _GREATER_ESSENCE_IDS.indexOf(id) !== -1; }

// Filter cells from all interact cells (touch + other + hp) by predicate on interactId
function _filterCellsById(cellData, predicate) {
  var all = (cellData.withTouch || []).concat(cellData.other || []).concat(cellData.withHp || []);
  var result = [];
  for (var i = 0; i < all.length; i++) {
    if (all[i].id && predicate(all[i].id)) result.push(all[i]);
  }
  return result;
}

function collectTypedCells(type, btn) {
  var overlay = btn.closest('.diag-overlay');
  if (!overlay || !overlay._cellData) return;
  var list = overlay._cellData['_' + type] || [];
  if (list.length === 0) { alert('None found'); return; }
  var msgId = _getMsgIdByName('CSGuildExploreEvent');
  if (!msgId) { alert('CSGuildExploreEvent msgId not found'); return; }

  var delay = 0;
  for (var i = 0; i < list.length; i++) {
    (function(e) {
      setTimeout(function() {
        var proto = _buildExploreEventProto(e.pos, e.id);
        var fl = 16 + proto.length;
        var frame = new Uint8Array(4 + fl);
        _w32(frame, 0, fl);
        _w32(frame, 4, randomToken());
        _w32(frame, 8, 0x000002A8);
        _w32(frame, 12, 0);
        _w32(frame, 16, msgId);
        frame.set(proto, 20);
        sendRawToServer('send', msgId, u8ToHex(frame));
      }, delay);
    })(list[i]);
    delay += 200;
  }
  btn.textContent = 'Sending ' + list.length + '...';
  btn.disabled = true;
  setTimeout(function() { btn.textContent = 'Done'; showToast('Sent ' + list.length + ' ' + type); }, delay);
}

function _quickActionsHtml(cellData) {
  var trees = _filterCellsById(cellData, _isTreeId);
  var stones = _filterCellsById(cellData, _isStoneId);
  var golds = _filterCellsById(cellData, _isGoldId);
  var goldCarts = _filterCellsById(cellData, _isGoldCartId);
  var lesserEss = _filterCellsById(cellData, _isLesserEssenceId);
  var greaterEss = _filterCellsById(cellData, _isGreaterEssenceId);
  cellData._trees = trees;
  cellData._stones = stones;
  cellData._golds = golds;
  cellData._goldCarts = goldCarts;
  cellData._lesserEss = lesserEss;
  cellData._greaterEss = greaterEss;

  var html = '<div class="ca-quick">';
  html += '<div class="ca-quick-title">Quick Actions</div>';
  html += '<div class="ca-quick-grid">';

  // Ancient Experience Tree
  var treeLabel = 'Ancient Exp Tree: ' + trees.length + 'H, ' + trees.length + ' action' + (trees.length !== 1 ? 's' : '');
  html += '<button class="bp ca-quick-btn" ' + (trees.length === 0 ? 'disabled style="opacity:0.4"' : '')
    + ' onclick="collectTypedCells(\'trees\',this)">' + treeLabel + '</button>';

  // Experience Stone
  var stoneXp = stones.length * 5000;
  var stoneLabel = 'Exp Stone: ' + (stoneXp > 0 ? stoneXp.toLocaleString() + ' xp' : '0') + ', ' + stones.length + ' action' + (stones.length !== 1 ? 's' : '');
  html += '<button class="bp ca-quick-btn" ' + (stones.length === 0 ? 'disabled style="opacity:0.4"' : '')
    + ' onclick="collectTypedCells(\'stones\',this)">' + stoneLabel + '</button>';

  // Gold Vein
  var goldAmt = golds.length * 7500;
  var goldLabel = 'Gold Vein: ' + (goldAmt > 0 ? goldAmt.toLocaleString() + ' gold' : '0') + ', ' + golds.length + ' action' + (golds.length !== 1 ? 's' : '');
  html += '<button class="bp ca-quick-btn" ' + (golds.length === 0 ? 'disabled style="opacity:0.4"' : '')
    + ' onclick="collectTypedCells(\'golds\',this)">' + goldLabel + '</button>';

  // Cart Full of Gold
  var cartLabel = 'Cart of Gold: ' + goldCarts.length + 'H, ' + goldCarts.length + ' action' + (goldCarts.length !== 1 ? 's' : '');
  html += '<button class="bp ca-quick-btn" ' + (goldCarts.length === 0 ? 'disabled style="opacity:0.4"' : '')
    + ' onclick="collectTypedCells(\'goldCarts\',this)">' + cartLabel + '</button>';

  // Lesser Dragon Essence
  var leLabel = 'Lesser Dragon Essence: ' + lesserEss.length + ', ' + lesserEss.length + ' action' + (lesserEss.length !== 1 ? 's' : '');
  html += '<button class="bp ca-quick-btn" ' + (lesserEss.length === 0 ? 'disabled style="opacity:0.4"' : '')
    + ' onclick="collectTypedCells(\'lesserEss\',this)">' + leLabel + '</button>';

  // Greater Dragon Essence
  var geLabel = 'Greater Dragon Essence: ' + greaterEss.length + ', ' + greaterEss.length + ' action' + (greaterEss.length !== 1 ? 's' : '');
  html += '<button class="bp ca-quick-btn" ' + (greaterEss.length === 0 ? 'disabled style="opacity:0.4"' : '')
    + ' onclick="collectTypedCells(\'greaterEss\',this)">' + geLabel + '</button>';

  // Placeholders
  var placeholders = [
    { name: 'Runes', key: 'runes' },
    { name: 'Dragon Soul', key: 'dragonsoul' },
    { name: 'Wyrms (1H + individual)', key: 'wyrms' },
    { name: 'Treasure Chest', key: 'chest' }
  ];
  for (var i = 0; i < placeholders.length; i++) {
    var p = placeholders[i];
    html += '<button class="bp ca-quick-btn" disabled style="opacity:0.3" title="Not yet mapped">'
      + p.name + ': ? (unmapped)</button>';
  }

  html += '</div></div>';
  return html;
}

function _showCellAnalysis(total, withHp, withTouch, other) {
  var overlay = document.createElement('div');
  overlay.className = 'diag-overlay';

  var playerCells = 0;
  for (var i = 0; i < other.length; i++) if (other[i].hasPlayer) playerCells++;

  var cellData = { withHp: withHp, withTouch: withTouch, other: other };

  var body = '<div class="ca-summary">'
    + 'Total cells: <strong>' + total + '</strong> &mdash; '
    + '<span style="color:var(--ac3)">HP: ' + withHp.length + '</span> &mdash; '
    + '<span style="color:var(--ac2)">Touch: ' + withTouch.length + '</span> &mdash; '
    + '<span style="color:var(--dim)">Other: ' + other.length + ' (' + playerCells + ' with players)</span>'
    + '</div>';

  body += _quickActionsHtml(cellData);

  body += _cellListHtml('Cells with HP (monsters/destructibles)', 'var(--ac3)', withHp, 'HP', 'kill');
  body += _cellListHtml('Cells with TouchTimes (collectibles)', 'var(--ac2)', withTouch, 'TouchTimes', 'collect');
  body += _cellListHtml('Cells with neither (static/other)', 'var(--dim)', other, '—', 'collect');

  overlay.innerHTML = '<div class="ca-popup">'
    + '<div class="ca-header">Cell Analysis — SCGuildExplore'
    + ' <div style="display:flex;gap:6px">'
    + '<button class="bp" style="font-size:10px;background:var(--ac3)" onclick="killAllCells(this)">Kill All</button>'
    + '<button class="bp" style="font-size:10px" onclick="collectAllCells(this)">Collect All</button>'
    + '<button class="bc ca-copy-btn" onclick="copyCellAnalysis(this)">Copy All</button>'
    + ' <button class="bc" onclick="this.closest(\'.diag-overlay\').remove()">Close</button></div></div>'
    + '<div class="ca-body">' + body + '</div></div>';

  overlay._cellData = cellData;
  document.body.appendChild(overlay);
}

function copyCellAnalysis(btn) {
  var overlay = btn.closest('.diag-overlay');
  var d = overlay._cellData;
  var lines = [];
  function addList(title, list, col) {
    lines.push('=== ' + title + ' (' + list.length + ') ===');
    for (var i = 0; i < list.length; i++) {
      var e = list[i];
      var extra = col === 'HP' ? e.hp : (col === 'TouchTimes' ? e.touch : '');
      lines.push('  id:' + e.id + '  pos:' + e.pos + (extra ? '  ' + col + ':' + extra : ''));
    }
    lines.push('');
  }
  addList('HP (monsters)', d.withHp, 'HP');
  addList('TouchTimes (collectibles)', d.withTouch, 'TouchTimes');
  addList('Other (static)', d.other, '');
  var text = lines.join('\n');
  if (navigator.clipboard) {
    navigator.clipboard.writeText(text).then(function() { showToast('Cell analysis copied'); });
  }
}

// ─── Split Resizer ──────────────────────────────────────────────────────────
(function () {
  var resizer = document.getElementById('splitResizer');
  var listPane = document.querySelector('.pkt-list-container');
  if (!resizer || !listPane) return;

  var startX, startW, dragging = false;

  resizer.addEventListener('mousedown', function (e) {
    e.preventDefault();
    startX = e.clientX;
    startW = listPane.offsetWidth;
    dragging = true;
    resizer.classList.add('dragging');
    document.body.style.cursor = 'col-resize';
    document.body.style.userSelect = 'none';
  });

  document.addEventListener('mousemove', function (e) {
    if (!dragging) return;
    var dx = e.clientX - startX;
    var newW = Math.max(200, Math.min(startW + dx, window.innerWidth * 0.7));
    listPane.style.width = newW + 'px';
  });

  document.addEventListener('mouseup', function () {
    if (!dragging) return;
    dragging = false;
    resizer.classList.remove('dragging');
    document.body.style.cursor = '';
    document.body.style.userSelect = '';
  });
})();

// ─── Init ───────────────────────────────────────────────────────────────────
connect();
