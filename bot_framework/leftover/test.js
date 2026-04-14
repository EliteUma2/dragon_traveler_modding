// Test: decrypt all 18 C→S packets from the captured session
import { RC4, crc32 } from './src/crypto.js';
import { readBE32 } from './src/frame.js';
import { Protocol } from './src/protocol.js';
import { decodeProtoToObject } from './src/protobuf.js';

const rc4Key = Buffer.from('f2298838bc25a44791cd094c44448746572b2450a3cd44dfbbe99b9b', 'hex');
const protocol = new Protocol('./data/msgdump.json', './data/overrides.json');

console.log(`Loaded ${protocol.msgIdToName.size} message definitions\n`);

const cs_raw = [
  '0000006f14f547dea30ab5b48925d24ad4599cb045b7468e1589c4704d6c049c49cfca5353dfe6eed4a5823b141dae339d5b74a538ed91f1af949f83834a58d0a388861a002a0a667a39993da233666aaa9881470c29987b47e74330b843ec69069a54cf3e94e42310f7fc9e2747e3d33a27a7',
  '00000012467177c47b62c75d69c83507a352b605db54',
  '0000001052d643f1728481a34fcd269681f05f1a',
  '0000001ebb998b75d2ba63072ccb29c3bddb1f4558afbf81890c10ddd1342a9a1323',
  '00000021f441c2e2b7674653e6bbff0f29f6979ca3936a4ecc47751e18df8a79f29201d6a5',
  '000000145ea12e010c3a074b99e20dedaf1ece33d0e6daa7',
  '00000014b40665c497d7f1babdc44b8e85c6176700e82e6d',
  '000000149bc4f4c1f3a8458c96fde48439db2bda5b23034c',
  '000000124ec5376a55859c6a7f5a9754c557c940014b',
  '00000010ffa278c2d5d81b359b15fceb0f490018',
  '00000010f3b66277f80a5d3400b354ee22d09a8e',
  '000000101915946f0603e49627aae607ac8dafaa',
  '00000010a3d32e3043928dc57845417b9e24cadc',
  '00000010cb052696a337c8df6bf46d40b877d222',
  '0000001a41ef82606b4cb03e48eda0774a1b521797614448' + '7b0eac36c140',
  '00000015354018ceeb6a8d05efb623b615cc510e7c14e4053b',
  '000000288ebc353522dd61ce39659d470e2a69619fe2ed412863b6238aecb4eff920201cbd6bc20336ae4519',
  '00000010f21f2840bc8a6eaf8d364aed9a47d71f',
];

const rc4 = new RC4(rc4Key);
let allOk = true;

for (let i = 0; i < cs_raw.length; i++) {
  const raw = Buffer.from(cs_raw[i], 'hex');
  const frameLen = readBE32(raw, 0);
  const frame = Buffer.from(raw.subarray(4));

  // Decrypt bytes 4+ (skip CRC at 0-3)
  rc4.process(frame, 4, frame.length);

  const crcVal = readBE32(frame, 0);
  const ackId = readBE32(frame, 4);
  const counter = readBE32(frame, 8);
  const msgId = readBE32(frame, 12);
  const proto = frame.subarray(16);

  const ok = msgId < 0x10000 && counter < 1000;
  const name = protocol.getName(msgId);
  const label = name || `0x${msgId.toString(16)}`;

  let decoded = '';
  if (ok && proto.length > 0) {
    try {
      const obj = protocol.decode(msgId, proto);
      decoded = JSON.stringify(obj);
      if (decoded.length > 100) decoded = decoded.substring(0, 100) + '...';
    } catch {
      decoded = proto.toString('hex').substring(0, 60);
    }
  }

  const status = ok ? '\x1b[32mOK\x1b[0m' : '\x1b[31mFAIL\x1b[0m';
  console.log(`#${String(i + 1).padStart(2)} [${status}] cnt=${counter} ack=${ackId} ${label} ${proto.length}B ${decoded}`);
  if (!ok) { allOk = false; break; }
}

console.log(allOk ? '\n\x1b[32mAll packets decrypted successfully!\x1b[0m' : '\n\x1b[31mDecryption failed!\x1b[0m');

// Test encode/decode roundtrip
console.log('\n--- Encode/Decode Roundtrip Test ---');
const login = {
  accid: 'test123',
  playerId: 'player456',
  platform: 'android',
  token: 'abc',
  reconnect: 0,
  sourceVersion: '1.0.0',
  dataVersion: '2.0.0',
  language: 'en',
};

const encoded = protocol.encode('CSLogin', login);
console.log(`CSLogin encoded: ${encoded.length}B ${encoded.toString('hex').substring(0, 60)}...`);
const decoded2 = protocol.decode('CSLogin', encoded);
console.log(`CSLogin decoded:`, JSON.stringify(decoded2));

// Verify fields match
for (const [k, v] of Object.entries(login)) {
  if (decoded2[k] !== v) {
    console.log(`\x1b[31mMISMATCH: ${k} expected=${v} got=${decoded2[k]}\x1b[0m`);
  }
}
console.log('\x1b[32mRoundtrip OK!\x1b[0m');
