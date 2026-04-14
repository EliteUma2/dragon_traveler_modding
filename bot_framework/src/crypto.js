// RC4 cipher — game variant: i,j reset to 0 on each process() call, S-box carries forward
export class RC4 {
  constructor(key) {
    this.state = new Uint8Array(256);
    for (let i = 0; i < 256; i++) this.state[i] = i;
    let j = 0;
    for (let i = 0; i < 256; i++) {
      j = (j + this.state[i] + key[i % key.length]) & 0xff;
      [this.state[i], this.state[j]] = [this.state[j], this.state[i]];
    }
  }

  process(data, start = 0, end) {
    const len = end ?? data.length;
    let i = 0, j = 0;
    for (let k = start; k < len; k++) {
      i = (i + 1) & 0xff;
      j = (j + this.state[i]) & 0xff;
      [this.state[i], this.state[j]] = [this.state[j], this.state[i]];
      data[k] ^= this.state[(this.state[i] + this.state[j]) & 0xff];
    }
    return data;
  }
}

// CRC32 (standard reflected polynomial 0xEDB88320)
const crc32Table = new Uint32Array(256);
for (let i = 0; i < 256; i++) {
  let crc = i;
  for (let j = 0; j < 8; j++) {
    crc = (crc & 1) ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
  }
  crc32Table[i] = crc;
}

export function crc32(data, start = 0, end) {
  const len = end ?? data.length;
  let crc = 0xffffffff;
  for (let i = start; i < len; i++) {
    crc = (crc >>> 8) ^ crc32Table[(crc ^ data[i]) & 0xff];
  }
  return (crc ^ 0xffffffff) >>> 0;
}
