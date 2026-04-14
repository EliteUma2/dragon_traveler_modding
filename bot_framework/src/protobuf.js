// Standalone protobuf encoder/decoder (no external deps)
// Supports: varint, fixed32, fixed64, length-delimited, zigzag, snappy, BattleOp

import { snappyDecompress } from './snappy.js';
import { decodeBattleOp } from './battleop.js';

// ── Varint ──────────────────────────────────────────────────────────────────

export function encodeVarint(v) {
  const out = [];
  v = v >>> 0;
  do {
    out.push((v & 0x7f) | (v > 0x7f ? 0x80 : 0));
    v >>>= 7;
  } while (v);
  return out;
}

export function encodeVarint64(lo, hi) {
  const out = [];
  lo = lo >>> 0;
  hi = hi >>> 0;
  while (hi > 0 || lo > 0x7f) {
    out.push((lo & 0x7f) | 0x80);
    lo = ((lo >>> 7) | (hi << 25)) >>> 0;
    hi >>>= 7;
  }
  out.push(lo & 0x7f);
  return out;
}

export function decodeVarint(buf, pos) {
  let result = 0, shift = 0;
  while (pos < buf.length) {
    const b = buf[pos++];
    result |= (b & 0x7f) << shift;
    if (!(b & 0x80)) return { value: result >>> 0, pos };
    shift += 7;
    if (shift > 35) throw new Error('Varint too long');
  }
  throw new Error('Varint truncated');
}

export function decodeVarint64(buf, pos) {
  let lo = 0, hi = 0, shift = 0;
  const start = pos;
  while (pos < buf.length) {
    const b = buf[pos++];
    if (shift < 28) lo |= (b & 0x7f) << shift;
    else if (shift === 28) { lo |= (b & 0x0f) << 28; hi |= (b & 0x70) >> 4; }
    else hi |= (b & 0x7f) << (shift - 32);
    if (!(b & 0x80)) {
      lo = lo >>> 0;
      hi = hi >>> 0;
      return { lo, hi, pos };
    }
    shift += 7;
    if (shift > 63) throw new Error('Varint64 too long');
  }
  throw new Error('Varint64 truncated');
}

// ── ZigZag ──────────────────────────────────────────────────────────────────

export function zigzagEncode(n) { return ((n << 1) ^ (n >> 31)) >>> 0; }
export function zigzagDecode(n) { return ((n >>> 1) ^ -(n & 1)) | 0; }

// ── Tag ─────────────────────────────────────────────────────────────────────

export function encodeTag(fieldNum, wireType) {
  return encodeVarint((fieldNum << 3) | wireType);
}

// ── Generic proto decoder ───────────────────────────────────────────────────

export function decodeProto(buf, start = 0, end) {
  const limit = end ?? buf.length;
  const fields = [];
  let pos = start;

  while (pos < limit) {
    const tagResult = decodeVarint(buf, pos);
    const fieldNum = tagResult.value >>> 3;
    const wireType = tagResult.value & 7;
    pos = tagResult.pos;

    if (fieldNum === 0 || fieldNum > 536870911) break;

    const field = { fieldNum, wireType };

    if (wireType === 0) {
      // Varint
      const v = decodeVarint64(buf, pos);
      field.value = v.hi === 0 ? v.lo : { lo: v.lo, hi: v.hi };
      field.lo = v.lo;
      field.hi = v.hi;
      pos = v.pos;
    } else if (wireType === 1) {
      // 64-bit fixed
      if (pos + 8 > limit) throw new Error('Fixed64 truncated');
      field.value = buf.subarray(pos, pos + 8);
      pos += 8;
    } else if (wireType === 2) {
      // Length-delimited
      const lenResult = decodeVarint(buf, pos);
      pos = lenResult.pos;
      if (pos + lenResult.value > limit) throw new Error('LD truncated');
      field.value = Buffer.from(buf.subarray(pos, pos + lenResult.value));
      pos += lenResult.value;
    } else if (wireType === 5) {
      // 32-bit fixed
      if (pos + 4 > limit) throw new Error('Fixed32 truncated');
      field.value = buf.readUInt32LE(pos);
      pos += 4;
    } else {
      throw new Error(`Unknown wire type ${wireType}`);
    }

    fields.push(field);
  }

  return fields;
}

