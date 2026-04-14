// Snappy decompression (for ByteString fields marked snappy in overrides)
export function snappyDecompress(input) {
  if (!input || input.length < 3) return null;
  let pos = 0;

  // Read uncompressed length (varint)
  let len = 0, shift = 0;
  while (pos < input.length && shift < 35) {
    const b = input[pos++];
    len |= (b & 0x7f) << shift;
    shift += 7;
    if ((b & 0x80) === 0) break;
  }
  len = len >>> 0;
  if (len === 0 || len > 20 * input.length || len > 10 * 1024 * 1024) return null;

  const output = Buffer.alloc(len);
  let outPos = 0;

  while (pos < input.length && outPos < len) {
    const tag = input[pos++];
    const type = tag & 3;

    if (type === 0) {
      // Literal
      const n = (tag >> 2) & 0x3f;
      let litLen;
      if (n < 60) litLen = n + 1;
      else if (n === 60) { litLen = input[pos++] + 1; }
      else if (n === 61) { litLen = (input[pos] | (input[pos + 1] << 8)) + 1; pos += 2; }
      else if (n === 62) { litLen = (input[pos] | (input[pos + 1] << 8) | (input[pos + 2] << 16)) + 1; pos += 3; }
      else { litLen = (input[pos] | (input[pos + 1] << 8) | (input[pos + 2] << 16) | ((input[pos + 3] << 24) >>> 0)) + 1; pos += 4; }

      if (pos + litLen > input.length || outPos + litLen > len) return null;
      input.copy ? input.copy(output, outPos, pos, pos + litLen) : output.set(input.subarray(pos, pos + litLen), outPos);
      pos += litLen;
      outPos += litLen;
    } else if (type === 1) {
      // Copy with 1-byte offset
      const copyLen = ((tag >> 2) & 7) + 4;
      const offset = ((tag >> 5) << 8) | input[pos++];
      if (offset === 0 || offset > outPos) return null;
      for (let i = 0; i < copyLen; i++) { output[outPos] = output[outPos - offset]; outPos++; }
    } else if (type === 2) {
      // Copy with 2-byte offset
      const copyLen = ((tag >> 2) & 0x3f) + 1;
      const offset = input[pos] | (input[pos + 1] << 8); pos += 2;
      if (offset === 0 || offset > outPos) return null;
      for (let i = 0; i < copyLen; i++) { output[outPos] = output[outPos - offset]; outPos++; }
    } else {
      // Copy with 4-byte offset
      const copyLen = ((tag >> 2) & 0x3f) + 1;
      const offset = input[pos] | (input[pos + 1] << 8) | (input[pos + 2] << 16) | ((input[pos + 3] << 24) >>> 0); pos += 4;
      if (offset === 0 || offset > outPos) return null;
      for (let i = 0; i < copyLen; i++) { output[outPos] = output[outPos - offset]; outPos++; }
    }
  }

  if (outPos !== len) return null;
  return output;
}
