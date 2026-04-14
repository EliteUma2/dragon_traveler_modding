// BattleOp binary format decoder/encoder
// Format: sequence of entries, each: [4B totalLen LE][4B header LE][proto fields...]
// Proto inside each entry: f1=itemId(vi), f2=entityInfo(ld>{f1=entityBlob(ld)}), f4=seq(vi), f5=tick(vi)

import { decodeProto, decodeProtoToObject } from './protobuf.js';

function readLE32(buf, off) {
  return ((buf[off + 3] << 24) | (buf[off + 2] << 16) | (buf[off + 1] << 8) | buf[off]) >>> 0;
}

function readLE32Signed(buf, off) {
  const v = readLE32(buf, off);
  return v > 0x7fffffff ? v - 0x100000000 : v;
}

function readLE64(buf, off) {
  const lo = readLE32(buf, off), hi = readLE32(buf, off + 4);
  if (hi === 0xffffffff) return lo - 0x100000000;
  if (hi === 0) return lo;
  if (hi > 0x7fffffff) return -(0x100000000 * (0xffffffff - hi) + (0x100000000 - lo));
  return hi * 0x100000000 + lo;
}

function decodeEntity(raw) {
  if (raw.length < 16) return { type: 'unknown', size: raw.length };
  const size = readLE32(raw, 0);
  const type = readLE32(raw, 4);
  const uuid = raw.subarray(8, 16).toString('hex');
  const entity = { size, type, typeName: type === 1 ? 'hero' : type === 2 ? 'spell' : `t${type}`, uuid };

  if (type === 1 && raw.length >= 17) {
    entity.autoBattle = raw[16] !== 0;
  } else if (type === 2 && raw.length >= 47) {
    entity.marker = readLE32(raw, 16);
    entity.f1 = readLE32Signed(raw, 20);
    entity.valueA = readLE64(raw, 24);
    entity.valueB = readLE64(raw, 32);
    entity.flagC = raw[40];
    entity.flagD = raw[41];
    entity.flagE = raw[42];
    entity.serial = readLE32Signed(raw, 43);
  }
  return entity;
}

export function decodeBattleOp(buf) {
  const entries = [];
  let pos = 0;

  while (pos + 8 <= buf.length) {
    const totalLen = readLE32(buf, pos);
    if (totalLen < 4 || pos + 4 + totalLen > buf.length) break;

    const header = readLE32(buf, pos + 4);
    let fields;
    try {
      fields = decodeProto(buf, pos + 8, pos + 4 + totalLen);
    } catch {
      fields = [];
    }

    const entry = { header };
    for (const f of fields) {
      if (f.fieldNum === 1 && f.wireType === 0) entry.itemId = f.lo;
      if (f.fieldNum === 4 && f.wireType === 0) entry.seq = f.lo;
      if (f.fieldNum === 5 && f.wireType === 0) {
        // Check for -1 sentinel (empty tick)
        entry.tick = f.lo;
        entry.emptyTick = (f.hi === 0xffffffff && f.lo === 0xffffffff);
        if (entry.emptyTick) entry.tick = null;
      }
      if (f.fieldNum === 2 && f.wireType === 2) {
        // Entity info wrapper: nested proto with f1 = entity blob
        try {
          const inner = decodeProto(f.value, 0, f.value.length);
          for (const inf of inner) {
            if (inf.fieldNum === 1 && inf.wireType === 2) {
              entry.entity = decodeEntity(inf.value);
              break;
            }
          }
        } catch {}
      }
    }

    entries.push(entry);
    pos += 4 + totalLen;
  }

  return entries;
}
