// Account registration — game-tree.com signup + mail.gw auto-verification
// Requires: node --openssl-legacy-provider (for AES-ECB)
import crypto from 'crypto';
import https from 'https';
import { appendFileSync } from 'fs';
import { generateDeviceId } from './login.js';

const DEFAULTS = {
  accountOrigin: 'https://member.game-tree.com',
  appKey: '1750644421831',
  channelId: '1000000002',
  mailApiBase: 'https://api.mail.gw',
};

function md5(str) { return crypto.createHash('md5').update(str).digest('hex'); }

function aesEncrypt(plaintext, keyStr) {
  const keyBuf = Buffer.from(keyStr, 'utf8');
  const algo = keyBuf.length <= 16 ? 'aes-128-ecb' : keyBuf.length <= 24 ? 'aes-192-ecb' : 'aes-256-ecb';
  const padded = Buffer.alloc(keyBuf.length <= 16 ? 16 : keyBuf.length <= 24 ? 24 : 32, 0);
  keyBuf.copy(padded);
  const cipher = crypto.createCipheriv(algo, padded, null);
  return cipher.update(plaintext, 'utf8', 'hex') + cipher.final('hex');
}

function aesDecrypt(hex, keyStr) {
  const keyBuf = Buffer.from(keyStr, 'utf8');
  const algo = keyBuf.length <= 16 ? 'aes-128-ecb' : keyBuf.length <= 24 ? 'aes-192-ecb' : 'aes-256-ecb';
  const padded = Buffer.alloc(keyBuf.length <= 16 ? 16 : keyBuf.length <= 24 ? 24 : 32, 0);
  keyBuf.copy(padded);
  const d = crypto.createDecipheriv(algo, padded, null);
  return d.update(hex, 'hex', 'utf8') + d.final('utf8');
}

function encryptRequest(params, appKey) {
  const ts = Date.now();
  let s = '';
  for (const k of Object.keys(params)) s += k + '|' + params[k] + ';';
  s += 't|' + ts;
  const nonce = ts.toString(36);
  return {
    headers: {
      'zl-p-env': '1',
      'zl-p-enc': md5(s),
      'zl-p-nonce': nonce,
      'appkey': appKey,
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    body: 'd=' + encodeURIComponent(aesEncrypt(s, nonce + nonce)),
    nonce,
  };
}

function decryptResponse(retHex, headers, nonce) {
  const key = String(Math.floor(parseInt(nonce, 36) / 1000)) + headers['zl-ret-nonce'];
  return JSON.parse(aesDecrypt(retHex, key));
}

// ── HTTP helpers ──────────────────────────────────────────────────────────────

function httpsPost(url, hdrs, body) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const req = https.request({
      hostname: u.hostname, port: 443,
      path: u.pathname + u.search, method: 'POST',
      headers: {
        ...hdrs,
        'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)',
        'Origin': 'https://www.game-tree.com',
        'Referer': 'https://www.game-tree.com/',
      },
    }, res => {
      let d = ''; res.on('data', c => d += c);
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: d }));
    });
    req.on('error', reject);
    req.write(body);
    req.end();
  });
}

function httpsJson(method, url, body, token) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const payload = body ? JSON.stringify(body) : '';
    const req = https.request({
      hostname: u.hostname, port: 443,
      path: u.pathname + u.search, method,
      headers: {
        'Content-Type': 'application/json',
        'Accept': 'application/json',
        ...(token ? { 'Authorization': `Bearer ${token}` } : {}),
        ...(payload ? { 'Content-Length': Buffer.byteLength(payload) } : {}),
      },
    }, res => {
      let d = ''; res.on('data', c => d += c);
      res.on('end', () => {
        try { resolve({ status: res.statusCode, data: JSON.parse(d) }); }
        catch { resolve({ status: res.statusCode, data: d }); }
      });
    });
    req.on('error', reject);
    if (payload) req.write(payload);
    req.end();
  });
}

// ── Rate limiter — serializes calls with minimum interval ─────────────────────

function createRateLimiter(minIntervalMs) {
  let last = 0;
  const queue = [];
  let running = false;

  async function drain() {
    if (running) return;
    running = true;
    while (queue.length > 0) {
      const elapsed = Date.now() - last;
      if (elapsed < minIntervalMs) await sleep(minIntervalMs - elapsed);
      last = Date.now();
      const { fn, resolve, reject } = queue.shift();
      try { resolve(await fn()); }
      catch (e) { reject(e); }
    }
    running = false;
  }

  return function throttle(fn) {
    return new Promise((resolve, reject) => {
      queue.push({ fn, resolve, reject });
      drain();
    });
  };
}

