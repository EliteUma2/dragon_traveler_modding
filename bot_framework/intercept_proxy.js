#!/usr/bin/env node
// Intercepting proxy PoC — drop, modify, and resequence packets
import { createProxy } from './src/intercept.js';
import { Protocol } from './src/protocol.js';

const protocol = new Protocol();

const proxy = createProxy({
  listenPort: 9003,
  targetHost: 'globalphoenix3746-ajm03f.game-tree.com',
  targetPort: 9002,
  protocol,
});

// ── Example rules ────────────────────────────────────────────────────────────

// 1. Drop specific packets entirely (both directions)
// proxy.drop('CSHeartbeat');

// 2. Intercept S→C: modify server response before it reaches the client
// proxy.intercept('SCLogin', (pkt) => {
//   // Modify any field in the decoded data
//   pkt.data.serverTime = 0;
//   return pkt;        // return pkt to forward (modified)
//   // return null;    // return null to drop
// }, 'S2C');

// 3. Intercept C→S: modify client request before it reaches the server
// proxy.intercept('CSBuyItem', (pkt) => {
//   pkt.data.count = 99;
//   return pkt;
// }, 'C2S');

// 4. Conditional drop
// proxy.intercept('SCChatMsg', (pkt) => {
//   if (pkt.data.channel === 1) return null; // drop world chat
//   return pkt;                              // forward others
// }, 'S2C');

// ── Tap: log + modify gacha packets by msgId ────────────────────────────────
// Hook by msgId directly since name lookup can be offset in this framework.
//   3015 = SCAddFighters (addFighter array — we rewrite templateId + starLv)
//   8802 = SCUpdateHeroGachaTimes (resTid array — we rewrite to top-tier)
// Use a shared queue so fighter-adds and resTid reveals use the SAME ids
// for a given pull (one gacha produces one SCAddFighters + one SCUpdateHeroGachaTimes).
// Pool of heroes to inject for gacha pulls
const TOP_TIER_HEROES = [
  1605, // Caesar
  2605, // Spimo
  4604, // Asteros
  6604, // Talos
];

// Deterministic mapping: same oldTid always → same newTid
// (so SCAddFighters and SCUpdateHeroGachaTimes agree on every pull)
function pickForTid(oldTid) {
  return TOP_TIER_HEROES[Math.abs(Number(oldTid)) % TOP_TIER_HEROES.length];
}

// SSR-Ex (Quality=7) mythology pool — only currently-released heroes with models
// (Thor 4702, Artemis 5702, Loki 6702 excluded — Show=0 or future ShowTime)
const SSR_EX_POOL_WEIGHTED = [
  6701, 6701, 6701,   // Poseidon x3 (main favorite)
  1701, // Athena
  2701, // Aphrodite
  3701, // Persephone
  4701, // Ares
  5701, // Apollo
];
const randSSREx = () => SSR_EX_POOL_WEIGHTED[Math.floor(Math.random() * SSR_EX_POOL_WEIGHTED.length)];

// SSR hero IDs (Quality=5) — we only swap a fighter if the original was one of these
const SSR_IDS = new Set([
  1501, 1502, 1503, 1504, 1505,
  2501, 2502, 2503,
  3501, 3502, 3503, 3504,
  4501, 4502, 4503, 4504,
  5501, 5502, 5503,
  6501, 6502, 6503, 6504, 6505,
]);

// Replacement pool: curated unreleased SSR+ (Q6) heroes with working assets
const UNRELEASED_POOL = [
  1605, // Caesar
  2605, // Spimo
  4604, // Asteros
  4608, // Asmodeus
  5603, // Orion
  5604, // Medusa
  5605, // Ningrose
  5606, // Mimir
  6604, // Talos
  6605, // Cleopatra
];
const randUnreleased = () => UNRELEASED_POOL[Math.floor(Math.random() * UNRELEASED_POOL.length)];

// ── Raw proto tree walk/rewrite (schema-free, byte-perfect preservation) ─────
// Wire format:
//   tag = varint( (fieldNum << 3) | wireType )
//   wireType 0 = varint value
//   wireType 1 = 8-byte fixed
//   wireType 2 = length-prefixed bytes
//   wireType 5 = 4-byte fixed
function readVarint(buf, pos) {
  let v = 0n, shift = 0n;
  while (pos < buf.length) {
    const b = buf[pos++];
    v |= BigInt(b & 0x7f) << shift;
    if (!(b & 0x80)) return { value: v, pos };
    shift += 7n;
  }
  throw new Error('varint truncated');
}
function encVarint(v) {
  const out = [];
  v = BigInt(v);
  while (v > 0x7fn) {
    out.push(Number(v & 0x7fn) | 0x80);
    v >>= 7n;
  }
  out.push(Number(v));
  return out;
}

