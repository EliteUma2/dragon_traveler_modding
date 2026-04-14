// Intercepting proxy — decrypt, inspect, modify/drop, re-encrypt with proper resequencing
// Supports both programmatic rules (functions) and data-driven rules (for web UI)
import * as net from 'net';
import { RC4, crc32 } from './crypto.js';
import { FrameReader, readBE32, writeBE32 } from './frame.js';
import { Protocol } from './protocol.js';

export function createProxy(opts = {}) {
  const {
    listenPort = 9003,
    protocol = new Protocol(),
  } = opts;
  let targetHost = opts.targetHost || '127.0.0.1';
  let targetPort = opts.targetPort || 9002;

  // Programmatic rules
  const fnRules = { c2s: new Map(), s2c: new Map() };
  const dropSet = new Set();
  let tapFn = null;

  // Data-driven rules (for web UI)
  const dataRules = [];
  let nextRuleId = 0;

  // Event listeners
  const listeners = {};
  function emit(event, data) {
    for (const fn of listeners[event] || []) {
      try { fn(data); } catch {}
    }
  }

  // Active session (single client)
  let session = null;

  function processRules(pkt) {
    // 1. Programmatic drop set
    if (dropSet.has(pkt.name)) return null;

    // 2. Data-driven rules (in order)
    for (const rule of dataRules) {
      if (!rule.enabled) continue;
      if (rule.packet !== '*' && rule.packet !== pkt.name) continue;
      if (rule.direction && rule.direction !== pkt.direction) continue;

      if (rule.action === 'drop') return null;

      if (rule.action === 'modify' && rule.mods) {
        for (const m of rule.mods) setPath(pkt.data, m.path, m.value);
        pkt._modified = true;
      }
    }

    // 3. Programmatic function rules
    const ruleMap = pkt.direction === 'C2S' ? fnRules.c2s : fnRules.s2c;
    const fn = ruleMap.get(pkt.name);
    if (fn) {
      pkt = fn(pkt);
      if (!pkt) return null;
    }

    return pkt;
  }

  const proxy = {
    protocol,

    // ── Programmatic API ──────────────────────────────────────────────────────
    intercept(name, fn, direction) {
      if (!direction || direction === 'C2S') fnRules.c2s.set(name, fn);
      if (!direction || direction === 'S2C') fnRules.s2c.set(name, fn);
      return proxy;
    },

    drop(name) {
      dropSet.add(name);
      return proxy;
    },

    tap(fn) {
      tapFn = fn;
      return proxy;
    },

    // ── Data rule management (for web UI) ─────────────────────────────────────
    addRule(rule) {
      const r = {
        id: ++nextRuleId,
        packet: rule.packet || '*',
        direction: rule.direction || null,
        action: rule.action || 'drop',
        mods: rule.mods || [],
        enabled: rule.enabled !== false,
        label: rule.label || '',
      };
      dataRules.push(r);
      return r;
    },

    removeRule(id) {
      const i = dataRules.findIndex(r => r.id === id);
      if (i >= 0) dataRules.splice(i, 1);
    },

    toggleRule(id) {
      const r = dataRules.find(r => r.id === id);
      if (r) r.enabled = !r.enabled;
      return r;
    },

    updateRule(id, updates) {
      const r = dataRules.find(r => r.id === id);
      if (r) {
        if (updates.packet !== undefined) r.packet = updates.packet;
        if (updates.direction !== undefined) r.direction = updates.direction;
        if (updates.action !== undefined) r.action = updates.action;
        if (updates.mods !== undefined) r.mods = updates.mods;
        if (updates.enabled !== undefined) r.enabled = updates.enabled;
        if (updates.label !== undefined) r.label = updates.label;
      }
      return r;
    },

    getRules() { return dataRules.map(r => ({ ...r })); },

    // ── Events ────────────────────────────────────────────────────────────────
    on(event, fn) {
      (listeners[event] ??= []).push(fn);
      return proxy;
    },

    // ── Injection ─────────────────────────────────────────────────────────────
    inject(direction, name, data) {
      if (!session) throw new Error('No active session');

      const msgId = protocol.getMsgId(name);
      if (!msgId) throw new Error(`Unknown message: ${name}`);
      const proto = protocol.encode(name, data);

      if (direction === 'C2S') {
        if (!session.rc4Encrypt) throw new Error('Handshake not complete');
        const frame = buildC2SFrame(
          session.rc4Encrypt, session.lastServerAck,
          session.serverCounter++, msgId, proto,
        );
        session.serverSocket.write(frame);
      } else {
        const out = Buffer.alloc(4 + 4 + 4 + proto.length);
        writeBE32(out, 0, 4 + 4 + proto.length);
        writeBE32(out, 4, 0);
        writeBE32(out, 8, msgId);
        proto.copy(out, 12);
        session.clientSocket.write(out);
      }

      emit('inject', { direction, name, msgId, data });
    },

    get hasSession() { return !!session; },

    getTarget() { return { host: targetHost, port: targetPort }; },
    setTarget(host, port) {
      targetHost = host;
      targetPort = port;
      log(`Target changed to ${host}:${port} (applies to next connection)`);
    },

    // ── Start ─────────────────────────────────────────────────────────────────
    start() {
      const server = net.createServer((clientSocket) => {
        handleSession(clientSocket);
      });
      server.listen(listenPort, () => {
        log(`Proxy listening on :${listenPort} → ${targetHost}:${targetPort}`);
      });
      return server;
    },
  };

  function handleSession(clientSocket) {
    // Kill old session if still alive
    if (session) {
      log('Closing old session for reconnect');
      try { session.clientSocket.destroy(); } catch {}
      try { session.serverSocket.destroy(); } catch {}
      session = null;
    }

    const serverSocket = new net.Socket();
    const clientReader = new FrameReader();
    const serverReader = new FrameReader();
    let alive = true;
    let handshakeDone = false;

    const sess = {
      clientSocket,
      serverSocket,
      rc4Decrypt: null,
      rc4Encrypt: null,
      serverCounter: 0,
      lastServerAck: 0,
    };
    session = sess;
    emit('session', { type: 'open' });

    function teardown() {
      if (!alive) return;
      alive = false;
      try { clientSocket.destroy(); } catch {}
      try { serverSocket.destroy(); } catch {}
      if (session === sess) { session = null; emit('session', { type: 'close' }); }
      log('Session closed');
    }

    serverSocket.connect(targetPort, targetHost, () => {
      log('Connected to server');
    });

    // ── C→S path ──────────────────────────────────────────────────────────────
    clientSocket.on('data', (chunk) => {
      if (!alive) return;
      clientReader.push(chunk);
      for (const frame of clientReader.drain()) {
        if (!handshakeDone) {
          serverSocket.write(wrapFrame(frame.payload));
          continue;
        }

        const data = Buffer.from(frame.payload);
        sess.rc4Decrypt.process(data, 4, data.length);

        const msgId = readBE32(data, 12);
        const proto = Buffer.from(data.subarray(16));
        const name = protocol.getName(msgId) || `unknown_${msgId}`;

        let decoded;
        try { decoded = protocol.decode(msgId, proto); }
        catch { decoded = { _raw: proto.toString('hex') }; }

        let pkt = { direction: 'C2S', msgId, name, data: decoded, raw: proto };

        // Apply rules
        const result = processRules(pkt);
        if (!result) {
          logDrop('C2S', pkt);
          emit('drop', sanitizePkt(pkt));
          continue;
        }
        pkt = result;

        if (tapFn) tapFn(pkt);
        emit('packet', sanitizePkt(pkt));

        let outProto = proto;
        if (pkt._modified || pkt.data !== decoded) {
          try { outProto = protocol.encode(msgId, pkt.data); }
          catch { outProto = proto; }
        }

        const outFrame = buildC2SFrame(
          sess.rc4Encrypt, sess.lastServerAck,
          sess.serverCounter++, msgId, outProto,
        );
        serverSocket.write(outFrame);

        logPkt('C2S', pkt, pkt._modified || pkt.data !== decoded);
      }
    });

    // ── S→C path (passthrough by default) ─────────────────────────────────────
    serverSocket.on('data', (chunk) => {
      if (!alive) return;

      // Forward raw bytes to client immediately — before any parsing
      clientSocket.write(chunk);

      // Now decode on the side for tapping/logging
      serverReader.push(chunk);
      for (const frame of serverReader.drain()) {
        if (!handshakeDone) {
          handshakeDone = true;
          if (frame.payload.length >= 8) {
            const keyBytes = frame.payload.subarray(8);
            sess.rc4Decrypt = new RC4(Uint8Array.from(keyBytes));
            sess.rc4Encrypt = new RC4(Uint8Array.from(keyBytes));
            log(`HANDSHAKE key=${keyBytes.toString('hex').substring(0, 32)}... (${keyBytes.length}B)`);
          } else {
            log('HANDSHAKE failed — no key bytes');
            teardown();
            return;
          }
          continue;
        }

        const ackId = readBE32(frame.payload, 0);
        const msgId = readBE32(frame.payload, 4);
        const proto = Buffer.from(frame.payload.subarray(8));
        const name = protocol.getName(msgId) || `unknown_${msgId}`;

        sess.lastServerAck = ackId;

        let decoded;
        try { decoded = protocol.decode(msgId, proto); }
        catch { decoded = { _raw: proto.toString('hex') }; }

        const pkt = { direction: 'S2C', msgId, name, ackId, data: decoded, raw: proto };

        if (tapFn) tapFn(pkt);
        emit('packet', sanitizePkt(pkt));
        logPkt('S2C', pkt, false);
      }
    });

    clientSocket.on('error', (e) => { log(`Client error: ${e.message}`); teardown(); });
    serverSocket.on('error', (e) => { log(`Server error: ${e.message}`); teardown(); });
    clientSocket.on('close', () => teardown());
    serverSocket.on('close', () => teardown());
  }

  return proxy;
}

