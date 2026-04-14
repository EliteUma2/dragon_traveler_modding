#!/usr/bin/env node
// Login client using async handler-based API
import { Protocol } from './src/protocol.js';
import { createClient } from './src/client.js';

const HOST = 'globalphoenix3746-ajm03f.game-tree.com';
const PORT = 9002;

const C = {
  reset: '\x1b[0m', bold: '\x1b[1m', dim: '\x1b[2m',
  green: '\x1b[32m', yellow: '\x1b[33m', cyan: '\x1b[36m', red: '\x1b[31m',
};

const protocol = new Protocol();
const client = createClient(protocol);

// Log all unhandled S→C packets
client.onDefault((pkt) => {
  const label = pkt.name.startsWith('unknown')
    ? `${C.dim}0x${pkt.msgId.toString(16)}${C.reset}`
    : `${C.green}${pkt.name}${C.reset}`;
  console.log(`${C.green}S→C${C.reset} ${label} ${C.dim}ack=${pkt.ackId} ${pkt.raw.length}B${C.reset}`);
  console.log(`${C.dim}${JSON.stringify(pkt.data, null, 2)}${C.reset}`);
});

// Ctrl+C
process.on('SIGINT', () => {
  console.log(`\n${C.dim}Shutting down...${C.reset}`);
  client.close();
  process.exit(0);
});

// ── Main flow ────────────────────────────────────────────────────────────────

console.log(`${C.bold}Connecting to ${HOST}:${PORT}...${C.reset}`);
await client.connect(HOST, PORT);
console.log(`${C.green}Connected!${C.reset} Waiting for handshake...`);

// Wait for handshake
const hs = await client.waitFor('_handshake');
console.log(`${C.cyan}HANDSHAKE${C.reset} key=${C.dim}${hs.data.keyHex}${C.reset} (${hs.data.keyLen}B)`);

// Send login, wait for response
console.log(`${C.yellow}C→S${C.reset} ${C.bold}CSLogin${C.reset}`);
const loginResp = await client.sendAndWait('CSLogin', {
  accid: '8047100000000210004468224',
  playerId: '3746000e4f10',
  token: '740491392AA904C748FBCC24B398DF86BA4C80FE129ECD59A6AE1FC5CA69C988E36DC1BE03791D91348B51EC1CF8F0829722CEAC9110898B2539BB9813E2C8084EFA58DA44C1B6473ACEF9EF8865FF05F874E821E7199A2F93E1E6877106C6B7D7B8BD1911A0F40F',
  reconnect: 0,
  sourceVersion: '58',
  dataVersion: '1.0.0.58',
  guideLogin: false,
}, 'SCLogin', 15000);

console.log(`\n${C.bold}Login successful!${C.reset} ${C.dim}(${loginResp.raw.length}B)${C.reset}`);

// Ping loop
let pingCount = 0;
while (client.connected) {
  await sleep(3000);
  if (!client.connected) break;
  pingCount++;
  console.log(`${C.yellow}C→S${C.reset} ${C.bold}CSPing${C.reset} #${pingCount}`);
  await client.sendAndWait('CSPing', {}, 'SCPong', 5000);
  console.log(`     ${C.cyan}^ pong${C.reset}`);
}

function sleep(ms) {
  return new Promise(r => setTimeout(r, ms));
}