// ── Schema-aware decoder ────────────────────────────────────────────────────

export function decodeWithSchema(buf, schema, schemas) {
  const raw = decodeProto(buf);
  const result = {};

  for (const field of raw) {
    const def = schema.fields.find(f => f.n === field.fieldNum);
    if (!def) {
      // Unknown field
      const key = `_unknown_${field.fieldNum}`;
      if (result[key] !== undefined) {
        if (!Array.isArray(result[key])) result[key] = [result[key]];
        result[key].push(field.value);
      } else {
        result[key] = field.value;
      }
      continue;
    }

    let value;
    if (def.t === 'string' && field.wireType === 2) {
      value = field.value.toString('utf-8');
    } else if (def.t === 'bytes' && field.wireType === 2) {
      if (def.bop) {
        // BattleOp binary field — decode the custom format
        try {
          value = decodeBattleOp(field.value);
        } catch {
          value = field.value.toString('hex');
        }
      } else if (def.snappy) {
        // Snappy-compressed field — decompress, then try to decode as sub-message
        const decompressed = snappyDecompress(field.value);
        if (decompressed && def.sub && schemas[def.sub]) {
          try {
            value = decodeWithSchema(decompressed, schemas[def.sub], schemas);
          } catch {
            value = decompressed.toString('hex');
          }
        } else if (decompressed) {
          try {
            value = decodeProtoToObject(decompressed);
          } catch {
            value = decompressed.toString('hex');
          }
        } else {
          value = field.value.toString('hex');
        }
      } else {
        value = field.value.toString('hex');
      }
    } else if (def.t === 'bool' && field.wireType === 0) {
      value = field.lo !== 0;
    } else if ((def.t === 'int32' || def.t === 'enum') && field.wireType === 0) {
      value = field.lo > 0x7fffffff ? field.lo - 0x100000000 : field.lo;
    } else if (def.t === 'int64' && field.wireType === 0) {
      if (field.hi === 0) value = field.lo;
      else if (field.hi === 0xffffffff) value = field.lo - 0x100000000;
      else value = { lo: field.lo, hi: field.hi };
    } else if (def.t === 'double' && field.wireType === 1) {
      value = Buffer.from(field.value).readDoubleLE(0);
    } else if (def.t === 'float' && field.wireType === 5) {
      const tmp = Buffer.alloc(4);
      tmp.writeUInt32LE(field.value);
      value = tmp.readFloatLE(0);
    } else if (def.t === 'message' && field.wireType === 2 && def.sub && schemas[def.sub]) {
      let msgBuf = field.value;
      if (def.snappy) {
        const decompressed = snappyDecompress(field.value);
        if (decompressed) msgBuf = decompressed;
      }
      value = decodeWithSchema(msgBuf, schemas[def.sub], schemas);
    } else if (def.t === 'message' && field.wireType === 2) {
      // Unknown sub-message, decode raw
      try {
        value = decodeProtoToObject(field.value);
      } catch {
        value = field.value.toString('hex');
      }
    } else {
      value = field.value;
    }

    const key = def.name;
    if (def.r) {
      if (!result[key]) result[key] = [];
      result[key].push(value);
    } else {
      result[key] = value;
    }
  }

  return result;
}

