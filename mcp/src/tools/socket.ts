import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { z } from "zod";
import * as net from "net";

function json(data: any): { content: { type: "text"; text: string }[] } {
  return { content: [{ type: "text", text: typeof data === "string" ? data : JSON.stringify(data, null, 2) }] };
}

// ─── RC4 Cipher ──────────────────────────────────────────────────────────────

class RC4 {
  private state: Uint8Array;

  constructor(key: Uint8Array) {
    this.state = new Uint8Array(256);
    for (let i = 0; i < 256; i++) this.state[i] = i;
    let j = 0;
    for (let i = 0; i < 256; i++) {
      j = (j + this.state[i] + key[i % key.length]) & 0xff;
      [this.state[i], this.state[j]] = [this.state[j], this.state[i]];
    }
  }

  // Game resets i=0, j=0 at the start of each Encrypt call (confirmed by disassembly:
  // xor eax,eax; mov [rcx+18h],rax). Only the S-box carries forward across calls.
  // Key is the 28-byte handshake payload from the server's first message.
  process(data: Buffer, start: number = 0, end?: number): Buffer {
    const len = end ?? data.length;
    let i = 0;
    let j = 0;
    for (let k = start; k < len; k++) {
      i = (i + 1) & 0xff;
      j = (j + this.state[i]) & 0xff;
      [this.state[i], this.state[j]] = [this.state[j], this.state[i]];
      data[k] ^= this.state[(this.state[i] + this.state[j]) & 0xff];
    }
    return data;
  }
}

// ─── CRC32 ───────────────────────────────────────────────────────────────────

const CRC32_POLY = 0xedb88320;
const crc32Table: Uint32Array = (() => {
  const table = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let crc = i;
    for (let j = 0; j < 8; j++) {
      crc = (crc & 1) ? (crc >>> 1) ^ CRC32_POLY : crc >>> 1;
    }
    table[i] = crc;
  }
  return table;
})();

function computeCRC32(data: Buffer, start: number = 0, end?: number): number {
  const len = end ?? data.length;
  let crc = 0xffffffff;
  for (let i = start; i < len; i++) {
    crc = (crc >>> 8) ^ crc32Table[(crc ^ data[i]) & 0xff];
  }
  return (crc ^ 0xffffffff) >>> 0;
}

// ─── Big-endian helpers ──────────────────────────────────────────────────────

function writeBE32(buf: Buffer, value: number, offset: number) {
  buf[offset]     = (value >>> 24) & 0xff;
  buf[offset + 1] = (value >>> 16) & 0xff;
  buf[offset + 2] = (value >>> 8) & 0xff;
  buf[offset + 3] = value & 0xff;
}

function readBE32(buf: Buffer, offset: number): number {
  return ((buf[offset] << 24) | (buf[offset + 1] << 16) | (buf[offset + 2] << 8) | buf[offset + 3]) >>> 0;
}

// ─── Socket Connection Manager ──────────────────────────────────────────────

interface GameSocket {
  id: number;
  socket: net.Socket;
  rc4Send: RC4 | null;
  rc4Recv: RC4 | null;
  readKey: boolean;
  recvBuf: Buffer;
  recvQueue: ReceivedPacket[];
  sendCounter: number;
  ackId: number;
  host: string;
  port: number;
  connected: boolean;
  error: string | null;
  handshakeKey: Buffer | null;
  rc4Key: Buffer | null;
}

interface ReceivedPacket {
  ackId: number;
  msgId: number;
  data: string; // hex
  ts: number;
}

let nextSocketId = 1;
const sockets = new Map<number, GameSocket>();

function createGameSocket(host: string, port: number): Promise<GameSocket> {
  return new Promise((resolve, reject) => {
    const sock = new net.Socket();
    const gs: GameSocket = {
      id: nextSocketId++,
      socket: sock,
      rc4Send: null,
      rc4Recv: null,
      readKey: false,
      recvBuf: Buffer.alloc(0),
      recvQueue: [],
      sendCounter: 0,
      ackId: 0,
      host,
      port,
      connected: false,
      error: null,
      handshakeKey: null,
      rc4Key: null,
    };

    const timeout = setTimeout(() => {
      sock.destroy();
      reject(new Error("Connection timeout (10s)"));
    }, 10000);

    sock.on("connect", () => {
      clearTimeout(timeout);
      gs.connected = true;
      sockets.set(gs.id, gs);
      resolve(gs);
    });

    sock.on("data", (chunk: Buffer) => {
      gs.recvBuf = Buffer.concat([gs.recvBuf, chunk]);
      processRecvBuffer(gs);
    });

    sock.on("error", (err) => {
      clearTimeout(timeout);
      gs.error = err.message;
      if (!gs.connected) reject(err);
    });

    sock.on("close", () => {
      gs.connected = false;
    });

    sock.connect(port, host);
  });
}

