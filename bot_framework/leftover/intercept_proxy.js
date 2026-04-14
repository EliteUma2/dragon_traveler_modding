#!/usr/bin/env node
// Intercepting proxy PoC — drop, modify, and resequence packets
import { createProxy } from './src/intercept.js';
import { Protocol } from './src/protocol.js';

const protocol = new Protocol();

const proxy = createProxy({
  listenPort: 9003,
  targetHost: 'globalphoenix3746-ajm03f.game-tree.com',
  targetPort: 9002,
  protocol,
});

// ── Example rules ────────────────────────────────────────────────────────────

// 1. Drop specific packets entirely (both directions)
// proxy.drop('CSHeartbeat');

// 2. Intercept S→C: modify server response before it reaches the client
// proxy.intercept('SCLogin', (pkt) => {
//   // Modify any field in the decoded data
//   pkt.data.serverTime = 0;
//   return pkt;        // return pkt to forward (modified)
//   // return null;    // return null to drop
// }, 'S2C');

// 3. Intercept C→S: modify client request before it reaches the server
// proxy.intercept('CSBuyItem', (pkt) => {
//   pkt.data.count = 99;
//   return pkt;
// }, 'C2S');

// 4. Conditional drop
// proxy.intercept('SCChatMsg', (pkt) => {
//   if (pkt.data.channel === 1) return null; // drop world chat
//   return pkt;                              // forward others
// }, 'S2C');

// 5. Passive tap — log everything without modifying
proxy.tap((pkt) => {
  // Full JSON dump for inspection
  const data = JSON.stringify(pkt.data, null, 2);
  if (data.length <= 500) {
    console.log(`  \x1b[2m${data}\x1b[0m`);
  } else {
    console.log(`  \x1b[2m${data.substring(0, 497)}...\x1b[0m`);
  }
});

proxy.start();