// ── Helpers ─────────────────────────────────────────────────────────────────────

function buildC2SFrame(rc4, ackId, counter, msgId, proto) {
  const frameLen = 16 + proto.length;
  const pkt = Buffer.alloc(4 + frameLen);
  writeBE32(pkt, 0, frameLen);
  writeBE32(pkt, 4, 0);
  writeBE32(pkt, 8, ackId);
  writeBE32(pkt, 12, counter);
  writeBE32(pkt, 16, msgId);
  proto.copy(pkt, 20);
  rc4.process(pkt, 8, 4 + frameLen);
  writeBE32(pkt, 4, crc32(pkt, 8, 4 + frameLen));
  return pkt;
}

function wrapFrame(payload) {
  const out = Buffer.alloc(4 + payload.length);
  writeBE32(out, 0, payload.length);
  payload.copy(out, 4);
  return out;
}

function setPath(obj, path, value) {
  const parts = path.split('.');
  let cur = obj;
  for (let i = 0; i < parts.length - 1; i++) {
    const key = parts[i];
    // Handle array indices: "items.0.count"
    const idx = /^\d+$/.test(key) ? parseInt(key) : key;
    if (cur[idx] === undefined || cur[idx] === null) cur[idx] = {};
    cur = cur[idx];
  }
  const last = parts[parts.length - 1];
  const lastIdx = /^\d+$/.test(last) ? parseInt(last) : last;
  cur[lastIdx] = value;
}