// Parse a length-delimited payload into a tree of { num, wire, value, bytes, children? }
function parseProto(buf, start = 0, end = buf.length) {
  const fields = [];
  let pos = start;
  while (pos < end) {
    const t = readVarint(buf, pos); pos = t.pos;
    const tag = Number(t.value);
    const num = tag >>> 3;
    const wire = tag & 7;
    if (wire === 0) {
      const v = readVarint(buf, pos); pos = v.pos;
      fields.push({ num, wire, value: v.value });
    } else if (wire === 1) {
      fields.push({ num, wire, value: buf.subarray(pos, pos + 8) }); pos += 8;
    } else if (wire === 2) {
      const l = readVarint(buf, pos); pos = l.pos;
      const len = Number(l.value);
      const bytes = buf.subarray(pos, pos + len); pos += len;
      fields.push({ num, wire, bytes });
    } else if (wire === 5) {
      fields.push({ num, wire, value: buf.subarray(pos, pos + 4) }); pos += 4;
    } else {
      throw new Error(`bad wire type ${wire}`);
    }
  }
  return fields;
}

// Serialize a field list back to bytes
function serializeProto(fields) {
  const out = [];
  for (const f of fields) {
    const tag = (f.num << 3) | f.wire;
    out.push(...encVarint(tag));
    if (f.wire === 0) {
      out.push(...encVarint(f.value));
    } else if (f.wire === 1) {
      for (const b of f.value) out.push(b);
    } else if (f.wire === 2) {
      out.push(...encVarint(f.bytes.length));
      for (const b of f.bytes) out.push(b);
    } else if (f.wire === 5) {
      for (const b of f.value) out.push(b);
    }
  }
  return Buffer.from(out);
}

// Recursively replace every varint occurrence of oldVal with newVal anywhere in
// a parsed proto tree. For length-delimited fields, try to parse as nested proto;
// if that succeeds cleanly, recurse and re-serialize. Otherwise leave the bytes
// untouched (could be a string/raw bytes field).
function replaceVarintEverywhere(fields, oldVal, newVal) {
  let count = 0;
  for (const f of fields) {
    if (f.wire === 0) {
      if (Number(f.value) === oldVal) {
        f.value = BigInt(newVal);
        count++;
      }
    } else if (f.wire === 2 && f.bytes && f.bytes.length > 0) {
      // Try to parse as nested proto — if clean, recurse
      let nested;
      try { nested = parseProto(f.bytes); }
      catch { nested = null; }
      if (nested && nested.length > 0 && looksLikeProto(nested, f.bytes.length)) {
        const inner = replaceVarintEverywhere(nested, oldVal, newVal);
        if (inner > 0) {
          f.bytes = serializeProto(nested);
          count += inner;
        }
      }
    }
  }
  return count;
}

// Heuristic: a parsed tree looks like proto if every field has a sensible num
// and re-serialization matches the input length (i.e. full coverage, no garbage).
function looksLikeProto(fields, originalLen) {
  try {
    if (fields.some(f => f.num <= 0 || f.num > 536870911)) return false;
    const re = serializeProto(fields);
    return re.length === originalLen;
  } catch { return false; }
}

// Rewrite SCFighterBagMsg raw bytes: find every FighterMsg with starLv >= 5,
// pick a random unreleased hero for each, and recursively replace all varints
// equal to that fighter's old templateId anywhere in the packet.
// Structure: SCFighterBagMsg{ f1: FighterBagMsg{ f1: [FighterMsg{f1:id, f2:tid, f3:starLv}] } }
function rewriteFighterBagRaw(raw) {
  const top = parseProto(raw);

  // Stage 1: collect unique oldTids of fighters that are SSR (Q=5) AND starLv >= 5
  const oldTids = new Set();
  for (const topField of top) {
    if (topField.num !== 1 || topField.wire !== 2) continue;
    const bagFields = parseProto(topField.bytes);
    for (const bf of bagFields) {
      if (bf.num !== 1 || bf.wire !== 2) continue;
      const fMsg = parseProto(bf.bytes);
      const starField = fMsg.find(x => x.num === 3 && x.wire === 0);
      if (!starField || Number(starField.value) < 5) continue;
      const tidField = fMsg.find(x => x.num === 2 && x.wire === 0);
      if (!tidField) continue;
      const tid = Number(tidField.value);
      if (!SSR_IDS.has(tid)) continue; // only swap SSR (Quality=5)
      oldTids.add(tid);
    }
  }
  if (oldTids.size === 0) return raw;

  // Stage 2: for each oldTid, pick a unique unreleased replacement and do global varint replace
  const usedPicks = new Set();
  let totalReplacements = 0;
  for (const oldTid of oldTids) {
    // Pick an unreleased tid not already used (prevents duplicates in the roster)
    let newTid;
    for (let tries = 0; tries < 20; tries++) {
      const candidate = randUnreleased();
      if (!usedPicks.has(candidate) && candidate !== oldTid) { newTid = candidate; break; }
    }
    if (newTid === undefined) newTid = randUnreleased();
    usedPicks.add(newTid);
    const n = replaceVarintEverywhere(top, oldTid, newTid);
    totalReplacements += n;
    console.log(`\x1b[35m[bag-swap] 5★+ tid ${oldTid} → ${newTid} (${n} occurrences)\x1b[0m`);
  }
  console.log(`\x1b[35m[bag-swap] Swapped ${oldTids.size} templates, ${totalReplacements} total varint replacements\x1b[0m`);
  return serializeProto(top);
}
const randTop = () => TOP_TIER_HEROES[Math.floor(Math.random() * TOP_TIER_HEROES.length)];

