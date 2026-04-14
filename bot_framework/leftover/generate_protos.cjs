const fs = require('fs');
const path = require('path');

// ── Paths ──────────────────────────────────────────────────────────────────────
const MSGDUMP_PATH   = path.join(__dirname, 'data', 'msgdump.json');
const OVERRIDES_PATH = path.join(__dirname, 'data', 'overrides.json');
const PROTO_DIR      = path.join(__dirname, 'proto');
const MSGID_MAP_PATH = path.join(__dirname, 'proto', 'msgid_map.json');

// ── Load data ──────────────────────────────────────────────────────────────────
const dump      = JSON.parse(fs.readFileSync(MSGDUMP_PATH, 'utf8'));
const overrides = JSON.parse(fs.readFileSync(OVERRIDES_PATH, 'utf8'));

const schemas = dump.schemas;
const enums   = dump.enums || {};

// ── Apply overrides ────────────────────────────────────────────────────────────
for (const [msgName, fieldOverrides] of Object.entries(overrides)) {
  if (msgName.startsWith('_')) continue; // skip comments
  const schema = schemas[msgName];
  if (!schema) {
    console.warn(`Override target "${msgName}" not found in schemas, skipping.`);
    continue;
  }
  for (const [fieldName, ov] of Object.entries(fieldOverrides)) {
    const field = schema.fields.find(f => f.name === fieldName);
    if (!field) {
      console.warn(`Override field "${msgName}.${fieldName}" not found, skipping.`);
      continue;
    }
    if (ov.num  !== undefined) field.n   = ov.num;
    if (ov.sub  !== undefined) field.sub = ov.sub;
    if (ov.type !== undefined) field.t   = ov.type;
    if (ov.snappy) field._snappy = true;
  }
}

// ── Fix duplicate field numbers ──────────────────────────────────────────────
// Overrides can cause collisions with sequential numbering. For each message,
// give priority to overridden fields and renumber colliding auto-numbered ones.
const overriddenFields = new Set();
for (const [msgName, fieldOverrides] of Object.entries(overrides)) {
  if (msgName.startsWith('_')) continue;
  for (const [fieldName, ov] of Object.entries(fieldOverrides)) {
    if (ov.num !== undefined) overriddenFields.add(msgName + '.' + fieldName);
  }
}

for (const [msgName, schema] of Object.entries(schemas)) {
  const fields = schema.fields || [];
  const used = new Set(fields.map(f => f.n));
  const seen = new Set();
  for (const f of fields) {
    if (seen.has(f.n)) {
      // Collision — if this field was NOT overridden, bump it
      if (!overriddenFields.has(msgName + '.' + f.name)) {
        let next = Math.max(...fields.map(x => x.n)) + 1;
        f.n = next;
        used.add(next);
      }
    }
    seen.add(f.n);
  }
}

// ── Type mapping ───────────────────────────────────────────────────────────────
const TYPE_MAP = {
  'string': 'string',
  'int32':  'int32',
  'int64':  'int64',
  'bool':   'bool',
  'bytes':  'bytes',
  'double': 'double',
  'float':  'float',
};

function protoType(field) {
  if (field.bop) return 'bytes';               // BattleOp binary (custom format, not proto)
  if (field.t === 'enum')    return 'int32';    // enums on wire are int32
  if (field.t === 'message') return field.sub || 'bytes';
  // bytes fields that are actually compressed sub-messages (from overrides)
  if (field.t === 'bytes' && field.sub) return field.sub;
  return TYPE_MAP[field.t] || 'bytes';
}

function protoLabel(field) {
  if (field.r) return 'repeated';
  return 'optional'; // game uses protobuf-lite, never enforces required
}

// ── Collect referenced sub-message names (to order definitions) ────────────────
// All message names from schemas
const allMsgNames = new Set(Object.keys(schemas));

// ── Generate .proto content ────────────────────────────────────────────────────
const lines = [];
lines.push('// Auto-generated from msgdump.json');
lines.push('// Generated: ' + new Date().toISOString());
lines.push('syntax = "proto2";');
lines.push('package game;');
lines.push('');

// ── Enums ──────────────────────────────────────────────────────────────────────
const enumNames = Object.keys(enums).sort();
if (enumNames.length > 0) {
  lines.push('// ═══════════════════════════════════════════════════════════════════');
  lines.push('//  Enums');
  lines.push('// ═══════════════════════════════════════════════════════════════════');
  lines.push('');

  for (const eName of enumNames) {
    const eValues = enums[eName];
    lines.push('enum ' + eName + ' {');
    // Sort by numeric value
    const entries = Object.entries(eValues).sort((a, b) => a[1] - b[1]);
    for (const [vName, vNum] of entries) {
      lines.push('  ' + vName + ' = ' + vNum + ';');
    }
    lines.push('}');
    lines.push('');
  }
}

// ── Messages ───────────────────────────────────────────────────────────────────
lines.push('// ═══════════════════════════════════════════════════════════════════');
lines.push('//  Messages');
lines.push('// ═══════════════════════════════════════════════════════════════════');
lines.push('');

// Sort: sub-messages (msgId === -1) first, then by name
const msgNames = Object.keys(schemas).sort((a, b) => {
  const aId = schemas[a].msgId;
  const bId = schemas[b].msgId;
  const aIsSub = (aId === -1) ? 0 : 1;
  const bIsSub = (bId === -1) ? 0 : 1;
  if (aIsSub !== bIsSub) return aIsSub - bIsSub;
  return a.localeCompare(b);
});

for (const msgName of msgNames) {
  const schema = schemas[msgName];
  const msgId  = schema.msgId;

  // Comment with msgId
  if (msgId !== -1) {
    lines.push('// msgId = ' + msgId);
  }

  lines.push('message ' + msgName + ' {');

  const fields = schema.fields || [];
  for (const f of fields) {
    const label = protoLabel(f);
    const type  = protoType(f);
    const num   = f.n;

    // Annotations for special encoding
    let comment = '';
    if (f._snappy && f.sub) comment = ' // wire: snappy-compressed ' + f.sub;
    else if (f._snappy)     comment = ' // wire: snappy-compressed bytes';
    if (f.bop)              comment = ' // wire: BattleOp binary (custom format, not proto)';

    if (label === 'repeated') {
      lines.push('  repeated ' + type + ' ' + f.name + ' = ' + num + ';' + comment);
    } else {
      lines.push('  ' + label + ' ' + type + ' ' + f.name + ' = ' + num + ';' + comment);
    }
  }

  lines.push('}');
  lines.push('');
}

// ── Write .proto file ──────────────────────────────────────────────────────────
fs.mkdirSync(PROTO_DIR, { recursive: true });
const protoPath = path.join(PROTO_DIR, 'messages.proto');
fs.writeFileSync(protoPath, lines.join('\n'), 'utf8');
console.log('Wrote ' + protoPath + ' (' + msgNames.length + ' messages, ' + enumNames.length + ' enums)');

// ── Generate msgid_map.json ────────────────────────────────────────────────────
const msgIdMap = {};
for (const [name, schema] of Object.entries(schemas)) {
  if (schema.msgId !== -1) {
    msgIdMap[schema.msgId] = name;
  }
}
fs.writeFileSync(MSGID_MAP_PATH, JSON.stringify(msgIdMap, null, 2), 'utf8');
console.log('Wrote ' + MSGID_MAP_PATH + ' (' + Object.keys(msgIdMap).length + ' entries)');
