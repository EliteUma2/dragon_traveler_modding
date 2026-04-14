// Protocol registry — uses protobufjs to parse the generated .proto file,
// with post-processing for snappy-compressed and BattleOp fields.
import { readFileSync } from 'fs';
import { createRequire } from 'module';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { snappyDecompress } from './snappy.js';
import { decodeBattleOp } from './battleop.js';
import { decodeProtoToObject } from './protobuf.js';

const require = createRequire(import.meta.url);
const protobuf = require('protobufjs');

const __dirname = dirname(fileURLToPath(import.meta.url));

export class Protocol {
  constructor(msgdumpPath, overridesPath) {
    this._dumpPath = msgdumpPath || join(__dirname, '..', 'data', 'msgdump.json');
    this._ovPath = overridesPath || join(__dirname, '..', 'data', 'overrides.json');
    this._protoPath = join(__dirname, '..', 'proto', 'messages.proto');
    this._load();
  }

  _load() {
    // Load protobufjs root from the generated .proto file
    this.root = protobuf.loadSync(this._protoPath);

    // Load raw schemas for metadata (msgId, snappy, bop flags)
    const dump = JSON.parse(readFileSync(this._dumpPath, 'utf-8'));
    let overrides = {};
    try { overrides = JSON.parse(readFileSync(this._ovPath, 'utf-8')); } catch {}

    this.schemas = dump.schemas;
    this.enums = dump.enums;

    // Apply overrides to raw schemas (for snappy/bop metadata)
    for (const [msgName, fieldOverrides] of Object.entries(overrides)) {
      if (msgName.startsWith('_')) continue;
      const schema = this.schemas[msgName];
      if (!schema) continue;
      for (const [fieldName, ov] of Object.entries(fieldOverrides)) {
        const field = schema.fields.find(f => f.name === fieldName);
        if (!field) continue;
        if (ov.num !== undefined) field.n = ov.num;
        if (ov.sub !== undefined) field.sub = ov.sub;
        if (ov.snappy !== undefined) field.snappy = ov.snappy;
        if (ov.type !== undefined) field.t = ov.type;
      }
    }

    // Build msgId <-> name maps
    this.msgIdToName = new Map();
    this.nameToMsgId = new Map();
    this._typeCache = new Map();

    for (const [name, schema] of Object.entries(this.schemas)) {
      if (schema.msgId > 0) {
        this.msgIdToName.set(schema.msgId, name);
        this.nameToMsgId.set(name, schema.msgId);
      }
    }

    // Build set of fields needing post-processing (snappy/BattleOp)
    this._specialFields = new Map();
    for (const [msgName, schema] of Object.entries(this.schemas)) {
      const specials = [];
      for (const f of schema.fields) {
        if (f.snappy || f.bop) {
          specials.push({ name: f.name, snappy: !!f.snappy, bop: !!f.bop, sub: f.sub });
        }
      }
      if (specials.length > 0) this._specialFields.set(msgName, specials);
    }
  }

  reload() {
    this._load();
    console.log('\x1b[35m[hot]\x1b[0m Protocol reloaded (proto + schemas + overrides)');
  }

  // Lookup protobufjs Type, cached
  _getType(name) {
    let type = this._typeCache.get(name);
    if (!type) {
      try {
        type = this.root.lookupType('game.' + name);
        this._typeCache.set(name, type);
      } catch {
        return null;
      }
    }
    return type;
  }

  _resolveName(msgIdOrName) {
    if (typeof msgIdOrName === 'number') {
      return this.msgIdToName.get(msgIdOrName) || null;
    }
    return msgIdOrName;
  }

  getName(msgId) { return this.msgIdToName.get(msgId) || null; }
  getMsgId(name) { return this.nameToMsgId.get(name) || null; }
  getSchema(nameOrId) {
    const name = this._resolveName(nameOrId);
    return name ? this.schemas[name] || null : null;
  }

  // ── Decode ──────────────────────────────────────────────────────────────────

  decode(msgIdOrName, buf) {
    const name = this._resolveName(msgIdOrName);
    if (!name) return decodeProtoToObject(buf);

    const type = this._getType(name);
    if (!type) return decodeProtoToObject(buf);

    try {
      const msg = type.decode(buf instanceof Uint8Array ? buf : Uint8Array.from(buf));
      const obj = type.toObject(msg, {
        longs: Number,
        enums: Number,
        bytes: Buffer,
        defaults: false,
      });

      this._postProcessDecode(name, obj);
      return obj;
    } catch {
      // Schema field numbers may be wrong — fall back to generic wire decode
      return decodeProtoToObject(buf);
    }
  }