proxy.tap((pkt) => {
  if (pkt.direction === 'S2C') {
    // Real SCAddFighters — msgId 3015
    if (pkt.msgId === 3015) {
      const d = pkt.data;
      const fighters = d?.addFighter || d?.f1;
      if (Array.isArray(fighters) && fighters.length > 0) {
        for (let i = 0; i < fighters.length; i++) {
          const f = fighters[i];
          const oldTid = f.templateId ?? f.f2;
          const newTid = pickForTid(oldTid); // deterministic per oldTid
          const oldStar = f.starLv ?? f.f3;
          if ('templateId' in f) f.templateId = newTid; else f.f2 = newTid;
          if ('starLv' in f) f.starLv = 5; else f.f3 = 5;
          if ('firstGet' in f) f.firstGet = 1; else f.f5 = 1;
          if ('fromType' in f) f.fromType = 0; else f.f6 = 0;
          console.log(`\x1b[35m[gacha-swap] Fighter tid ${oldTid} → ${newTid} (★${oldStar}→5, firstGet=1, fromType=0)\x1b[0m`);
        }
        pkt._modified = true;
      }
    }
    // Real SCUpdateHeroGachaTimes — msgId 8802
    else if (pkt.msgId === 8802) {
      const d = pkt.data;
      if (!d) return;
      // Deterministic mapping: each resTid value is swapped based on its oldTid,
      // so it matches whatever SCAddFighters picked for the same oldTid.
      const applyArr = (arr) => {
        const picks = arr.map(pickForTid);
        console.log(`\x1b[35m[gacha-swap] resTid [${arr.join(',')}] → [${picks.join(',')}]\x1b[0m`);
        return picks;
      };
      if (Array.isArray(d.resTid) && d.resTid.length) {
        d.resTid = applyArr(d.resTid); pkt._modified = true;
      } else if (Array.isArray(d.f2) && d.f2.length) {
        d.f2 = applyArr(d.f2); pkt._modified = true;
      } else if (typeof d.f2 === 'number') {
        const pick = pickForTid(d.f2);
        console.log(`\x1b[35m[gacha-swap] resTid(f2) ${d.f2} → ${pick}\x1b[0m`);
        d.f2 = pick; pkt._modified = true;
      }
      // Reset timesAfterLastOrange to 0 (we "pulled" top-tier)
      const gi = d.gachaInfo || d.f1;
      if (gi && typeof gi === 'object') {
        if ('timesAfterLastOrange' in gi) gi.timesAfterLastOrange = 0;
        else gi.f6 = 0; // field 6 = timesAfterLastOrange (field 5 is unused)
        pkt._modified = true;
      }
    }
    // Real SCFighterBagMsg — msgId 1003 (full roster on login / updates)
    // Instead of relying on the schema (which is out of date vs wire format), do a
    // raw-proto byte-level rewrite: parse → walk → mutate templateId → re-serialize.
    else if (pkt.msgId === 1003) {
      try {
        const newBytes = rewriteFighterBagRaw(pkt.raw);
        if (newBytes && !newBytes.equals(pkt.raw)) {
          pkt.raw = newBytes;           // intercept.js re-wraps pkt.raw into a frame
          pkt._rawBytes = newBytes;
          pkt._modified = true;
        }
      } catch (e) {
        console.log(`\x1b[31m[bag-swap] Raw rewrite failed: ${e.message}\x1b[0m`);
      }
    }
  }

  // Full JSON dump for inspection
  const data = JSON.stringify(pkt.data, null, 2);
  if (data.length <= 500) {
    console.log(`  \x1b[2m${data}\x1b[0m`);
  } else {
    console.log(`  \x1b[2m${data.substring(0, 497)}...\x1b[0m`);
  }
});

// ── Gacha swap: swallow CSHeroGacha, inject fake top-tier hero as SCAddFighters ──
// Top-tier heroes (Quality=7): [1701, 2701, 3701, 4701, 4702, 5701, 5702, 6701, 6702]
// (gacha-swap logic is handled in the tap function above — by msgId)

proxy.start();