// Rate-limited wrapper for mail.gw requests (1 req/sec)
function mailJsonRL(limiter, method, url, body, token) {
  return limiter(() => httpsJson(method, url, body, token));
}

// ── mail.gw — disposable email ────────────────────────────────────────────────

// Cached domain (fetched once per batch)
let cachedDomain = null;
let domainExpiry = 0;

async function getMailDomain(base, limiter) {
  if (cachedDomain && Date.now() < domainExpiry) return cachedDomain;
  const res = await mailJsonRL(limiter, 'GET', `${base}/domains`);
  const domains = res.data?.['hydra:member'] || res.data;
  if (!Array.isArray(domains) || domains.length === 0) throw new Error('No mail.gw domains available');
  const active = domains.filter(d => d.isActive !== false);
  cachedDomain = active[Math.floor(Math.random() * active.length)].domain;
  domainExpiry = Date.now() + 300000; // cache 5 min
  return cachedDomain;
}

function randomUser(len = 10) {
  const chars = 'abcdefghijklmnopqrstuvwxyz0123456789';
  let s = '';
  for (let i = 0; i < len; i++) s += chars[Math.floor(Math.random() * chars.length)];
  return s;
}

function randomPassword(len = 12) {
  const chars = 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789';
  let s = '';
  for (let i = 0; i < len; i++) s += chars[Math.floor(Math.random() * chars.length)];
  return s;
}

async function createTempMail(base, limiter) {
  const domain = await getMailDomain(base, limiter);
  const address = `${randomUser()}@${domain}`;
  const mailPass = randomPassword();

  const create = await mailJsonRL(limiter, 'POST', `${base}/accounts`, { address, password: mailPass });
  if (create.status !== 201 && create.status !== 200) {
    throw new Error(`mail.gw create failed (${create.status}): ${JSON.stringify(create.data)}`);
  }

  const auth = await mailJsonRL(limiter, 'POST', `${base}/token`, { address, password: mailPass });
  if (!auth.data?.token) throw new Error('mail.gw auth failed: ' + JSON.stringify(auth.data));

  return { address, mailPass, token: auth.data.token, id: create.data.id };
}

async function waitForVerificationCode(base, token, limiter, timeoutMs = 60000, pollMs = 3000) {
  const deadline = Date.now() + timeoutMs;

  while (Date.now() < deadline) {
    const res = await mailJsonRL(limiter, 'GET', `${base}/messages`, null, token);
    const messages = res.data?.['hydra:member'] || res.data;

    if (Array.isArray(messages) && messages.length > 0) {
      const msg = messages[0];
      const full = await mailJsonRL(limiter, 'GET', `${base}/messages/${msg.id}`, null, token);
      const body = full.data?.text || full.data?.html?.join('') || full.data?.intro || '';

      // Extract 6-digit verification code — may be adjacent to text (no whitespace)
      const match = body.match(/(\d{6})/);
      if (match) return match[1];
    }

    await sleep(pollMs);
  }

  throw new Error('Timed out waiting for verification email');
}

// ── game-tree.com registration ────────────────────────────────────────────────

const SUCCESS_CODES = [10112105, 10112107, 10112204, 10112206];

async function sendVerificationCode(email, cfg) {
  const params = {
    action: 'page',
    method: 'send',
    appkey: cfg.appKey,
    account: email,
    sign: md5(md5(email)),
    annex: '',
    lang: 'en',
  };

  const { headers, body, nonce } = encryptRequest(params, cfg.appKey);
  const res = await httpsPost(`${cfg.accountOrigin}/passport/main.game`, headers, body);

  let parsed;
  try { parsed = JSON.parse(res.body); }
  catch { throw new Error('Invalid send-code response: ' + res.body.substring(0, 200)); }

  if (parsed.ret) {
    const dec = decryptResponse(parsed.ret, res.headers, nonce);
    if (dec.info === 10115105) return { ok: true, data: dec };
    if (dec.info === 10111201 || dec.info === 10115101) throw new Error('Email already registered');
    throw new Error(`Send code failed (info=${dec.info}): ${JSON.stringify(dec)}`);
  }

  if (parsed.status === 1) return { ok: true, data: parsed };
  if (parsed.info === 10111201 || parsed.info === 10115101) throw new Error('Email already registered');
  throw new Error(`Send code failed: ${JSON.stringify(parsed)}`);
}

