#!/usr/bin/env node
// DragonTraveler Multi-Account Bot
// Usage: node --openssl-legacy-provider bot.js [accounts.txt] [--port 9999] [--proxy 9003] [--target host:port]
//        node --openssl-legacy-provider bot.js --register 5 [--server 12345] [--output accounts.txt]
import { AccountManager } from './src/manager.js';
import { createDashboard } from './src/dashboard.js';
import { createProxy } from './src/intercept.js';
import { Protocol } from './src/protocol.js';
import { createAccounts } from './src/register.js';
import { getHotReloader } from './src/hot.js';

const C = {
  reset: '\x1b[0m', bold: '\x1b[1m', dim: '\x1b[2m',
  green: '\x1b[32m', yellow: '\x1b[33m', cyan: '\x1b[36m', red: '\x1b[31m',
};

// ── Parse args ───────────────────────────────────────────────────────────────

const args = process.argv.slice(2);
let accountsFile = null;
let port = 9999;
let autoStart = false;
let proxyPort = 0;       // 0 = disabled, >0 = listen port
let targetHost = '127.0.0.1';
let targetPort = 9002;
let registerCount = 0;   // --register N
let registerServerId = null;
let registerOutput = 'accounts.txt';

for (let i = 0; i < args.length; i++) {
  if (args[i] === '--port' && args[i + 1]) { port = parseInt(args[++i]); }
  else if (args[i] === '--proxy' && args[i + 1]) { proxyPort = parseInt(args[++i]); }
  else if (args[i] === '--target' && args[i + 1]) {
    const [h, p] = args[++i].split(':');
    targetHost = h;
    targetPort = parseInt(p) || 9002;
  }
  else if (args[i] === '--register' && args[i + 1]) { registerCount = parseInt(args[++i]); }
  else if (args[i] === '--server' && args[i + 1]) { registerServerId = args[++i]; }
  else if (args[i] === '--output' && args[i + 1]) { registerOutput = args[++i]; }
  else if (args[i] === '--auto') { autoStart = true; }
  else if (!args[i].startsWith('--')) { accountsFile = args[i]; }
}

// ── Register mode ────────────────────────────────────────────────────────────

if (registerCount > 0) {
  console.log(`${C.bold}${C.cyan}DragonTraveler Account Creator${C.reset}\n`);
  console.log(`Creating ${C.yellow}${registerCount}${C.reset} accounts → ${C.dim}${registerOutput}${C.reset}\n`);

  const log = (msg) => {
    const color = msg.includes('ERROR') ? C.red
      : msg.includes('Success') ? C.green
      : msg.includes('===') ? C.yellow
      : C.dim;
    console.log(`${color}${msg}${C.reset}`);
  };

  const results = await createAccounts(registerCount, {
    serverId: registerServerId,
    outputFile: registerOutput,
    log,
  });

  const ok = results.filter(r => !r.error).length;
  console.log(`\n${C.bold}${ok}/${registerCount} accounts created${C.reset}`);
  if (ok > 0) console.log(`Credentials saved to ${C.cyan}${registerOutput}${C.reset}`);
  process.exit(0);
}

// ── Setup ────────────────────────────────────────────────────────────────────

console.log(`${C.bold}${C.cyan}DragonTraveler Multi-Account Bot${C.reset}\n`);

const protocol = new Protocol();
const manager = new AccountManager({ staggerMs: 3000, protocol });

// Console logging
manager.on('state', (e) => {
  const acc = manager.getAccount(e.account);
  const email = acc?.email || '?';
  const stateColor = e.state === 'online' ? C.green : e.state === 'error' ? C.red : C.yellow;
  console.log(`${C.dim}[${e.account}]${C.reset} ${email.split('@')[0]} ${stateColor}${e.state}${C.reset}`);
});

manager.on('log', (e) => {
  const acc = manager.getAccount(e.account);
  const isErr = e.msg.includes('ERROR');
  const color = isErr ? C.red : C.dim;
  console.log(`${C.dim}[${e.account}]${C.reset} ${color}${e.msg}${C.reset}`);
});

// Load accounts from file if provided
if (accountsFile) {
  try {
    const count = manager.loadFile(accountsFile);
    console.log(`${C.green}Loaded ${count} accounts from ${accountsFile}${C.reset}\n`);
  } catch (e) {
    console.error(`${C.red}Failed to load accounts: ${e.message}${C.reset}`);
  }
}

// Start proxy if requested
let proxy = null;
if (proxyPort > 0) {
  proxy = createProxy({
    listenPort: proxyPort,
    targetHost,
    targetPort,
    protocol,
  });
  proxy.start();
  console.log(`${C.cyan}Proxy listening on :${proxyPort} → ${targetHost}:${targetPort}${C.reset}\n`);
}

// Start dashboard (pass proxy if active)
const dashboard = createDashboard(manager, { port, proxy });

// ── Hot reload ──────────────────────────────────────────────────────────────

const hot = getHotReloader();
hot.start();

// Reload protocol when proto/data files change
hot.on('protocol', () => {
  try { protocol.reload(); } catch (e) {
    console.error(`${C.red}[hot] Protocol reload failed: ${e.message}${C.reset}`);
  }
});

// Auto-start if requested
if (autoStart && manager.accounts.size > 0) {
  console.log(`\n${C.yellow}Auto-starting all accounts...${C.reset}\n`);
  manager.startAll().catch(e => {
    console.error(`${C.red}Start failed: ${e.message}${C.reset}`);
  });
}

// Graceful shutdown
process.on('SIGINT', () => {
  console.log(`\n${C.dim}Shutting down...${C.reset}`);
  manager.stopAll();
  process.exit(0);
});

// Keep alive
process.on('uncaughtException', (e) => {
  console.error(`${C.red}Uncaught: ${e.message}${C.reset}`);
});