function processRecvBuffer(gs: GameSocket) {
  while (true) {
    if (gs.recvBuf.length < 4) return;
    const frameLen = readBE32(gs.recvBuf, 0);
    if (gs.recvBuf.length < 4 + frameLen) return;

    const payload = gs.recvBuf.subarray(4, 4 + frameLen);
    gs.recvBuf = Buffer.from(gs.recvBuf.subarray(4 + frameLen));

    if (!gs.readKey) {
      // First message from server: hello/handshake
      // Format: [4B ackId=0][4B marker=0xFFFFFFFF][key bytes...]
      gs.handshakeKey = Buffer.from(payload);
      if (payload.length >= 8) {
        const keyBytes = Buffer.from(payload.subarray(8));
        gs.rc4Key = keyBytes;
        // Single RC4 instance shared for both send and recv
        // Both directions advance the same keystream
        gs.rc4Send = new RC4(Uint8Array.from(keyBytes));
      }
      gs.readKey = true;
    } else {
      // Parse message: [4B ackId][4B msgId][proto data]
      // S→C is plaintext, no RC4 involved (Encrypt only called on send)
      const data = Buffer.from(payload);
      if (data.length < 8) continue;

      const ackId = readBE32(data, 0);
      const msgId = readBE32(data, 4);
      const protoData = Buffer.from(data.subarray(8));

      gs.ackId = ackId;
      gs.recvQueue.push({
        ackId,
        msgId,
        data: protoData.toString("hex"),
        ts: Date.now(),
      });
    }
  }
}

function sendPacket(gs: GameSocket, msgId: number, protoHex: string): void {
  if (!gs.connected) throw new Error("Socket not connected");
  if (!gs.readKey) throw new Error("Handshake not complete — no hello received yet");

  const protoData = Buffer.from(protoHex, "hex");
  // Frame: [4B len][4B CRC32][4B ackId][4B counter][4B msgId][proto]
  //         0-3      4-7       8-11      12-15       16-19     20+
  // RC4 encrypts bytes 8+ (after len and CRC), CRC32 computed over encrypted bytes
  const frameLen = 20 + protoData.length;
  const frame = Buffer.alloc(frameLen);

  writeBE32(frame, frameLen - 4, 0); // len = total - 4 (excludes length field itself)
  writeBE32(frame, 0, 4);            // CRC32 placeholder
  writeBE32(frame, gs.ackId, 8);     // ackId from last recv
  writeBE32(frame, gs.sendCounter++, 12); // counter
  writeBE32(frame, msgId, 16);       // msgId
  protoData.copy(frame, 20);

  // RC4 encrypt from byte 8 onward
  if (gs.rc4Send) {
    gs.rc4Send.process(frame, 8, frameLen);
  }

  // CRC32 over encrypted bytes (8 to end)
  const crc = computeCRC32(frame, 8, frameLen);
  writeBE32(frame, crc, 4);

  gs.socket.write(frame);
}

// ─── MCP Tools ───────────────────────────────────────────────────────────────