async function registerAccount(email, password, code, deviceId, cfg) {
  const params = {
    action: 'qregister',
    account: email,
    password: password,
    code: code,
    appkey: cfg.appKey,
    device: deviceId,
    channel: cfg.channelId,
  };

  const { headers, body, nonce } = encryptRequest(params, cfg.appKey);
  const res = await httpsPost(`${cfg.accountOrigin}/passport/main.game`, headers, body);

  let parsed;
  try { parsed = JSON.parse(res.body); }
  catch { throw new Error('Invalid register response: ' + res.body.substring(0, 200)); }

  if (!parsed.ret) throw new Error('No ret field in register response');
  const dec = decryptResponse(parsed.ret, res.headers, nonce);

  if (!SUCCESS_CODES.includes(dec.info)) {
    if (dec.info === 10111205 || dec.info === 10113113) throw new Error('Invalid verification code');
    if (dec.info === 10111206 || dec.info === 10113114) throw new Error('Verification code expired');
    throw new Error(`Registration failed (info=${dec.info}): ${JSON.stringify(dec)}`);
  }

  return {
    userId: dec.data.userid,
    token: dec.data.token,
    email,
    password,
  };
}

// ── Public API ────────────────────────────────────────────────────────────────

/**
 * Create a single game account with auto email verification.
 * Accepts a shared rate limiter for parallel use; creates one if not provided.
 * Returns { email, password, userId, token }
 */
export async function createAccount(opts = {}) {
  const cfg = { ...DEFAULTS, ...opts };
  const log = opts.log || console.log;
  const tag = opts.tag || '';
  const limiter = opts._limiter || createRateLimiter(1000);
  const deviceId = generateDeviceId();

  log(`${tag}Creating temp mailbox...`);
  const mail = await createTempMail(cfg.mailApiBase, limiter);
  const email = mail.address;
  const password = opts.password || randomPassword();
  log(`${tag}Mailbox: ${email}`);

  log(`${tag}Requesting verification code...`);
  await sendVerificationCode(email, cfg);
  log(`${tag}Code sent, waiting for email...`);

  const code = await waitForVerificationCode(cfg.mailApiBase, mail.token, limiter, opts.codeTimeout || 90000);
  log(`${tag}Got code: ${code}`);

  log(`${tag}Registering account...`);
  const result = await registerAccount(email, password, code, deviceId, cfg);
  log(`${tag}Success! userId=${result.userId}`);

  return { email, password, userId: result.userId, token: result.token };
}

/**
 * Bulk-create N accounts (up to 20 concurrently), save credentials to file.
 * All mail.gw requests go through a shared rate limiter (1 req/sec).
 * Returns array of { email, password, userId } or { error }
 */
export async function createAccounts(count, opts = {}) {
  const cfg = { ...DEFAULTS, ...opts };
  const log = opts.log || console.log;
  const outputFile = opts.outputFile || 'accounts.txt';
  const serverId = opts.serverId || null;
  const concurrency = Math.min(count, opts.concurrency ?? 20);

  // Shared rate limiter — all workers share a single 1 req/sec queue to mail.gw
  const limiter = createRateLimiter(1000);

  // Pre-cache the domain so all workers use the same one
  await getMailDomain(cfg.mailApiBase, limiter);

  log(`[register] Starting ${count} accounts (${concurrency} concurrent, 1 mail req/sec)\n`);

  const results = new Array(count).fill(null);
  let nextIdx = 0;

  async function worker() {
    while (nextIdx < count) {
      const idx = nextIdx++;
      const tag = `[${idx + 1}/${count}] `;
      try {
        const acc = await createAccount({ ...cfg, log, tag, _limiter: limiter });

        const line = serverId
          ? `${acc.email}:${acc.password}:${serverId}`
          : `${acc.email}:${acc.password}`;
        appendFileSync(outputFile, line + '\n');
        log(`${tag}Saved to ${outputFile}`);

        results[idx] = acc;
      } catch (e) {
        log(`${tag}ERROR: ${e.message}`);
        results[idx] = { error: e.message };
      }
    }
  }

  // Launch workers
  const workers = [];
  for (let i = 0; i < concurrency; i++) workers.push(worker(i));
  await Promise.all(workers);

  const ok = results.filter(r => r && !r.error).length;
  log(`\n[register] Done: ${ok}/${count} accounts created`);
  return results;
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
