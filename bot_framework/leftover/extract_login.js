import { RC4 } from './src/crypto.js';
import { readBE32 } from './src/frame.js';
import { Protocol } from './src/protocol.js';

const p = new Protocol();
const rc4Key = Buffer.from('f2298838bc25a44791cd094c44448746572b2450a3cd44dfbbe99b9b', 'hex');
const rc4 = new RC4(rc4Key);
const raw = Buffer.from('0000006f14f547dea30ab5b48925d24ad4599cb045b7468e1589c4704d6c049c49cfca5353dfe6eed4a5823b141dae339d5b74a538ed91f1af949f83834a58d0a388861a002a0a667a39993da233666aaa9881470c29987b47e74330b843ec69069a54cf3e94e42310f7fc9e2747e3d33a27a7', 'hex');
const frame = Buffer.from(raw.subarray(4));
rc4.process(frame, 4, frame.length);
const proto = frame.subarray(16);
const login = p.decode('CSLogin', proto);
console.log(JSON.stringify(login, null, 2));