export function registerSocketTools(server: McpServer) {
  server.tool(
    "socket_connect",
    "Open a raw TCP connection to a game server. Returns socket ID for subsequent operations. Performs RC4 key handshake automatically.",
    {
      host: z.string().describe("Server hostname or IP"),
      port: z.number().describe("Server port"),
    },
    async ({ host, port }) => {
      try {
        const gs = await createGameSocket(host, port);
        // Wait briefly for key handshake
        await new Promise(r => setTimeout(r, 500));
        return json({
          socketId: gs.id,
          connected: gs.connected,
          handshake: gs.readKey,
          rawKeyHex: gs.handshakeKey?.toString("hex") ?? null,
          rawKeyLen: gs.handshakeKey?.length ?? 0,
          rc4KeyHex: gs.rc4Key?.toString("hex") ?? null,
          rc4KeyLen: gs.rc4Key?.length ?? 0,
          pendingPackets: gs.recvQueue.length,
        });
      } catch (e: any) {
        return json({ error: e.message });
      }
    }
  );

  server.tool(
    "socket_send",
    "Send a packet through an established socket connection. Builds transport frame (CRC32 + AckId + Counter + MsgId), RC4-encrypts, and sends.",
    {
      socketId: z.number().describe("Socket ID from socket_connect"),
      msgId: z.number().describe("Message ID"),
      data: z.string().optional().describe("Hex-encoded proto payload (empty string for no payload)"),
    },
    async ({ socketId, msgId, data }) => {
      const gs = sockets.get(socketId);
      if (!gs) return json({ error: `Socket ${socketId} not found` });
      try {
        sendPacket(gs, msgId, data ?? "");
        return json({
          sent: true,
          msgId,
          dataLen: (data ?? "").length / 2,
          counter: gs.sendCounter - 1,
          ackId: gs.ackId,
        });
      } catch (e: any) {
        return json({ error: e.message });
      }
    }
  );

  server.tool(
    "socket_recv",
    "Read received packets from a socket's queue. Packets are automatically RC4-decrypted and parsed into AckId + MsgId + proto payload.",
    {
      socketId: z.number().describe("Socket ID from socket_connect"),
      wait: z.number().optional().describe("Wait up to N milliseconds for packets (default 1000)"),
      clear: z.boolean().optional().describe("Clear queue after reading (default true)"),
    },
    async ({ socketId, wait, clear }) => {
      const gs = sockets.get(socketId);
      if (!gs) return json({ error: `Socket ${socketId} not found` });

      const waitMs = wait ?? 1000;
      if (gs.recvQueue.length === 0 && waitMs > 0) {
        const deadline = Date.now() + waitMs;
        while (gs.recvQueue.length === 0 && Date.now() < deadline) {
          await new Promise(r => setTimeout(r, 50));
        }
      }

      const packets = [...gs.recvQueue];
      if (clear !== false) gs.recvQueue = [];

      return json({
        socketId,
        connected: gs.connected,
        handshake: gs.readKey,
        count: packets.length,
        packets,
      });
    }
  );

  server.tool(
    "socket_close",
    "Close a socket connection and clean up resources.",
    {
      socketId: z.number().describe("Socket ID to close"),
    },
    async ({ socketId }) => {
      const gs = sockets.get(socketId);
      if (!gs) return json({ error: `Socket ${socketId} not found` });
      gs.socket.destroy();
      sockets.delete(socketId);
      return json({ closed: true, socketId });
    }
  );

  server.tool(
    "socket_status",
    "Get status of all open sockets or a specific socket.",
    {
      socketId: z.number().optional().describe("Specific socket ID (omit for all)"),
    },
    async ({ socketId }) => {
      if (socketId !== undefined) {
        const gs = sockets.get(socketId);
        if (!gs) return json({ error: `Socket ${socketId} not found` });
        return json({
          socketId: gs.id,
          host: gs.host,
          port: gs.port,
          connected: gs.connected,
          handshake: gs.readKey,
          keyHex: gs.handshakeKey?.toString("hex") ?? null,
          sendCounter: gs.sendCounter,
          ackId: gs.ackId,
          pendingPackets: gs.recvQueue.length,
          error: gs.error,
        });
      }
      const all = [...sockets.values()].map(gs => ({
        socketId: gs.id,
        host: gs.host,
        port: gs.port,
        connected: gs.connected,
        handshake: gs.readKey,
        pendingPackets: gs.recvQueue.length,
        error: gs.error,
      }));
      return json({ sockets: all, count: all.length });
    }
  );

  server.tool(
    "socket_raw_recv",
    "Read raw bytes received (before framing/decryption). Useful for debugging the handshake or unknown protocols.",
    {
      socketId: z.number().describe("Socket ID"),
      wait: z.number().optional().describe("Wait up to N ms (default 1000)"),
    },
    async ({ socketId, wait }) => {
      const gs = sockets.get(socketId);
      if (!gs) return json({ error: `Socket ${socketId} not found` });

      const waitMs = wait ?? 1000;
      if (gs.recvBuf.length === 0 && waitMs > 0) {
        const deadline = Date.now() + waitMs;
        while (gs.recvBuf.length === 0 && Date.now() < deadline && gs.connected) {
          await new Promise(r => setTimeout(r, 50));
        }
      }

      return json({
        socketId,
        connected: gs.connected,
        bufferLen: gs.recvBuf.length,
        bufferHex: gs.recvBuf.toString("hex"),
        handshake: gs.readKey,
        keyHex: gs.handshakeKey?.toString("hex") ?? null,
      });
    }
  );
}
