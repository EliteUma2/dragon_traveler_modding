#!/usr/bin/env node
// Real-time decoding proxy server
// Usage: node src/proxy.js [listenPort] [targetHost] [targetPort]
//   e.g. node src/proxy.js 9003 127.0.0.1 9002

import * as net from 'net';
import { RC4 } from './crypto.js';
import { FrameReader, readBE32 } from './frame.js';
import { Protocol } from './protocol.js';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

const LISTEN_PORT = parseInt(process.argv[2]) || 9003;
const TARGET_HOST = process.argv[3] || '127.0.0.1';
const TARGET_PORT = parseInt(process.argv[4]) || 9002;

// Color codes
const C = {
  reset: '\x1b[0m',
  dim: '\x1b[2m',
  bold: '\x1b[1m',
  red: '\x1b[31m',
  green: '\x1b[32m',
  yellow: '\x1b[33m',
  blue: '\x1b[34m',
  magenta: '\x1b[35m',
  cyan: '\x1b[36m',
  white: '\x1b[37m',
  bgRed: '\x1b[41m',
  bgGreen: '\x1b[42m',
  bgBlue: '\x1b[44m',
};

const protocol = new Protocol(
  join(__dirname, '..', 'data', 'msgdump.json'),
  join(__dirname, '..', 'data', 'overrides.json'),
);

console.log(`${C.bold}Game Protocol Proxy${C.reset}`);
console.log(`${C.dim}Loaded ${protocol.msgIdToName.size} message definitions, ${Object.keys(protocol.schemas).length} schemas${C.reset}`);
console.log(`${C.dim}Listening on :${LISTEN_PORT} → ${TARGET_HOST}:${TARGET_PORT}${C.reset}\n`);

let sessionId = 0;

const server = net.createServer((clientSocket) => {
  const sid = ++sessionId;
  const tag = `${C.dim}[S${sid}]${C.reset}`;
  console.log(`${tag} ${C.green}Client connected${C.reset} from ${clientSocket.remoteAddress}`);

  const serverSocket = new net.Socket();

  const clientReader = new FrameReader();
  const serverReader = new FrameReader();
  let rc4 = null;
  let handshakeDone = false;
  let packetNum = 0;

  serverSocket.connect(TARGET_PORT, TARGET_HOST, () => {
    console.log(`${tag} ${C.blue}Connected to server${C.reset} ${TARGET_HOST}:${TARGET_PORT}`);
  });

  // Client → Server
  clientSocket.on('data', (chunk) => {
    serverSocket.write(chunk);  // forward immediately

    clientReader.push(chunk);
    for (const frame of clientReader.drain()) {
      packetNum++;
      const num = String(packetNum).padStart(4, ' ');

      if (!handshakeDone) {
        // Client's first data before handshake — shouldn't happen normally
        console.log(`${tag} ${C.yellow}C→S${C.reset} #${num} ${C.dim}(pre-handshake, ${frame.len}B)${C.reset}`);
        continue;
      }

      // Decrypt C→S frame
      // frame.payload = [4B CRC][encrypted: 4B ackId | 4B counter | 4B msgId | proto]
      const data = Buffer.from(frame.payload);
      if (rc4) {
        rc4.process(data, 4, data.length);
      }

      const crc = readBE32(data, 0);
      const ackId = readBE32(data, 4);
      const counter = readBE32(data, 8);
      const msgId = readBE32(data, 12);
      const proto = data.subarray(16);

      const name = protocol.getName(msgId);
      let decoded;
      try {
        decoded = protocol.decode(msgId, proto);
      } catch {
        decoded = null;
      }

      const label = name
        ? `${C.cyan}${name}${C.reset}`
        : `${C.yellow}0x${msgId.toString(16)}${C.reset}`;

      console.log(`${tag} ${C.bold}${C.yellow}C→S${C.reset} #${num} ${label} ${C.dim}cnt=${counter} ack=${ackId} ${proto.length}B${C.reset}`);
      if (decoded) {
        printObject(decoded, '     ');
      } else if (proto.length > 0) {
        console.log(`     ${C.dim}${proto.toString('hex').substring(0, 120)}${proto.length > 60 ? '...' : ''}${C.reset}`);
      }
    }
  });

  // Server → Client
  serverSocket.on('data', (chunk) => {
    clientSocket.write(chunk);  // forward immediately

    serverReader.push(chunk);
    for (const frame of serverReader.drain()) {
      packetNum++;
      const num = String(packetNum).padStart(4, ' ');

      if (!handshakeDone) {
        // First S→C message: handshake
        handshakeDone = true;
        if (frame.payload.length >= 8) {
          const keyBytes = frame.payload.subarray(8);
          rc4 = new RC4(Uint8Array.from(keyBytes));
          console.log(`${tag} ${C.magenta}HANDSHAKE${C.reset} key=${C.dim}${keyBytes.toString('hex')}${C.reset} (${keyBytes.length}B)`);
        } else {
          console.log(`${tag} ${C.red}HANDSHAKE${C.reset} ${C.dim}(no key, ${frame.payload.length}B)${C.reset}`);
        }
        continue;
      }

      // S→C is plaintext: [4B ackId][4B msgId][proto]
      const ackId = readBE32(frame.payload, 0);
      const msgId = readBE32(frame.payload, 4);
      const proto = frame.payload.subarray(8);

      const name = protocol.getName(msgId);
      let decoded;
      try {
        decoded = protocol.decode(msgId, proto);
      } catch {
        decoded = null;
      }

      const label = name
        ? `${C.green}${name}${C.reset}`
        : `${C.yellow}0x${msgId.toString(16)}${C.reset}`;

      console.log(`${tag} ${C.bold}${C.green}S→C${C.reset} #${num} ${label} ${C.dim}ack=${ackId} ${proto.length}B${C.reset}`);
      if (decoded) {
        printObject(decoded, '     ');
      } else if (proto.length > 0) {
        console.log(`     ${C.dim}${proto.toString('hex').substring(0, 120)}${proto.length > 60 ? '...' : ''}${C.reset}`);
      }
    }
  });

  clientSocket.on('error', (err) => {
    console.log(`${tag} ${C.red}Client error:${C.reset} ${err.message}`);
  });

  serverSocket.on('error', (err) => {
    console.log(`${tag} ${C.red}Server error:${C.reset} ${err.message}`);
  });

  clientSocket.on('close', () => {
    console.log(`${tag} ${C.dim}Client disconnected${C.reset}`);
    serverSocket.destroy();
  });

  serverSocket.on('close', () => {
    console.log(`${tag} ${C.dim}Server disconnected${C.reset}`);
    clientSocket.destroy();
  });
});