// Decode raw proto to a generic object (no schema)
export function decodeProtoToObject(buf) {
  const raw = decodeProto(buf);
  const result = {};

  for (const field of raw) {
    const key = `f${field.fieldNum}`;
    let value;

    if (field.wireType === 0) {
      value = field.hi === 0 ? field.lo : { lo: field.lo, hi: field.hi };
    } else if (field.wireType === 2) {
      // Try to decode as string
      const bytes = field.value;
      const isUtf8 = bytes.every(b => b >= 0x20 && b < 0x7f || b === 0x0a || b === 0x0d);
      if (isUtf8 && bytes.length > 0 && bytes.length < 200) {
        value = bytes.toString('utf-8');
      } else {
        // Try nested proto
        try {
          const nested = decodeProto(bytes);
          if (nested.length > 0) {
            value = decodeProtoToObject(bytes);
          } else {
            value = bytes.toString('hex');
          }
        } catch {
          value = bytes.toString('hex');
        }
      }
    } else if (field.wireType === 5) {
      value = field.value;
    } else if (field.wireType === 1) {
      value = Buffer.from(field.value).toString('hex');
    }

    // Handle repeated fields
    if (result[key] !== undefined) {
      if (!Array.isArray(result[key])) result[key] = [result[key]];
      result[key].push(value);
    } else {
      result[key] = value;
    }
  }

  return result;
}

// ── Schema-aware encoder ────────────────────────────────────────────────────

export function encodeWithSchema(obj, schema, schemas) {
  const out = [];

  for (const def of schema.fields) {
    const val = obj[def.name];
    if (val === undefined || val === null) continue;

    const values = def.r ? (Array.isArray(val) ? val : [val]) : [val];

    for (const v of values) {
      if (def.t === 'string') {
        const bytes = Buffer.from(String(v), 'utf-8');
        out.push(...encodeTag(def.n, 2));
        out.push(...encodeVarint(bytes.length));
        for (const b of bytes) out.push(b);
      } else if (def.t === 'bytes') {
        const bytes = typeof v === 'string' ? Buffer.from(v, 'hex') : Buffer.from(v);
        out.push(...encodeTag(def.n, 2));
        out.push(...encodeVarint(bytes.length));
        for (const b of bytes) out.push(b);
      } else if (def.t === 'bool') {
        out.push(...encodeTag(def.n, 0));
        out.push(v ? 1 : 0);
      } else if (def.t === 'int32' || def.t === 'enum') {
        out.push(...encodeTag(def.n, 0));
        const uval = v < 0 ? (v + 0x100000000) >>> 0 : v >>> 0;
        out.push(...encodeVarint(uval));
      } else if (def.t === 'int64') {
        out.push(...encodeTag(def.n, 0));
        if (typeof v === 'number') {
          if (v >= 0 && v < 0x100000000) {
            out.push(...encodeVarint64(v >>> 0, 0));
          } else if (v < 0) {
            const lo = (v + 0x100000000) >>> 0;
            out.push(...encodeVarint64(lo, 0xffffffff));
          } else {
            const lo = v >>> 0;
            const hi = ((v - lo) / 0x100000000) >>> 0;
            out.push(...encodeVarint64(lo, hi));
          }
        } else if (typeof v === 'bigint') {
          const lo = Number(v & 0xffffffffn) >>> 0;
          const hi = Number((v >> 32n) & 0xffffffffn) >>> 0;
          out.push(...encodeVarint64(lo, hi));
        } else {
          out.push(...encodeVarint(0));
        }
      } else if (def.t === 'double') {
        out.push(...encodeTag(def.n, 1));
        const tmp = Buffer.alloc(8);
        tmp.writeDoubleLE(v);
        for (const b of tmp) out.push(b);
      } else if (def.t === 'float') {
        out.push(...encodeTag(def.n, 5));
        const tmp = Buffer.alloc(4);
        tmp.writeFloatLE(v);
        for (const b of tmp) out.push(b);
      } else if (def.t === 'message' && def.sub && schemas[def.sub]) {
        const subBytes = Buffer.from(encodeWithSchema(v, schemas[def.sub], schemas));
        out.push(...encodeTag(def.n, 2));
        out.push(...encodeVarint(subBytes.length));
        for (const b of subBytes) out.push(b);
      } else if (def.t === 'message') {
        // Unknown sub — pass raw bytes
        const bytes = typeof v === 'string' ? Buffer.from(v, 'hex') : Buffer.from(v);
        out.push(...encodeTag(def.n, 2));
        out.push(...encodeVarint(bytes.length));
        for (const b of bytes) out.push(b);
      }
    }
  }

  return Buffer.from(out);
}