  _postProcessDecode(msgName, obj) {
    const schema = this.schemas[msgName];
    if (!schema) return;

    // Handle snappy/BattleOp fields
    const specials = this._specialFields.get(msgName);
    if (specials) {
      for (const spec of specials) {
        const val = obj[spec.name];
        if (!val) continue;

        const values = Array.isArray(val) ? val : [val];
        const results = [];

        for (const v of values) {
          const buf = Buffer.isBuffer(v) ? v : (v instanceof Uint8Array ? Buffer.from(v) : null);
          if (!buf) { results.push(v); continue; }

          if (spec.bop) {
            try { results.push(decodeBattleOp(buf)); }
            catch { results.push(buf.toString('hex')); }
          } else if (spec.snappy) {
            const decompressed = snappyDecompress(buf);
            if (decompressed && spec.sub) {
              const subType = this._getType(spec.sub);
              if (subType) {
                try {
                  const subMsg = subType.decode(decompressed);
                  const subObj = subType.toObject(subMsg, { longs: Number, enums: Number, bytes: Buffer, defaults: false });
                  this._postProcessDecode(spec.sub, subObj);
                  results.push(subObj);
                } catch { results.push(decompressed.toString('hex')); }
              } else {
                try { results.push(decodeProtoToObject(decompressed)); }
                catch { results.push(decompressed.toString('hex')); }
              }
            } else if (decompressed) {
              try { results.push(decodeProtoToObject(decompressed)); }
              catch { results.push(decompressed.toString('hex')); }
            } else {
              results.push(buf.toString('hex'));
            }
          }
        }

        obj[spec.name] = Array.isArray(val) ? results : results[0];
      }
    }

    // Recursively post-process nested message fields
    for (const fieldDef of schema.fields) {
      if (fieldDef.t !== 'message' || !fieldDef.sub) continue;
      const val = obj[fieldDef.name];
      if (!val) continue;

      if (Array.isArray(val)) {
        for (const item of val) {
          if (item && typeof item === 'object' && !Buffer.isBuffer(item)) {
            this._postProcessDecode(fieldDef.sub, item);
          }
        }
      } else if (typeof val === 'object' && !Buffer.isBuffer(val)) {
        this._postProcessDecode(fieldDef.sub, val);
      }
    }
  }

  // ── Encode ──────────────────────────────────────────────────────────────────

  encode(msgIdOrName, obj) {
    const name = this._resolveName(msgIdOrName);
    if (!name) throw new Error(`Unknown message: ${msgIdOrName}`);

    const type = this._getType(name);
    if (!type) throw new Error(`No protobuf type for: ${name}`);

    const prepared = this._prepareEncode(name, obj);
    const err = type.verify(prepared);
    if (err) throw new Error(`Verify ${name}: ${err}`);

    return Buffer.from(type.encode(type.create(prepared)).finish());
  }

  _prepareEncode(msgName, obj) {
    if (!obj || typeof obj !== 'object') return obj;
    const schema = this.schemas[msgName];
    if (!schema) return obj;

    const result = { ...obj };
    for (const fieldDef of schema.fields) {
      const val = result[fieldDef.name];
      if (val === undefined || val === null) continue;

      if (fieldDef.t === 'bytes' && typeof val === 'string') {
        result[fieldDef.name] = Buffer.from(val, 'hex');
      } else if (fieldDef.t === 'message' && fieldDef.sub) {
        if (Array.isArray(val)) {
          result[fieldDef.name] = val.map(v =>
            typeof v === 'object' && v !== null ? this._prepareEncode(fieldDef.sub, v) : v
          );
        } else if (typeof val === 'object') {
          result[fieldDef.name] = this._prepareEncode(fieldDef.sub, val);
        }
      }
    }
    return result;
  }

  // ── Helpers ─────────────────────────────────────────────────────────────────

  direction(msgId) {
    const name = this.msgIdToName.get(msgId);
    if (!name) return null;
    if (name.startsWith('CS')) return 'C2S';
    if (name.startsWith('SC')) return 'S2C';
    return null;
  }

  // Get the protobufjs Type directly for advanced usage
  getType(msgIdOrName) {
    const name = this._resolveName(msgIdOrName);
    return name ? this._getType(name) : null;
  }
}