server.listen(LISTEN_PORT, () => {
  console.log(`${C.green}Proxy listening on port ${LISTEN_PORT}${C.reset}\n`);
});

// Pretty-print decoded proto object
function printObject(obj, indent = '') {
  const entries = Object.entries(obj);
  if (entries.length === 0) return;

  // Compact single-level objects
  const simple = entries.every(([, v]) => typeof v !== 'object' || v === null || Buffer.isBuffer(v));
  if (simple && entries.length <= 6) {
    const parts = entries.map(([k, v]) => {
      if (typeof v === 'string' && v.length > 60) return `${C.cyan}${k}${C.reset}="${v.substring(0, 57)}..."`;
      if (typeof v === 'string') return `${C.cyan}${k}${C.reset}="${v}"`;
      if (typeof v === 'boolean') return `${C.cyan}${k}${C.reset}=${v ? `${C.green}true${C.reset}` : `${C.red}false${C.reset}`}`;
      return `${C.cyan}${k}${C.reset}=${v}`;
    });
    console.log(`${indent}${parts.join(' ')}`);
    return;
  }

  for (const [key, value] of entries) {
    if (value === null || value === undefined) continue;

    if (Array.isArray(value)) {
      if (value.length === 0) continue;
      console.log(`${indent}${C.cyan}${key}${C.reset}[] (${value.length}):`);
      for (let i = 0; i < Math.min(value.length, 10); i++) {
        if (typeof value[i] === 'object' && value[i] !== null && !Buffer.isBuffer(value[i])) {
          console.log(`${indent}  [${i}]:`);
          printObject(value[i], indent + '    ');
        } else {
          const display = formatValue(value[i]);
          console.log(`${indent}  [${i}] ${display}`);
        }
      }
      if (value.length > 10) {
        console.log(`${indent}  ${C.dim}... and ${value.length - 10} more${C.reset}`);
      }
    } else if (typeof value === 'object' && !Buffer.isBuffer(value)) {
      console.log(`${indent}${C.cyan}${key}${C.reset}:`);
      printObject(value, indent + '  ');
    } else {
      console.log(`${indent}${C.cyan}${key}${C.reset}=${formatValue(value)}`);
    }
  }
}

function formatValue(v) {
  if (typeof v === 'string' && v.length > 80) return `"${v.substring(0, 77)}..."`;
  if (typeof v === 'string') return `"${v}"`;
  if (typeof v === 'boolean') return v ? `${C.green}true${C.reset}` : `${C.red}false${C.reset}`;
  if (Buffer.isBuffer(v)) return `${C.dim}${v.toString('hex').substring(0, 40)}${v.length > 20 ? '...' : ''} (${v.length}B)${C.reset}`;
  return String(v);
}
