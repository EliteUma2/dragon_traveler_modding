#!/usr/bin/env node
// Intercepting proxy with web UI
import { Protocol } from './src/protocol.js';
import { createProxy } from './src/intercept.js';
import { createPanel } from './src/panel.js';

const TARGET_HOST = process.argv[2] || 'globalphoenix3746-ajm03f.game-tree.com';
const TARGET_PORT = parseInt(process.argv[3]) || 9002;
const LISTEN_PORT = parseInt(process.argv[4]) || 9003;
const PANEL_PORT  = parseInt(process.argv[5]) || 9080;

const protocol = new Protocol();

const proxy = createProxy({
  listenPort: LISTEN_PORT,
  targetHost: TARGET_HOST,
  targetPort: TARGET_PORT,
  protocol,
});

createPanel(proxy, { port: PANEL_PORT });
proxy.start();

console.log(`\nGame client connects to localhost:${LISTEN_PORT}`);
console.log(`Web panel at http://localhost:${PANEL_PORT}\n`);

process.on('SIGINT', () => process.exit(0));