function sanitizePkt(pkt) {
  return {
    direction: pkt.direction,
    msgId: pkt.msgId,
    name: pkt.name,
    ackId: pkt.ackId,
    data: sanitizeObj(pkt.data),
    size: pkt.raw ? pkt.raw.length : 0,
    modified: !!pkt._modified,
    ts: Date.now(),
  };
}

function sanitizeObj(obj) {
  if (obj === null || obj === undefined) return obj;
  if (Buffer.isBuffer(obj) || obj instanceof Uint8Array) {
    return '0x' + Buffer.from(obj).toString('hex');
  }
  if (Array.isArray(obj)) return obj.map(sanitizeObj);
  if (typeof obj === 'object') {
    const out = {};
    for (const [k, v] of Object.entries(obj)) out[k] = sanitizeObj(v);
    return out;
  }
  return obj;
}

// ── Logging ─────────────────────────────────────────────────────────────────────

const K = {
  reset: '\x1b[0m', bold: '\x1b[1m', dim: '\x1b[2m',
  red: '\x1b[31m', green: '\x1b[32m', yellow: '\x1b[33m',
  cyan: '\x1b[36m', magenta: '\x1b[35m', strikethrough: '\x1b[9m',
};

function log(msg) {
  console.log(`${K.dim}[proxy]${K.reset} ${msg}`);
}

function logPkt(dir, pkt, modified) {
  const arrow = dir === 'C2S' ? `${K.yellow}C→S${K.reset}` : `${K.green}S→C${K.reset}`;
  const label = pkt.name.startsWith('unknown')
    ? `${K.dim}0x${pkt.msgId.toString(16)}${K.reset}`
    : `${K.cyan}${pkt.name}${K.reset}`;
  const mod = modified ? ` ${K.magenta}[MODIFIED]${K.reset}` : '';
  console.log(`${arrow} ${label}${mod} ${K.dim}${pkt.raw.length}B${K.reset}`);
}

function logDrop(dir, pkt) {
  const arrow = dir === 'C2S' ? `${K.yellow}C→S${K.reset}` : `${K.green}S→C${K.reset}`;
  console.log(`${arrow} ${K.red}${K.strikethrough}${pkt.name}${K.reset} ${K.red}[DROPPED]${K.reset}`);
}
