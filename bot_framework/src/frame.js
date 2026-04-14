import { RC4, crc32 } from './crypto.js';

// Big-endian read/write
export function readBE32(buf, off) {
  return ((buf[off] << 24) | (buf[off + 1] << 16) | (buf[off + 2] << 8) | buf[off + 3]) >>> 0;
}

export function writeBE32(buf, off, val) {
  val = val >>> 0;
  buf[off] = (val >>> 24) & 0xff;
  buf[off + 1] = (val >>> 16) & 0xff;
  buf[off + 2] = (val >>> 8) & 0xff;
  buf[off + 3] = val & 0xff;
}

// C→S frame format:
// [4B len][4B CRC32][encrypted: 4B ackId | 4B counter | 4B msgId | proto]
// RC4 encrypts from offset 8 (after len+CRC), CRC over encrypted bytes

// S→C frame format:
// [4B len][4B ackId][4B msgId][proto]  — plaintext, no RC4

export class FrameCodec {
  constructor() {
    this.rc4Send = null;
    this.rc4Recv = null;  // unused — S→C is plaintext
    this.sendCounter = 0;
    this.ackId = 0;
    this.rc4Key = null;
  }

  setKey(keyBytes) {
    this.rc4Key = Buffer.from(keyBytes);
    this.rc4Send = new RC4(Uint8Array.from(keyBytes));
  }

  // Build a C→S frame ready to send over the wire
  buildFrame(msgId, protoData) {
    const proto = Buffer.isBuffer(protoData) ? protoData : Buffer.from(protoData);
    const frameLen = 16 + proto.length;  // CRC + ackId + counter + msgId + proto
    const packet = Buffer.alloc(4 + frameLen);

    writeBE32(packet, 0, frameLen);       // length (excludes itself)
    writeBE32(packet, 4, 0);             // CRC placeholder
    writeBE32(packet, 8, this.ackId);    // last received ackId
    writeBE32(packet, 12, this.sendCounter++);
    writeBE32(packet, 16, msgId);
    proto.copy(packet, 20);

    // RC4 encrypt bytes 8+ (after len and CRC)
    if (this.rc4Send) {
      this.rc4Send.process(packet, 8, 4 + frameLen);
    }

    // CRC32 over encrypted bytes (8 to end)
    const crcVal = crc32(packet, 8, 4 + frameLen);
    writeBE32(packet, 4, crcVal);

    return packet;
  }

  // Decrypt a C→S frame (for proxy use). Returns { ackId, counter, msgId, proto }
  decryptClientFrame(frame) {
    // frame = everything after the 4-byte length prefix
    // [4B CRC][encrypted: 4B ackId | 4B counter | 4B msgId | proto]
    const data = Buffer.from(frame);
    if (this.rc4Send) {
      this.rc4Send.process(data, 4, data.length);
    }
    return {
      crc: readBE32(data, 0),
      ackId: readBE32(data, 4),
      counter: readBE32(data, 8),
      msgId: readBE32(data, 12),
      proto: data.subarray(16),
    };
  }

  // Parse an S→C frame (plaintext). Returns { ackId, msgId, proto }
  parseServerFrame(frame) {
    // frame = everything after the 4-byte length prefix
    // [4B ackId][4B msgId][proto]
    return {
      ackId: readBE32(frame, 0),
      msgId: readBE32(frame, 4),
      proto: frame.subarray(8),
    };
  }
}

// Stream parser: accumulates TCP data and yields complete frames
export class FrameReader {
  constructor() {
    this.buf = Buffer.alloc(0);
  }

  push(chunk) {
    this.buf = Buffer.concat([this.buf, chunk]);
  }

  // Returns array of { len, payload } for all complete frames
  drain() {
    const frames = [];
    while (this.buf.length >= 4) {
      const frameLen = readBE32(this.buf, 0);
      if (this.buf.length < 4 + frameLen) break;
      frames.push({
        len: frameLen,
        payload: Buffer.from(this.buf.subarray(4, 4 + frameLen)),
      });
      this.buf = Buffer.from(this.buf.subarray(4 + frameLen));
    }
    return frames;
  }
}
