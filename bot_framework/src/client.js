// Game client — async-first with per-packet handler registration
import * as net from 'net';
import { FrameCodec, FrameReader } from './frame.js';
import { Protocol } from './protocol.js';

export function createClient(protocol) {
  const codec = new FrameCodec();
  const reader = new FrameReader();
  let socket = null;
  let connected = false;
  let handshakeComplete = false;

  // Handler maps: name → [fn, fn, ...] for persistent, name → [{resolve, reject, timer}] for waiters
  const handlers = new Map();
  const waiters = new Map();
  let defaultHandler = null;

  function dispatch(pkt) {
    // Persistent handlers for this packet name
    const fns = handlers.get(pkt.name);
    if (fns) for (const fn of fns) fn(pkt);

    // One-shot waiters
    const queue = waiters.get(pkt.name);
    if (queue && queue.length > 0) {
      const w = queue.shift();
      if (w.timer) clearTimeout(w.timer);
      w.resolve(pkt);
      if (queue.length === 0) waiters.delete(pkt.name);
    }

    // Fallback
    if (!fns && !queue && defaultHandler) defaultHandler(pkt);
  }

  function handleFrame(payload) {
    if (!handshakeComplete) {
      handshakeComplete = true;
      if (payload.length >= 8) {
        const keyBytes = payload.subarray(8);
        codec.setKey(keyBytes);
        // Resolve handshake waiters
        dispatch({
          name: '_handshake',
          msgId: 0xFFFFFFFF,
          ackId: 0,
          data: { keyHex: keyBytes.toString('hex'), keyLen: keyBytes.length },
          raw: keyBytes,
        });
      }
      return;
    }

    const parsed = codec.parseServerFrame(payload);
    codec.ackId = parsed.ackId;

    const name = protocol.getName(parsed.msgId) || `unknown_${parsed.msgId}`;
    let data;
    try {
      data = protocol.decode(parsed.msgId, parsed.proto);
    } catch {
      data = { _raw: parsed.proto.toString('hex') };
    }

    dispatch({
      direction: 'S2C',
      msgId: parsed.msgId,
      name,
      ackId: parsed.ackId,
      data,
      raw: parsed.proto,
    });
  }

  const client = {
    // Register a persistent handler for a packet name
    handle(name, fn) {
      if (!handlers.has(name)) handlers.set(name, []);
      handlers.get(name).push(fn);
      return client;
    },

    // Remove a persistent handler
    off(name, fn) {
      const fns = handlers.get(name);
      if (!fns) return client;
      const i = fns.indexOf(fn);
      if (i >= 0) fns.splice(i, 1);
      if (fns.length === 0) handlers.delete(name);
      return client;
    },

    // Set a default handler for unhandled packets
    onDefault(fn) {
      defaultHandler = fn;
      return client;
    },

    // Wait for a specific packet (one-shot, returns promise)
    waitFor(name, timeoutMs = 10000) {
      return new Promise((resolve, reject) => {
        const w = { resolve, reject, timer: null };
        if (timeoutMs > 0) {
          w.timer = setTimeout(() => {
            const queue = waiters.get(name);
            if (queue) {
              const i = queue.indexOf(w);
              if (i >= 0) queue.splice(i, 1);
              if (queue.length === 0) waiters.delete(name);
            }
            reject(new Error(`waitFor('${name}') timed out after ${timeoutMs}ms`));
          }, timeoutMs);
        }
        if (!waiters.has(name)) waiters.set(name, []);
        waiters.get(name).push(w);
      });
    },

    // Connect to server, resolves when TCP connected
    connect(host, port, timeoutMs = 10000) {
      return new Promise((resolve, reject) => {
        socket = new net.Socket();
        const timer = setTimeout(() => {
          socket.destroy();
          reject(new Error('Connection timeout'));
        }, timeoutMs);

        socket.on('connect', () => {
          clearTimeout(timer);
          connected = true;
          resolve();
        });

        socket.on('data', (chunk) => {
          reader.push(chunk);
          for (const frame of reader.drain()) {
            handleFrame(frame.payload);
          }
        });

        socket.on('error', (err) => {
          clearTimeout(timer);
          if (!connected) reject(err);
          // Reject all pending waiters
          for (const [, queue] of waiters) {
            for (const w of queue) {
              if (w.timer) clearTimeout(w.timer);
              w.reject(err);
            }
          }
          waiters.clear();
        });

        socket.on('close', () => {
          connected = false;
          const err = new Error('Connection closed');
          for (const [, queue] of waiters) {
            for (const w of queue) {
              if (w.timer) clearTimeout(w.timer);
              w.reject(err);
            }
          }
          waiters.clear();
          dispatch({ name: '_disconnect', msgId: 0, ackId: 0, data: {}, raw: Buffer.alloc(0) });
        });

        socket.connect(port, host);
      });
    },

    // Send a packet (fire-and-forget)
    send(msgIdOrName, data = {}) {
      if (!connected || !handshakeComplete) {
        throw new Error('Not connected or handshake incomplete');
      }
      let msgId;
      if (typeof msgIdOrName === 'string') {
        msgId = protocol.getMsgId(msgIdOrName);
        if (!msgId) throw new Error(`Unknown message: ${msgIdOrName}`);
      } else {
        msgId = msgIdOrName;
      }
      const proto = protocol.encode(msgId, data);
      const frame = codec.buildFrame(msgId, proto);
      socket.write(frame);
    },

    // Send raw proto bytes
    sendRaw(msgId, protoBytes) {
      if (!connected || !handshakeComplete) {
        throw new Error('Not connected or handshake incomplete');
      }
      socket.write(codec.buildFrame(msgId, protoBytes));
    },

    // Send + wait for a response packet
    async sendAndWait(sendName, data, waitName, timeoutMs = 10000) {
      const pending = client.waitFor(waitName, timeoutMs);
      client.send(sendName, data);
      return pending;
    },

    close() {
      if (socket) {
        socket.destroy();
        socket = null;
      }
    },

    get connected() { return connected; },
    get handshakeComplete() { return handshakeComplete; },
    get protocol() { return protocol; },
  };

  return client;
}
