// Game-tree.com login + server list flow
// Requires: node --openssl-legacy-provider (for DES-ECB)
import crypto from 'crypto';
import https from 'https';
import http from 'http';

const DEFAULTS = {
  accountOrigin: 'https://member.game-tree.com',
  appKey: '1750644421831',
  channelId: '1000000002',
  ecid: '3010081001',
  desKey: 'cyou-mrd',
  serverListUrl: 'http://usphoenix-global-gf.game-tree.com:8001/account/login?mod=web',
  clientVersion: '539',
  clientSubVersion: '689',
  sourceVersion: '69',
  dataVersion: '1.0.0.69',
};
// ── Crypto helpers ───────────────────────────────────────────────────────────

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

function md5(str) { return crypto.createHash('md5').update(str).digest('hex'); }

function desEncrypt(plaintext, keyStr) {
  const cipher = crypto.createCipheriv('des-ecb', Buffer.from(keyStr, 'utf8'), null);
  return cipher.update(plaintext, 'utf8', 'hex') + cipher.final('hex');
}

function generateDeviceId() {
  const hwid1 = crypto.randomBytes(20).toString('hex');
  const hwid2 = [
    crypto.randomBytes(4).toString('hex'),
    crypto.randomBytes(2).toString('hex'),
    crypto.randomBytes(2).toString('hex'),
    crypto.randomBytes(2).toString('hex'),
    crypto.randomBytes(6).toString('hex'),
  ].join('-');
  return 'UNITYPC\x02' + hwid1 + '\x02' + hwid2;
}

// ── Request encryption (penc.ps / penc.de) ───────────────────────────────────

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

// ── HTTP helpers ─────────────────────────────────────────────────────────────

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
        'Accept': '*/*',
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

function httpPost(url, hdrs, body) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const req = http.request({
      hostname: u.hostname, port: u.port || 80,
      path: u.pathname + u.search, method: 'POST',
      headers: hdrs,
    }, res => {
      let d = ''; res.on('data', c => d += c);
      res.on('end', () => resolve({ status: res.statusCode, headers: res.headers, body: d }));
    });
    req.on('error', reject);
    req.write(body);
    req.end();
  });
}

// ── Full login flow ──────────────────────────────────────────────────────────

const SUCCESS_CODES = [10112105, 10112107, 10112204, 10112206];

export async function gameTreeLogin(email, password, opts = {}) {
  const cfg = { ...DEFAULTS, ...opts };
  const deviceId = opts.deviceId || generateDeviceId();

  // Step 1: game-tree.com login → token + userid
  const params = {
    appkey: cfg.appKey, device: deviceId,
    account: email, password: password,
    action: 'access', tag: '1',
  };
  const { headers, body, nonce } = encryptRequest(params, cfg.appKey);
  const res = await httpsPost(`${cfg.accountOrigin}/passport/method_login.game`, headers, body);
  const parsed = JSON.parse(res.body);
  if (!parsed.ret) throw new Error('No ret field: ' + JSON.stringify(parsed));
  const dec = decryptResponse(parsed.ret, res.headers, nonce);
  if (!SUCCESS_CODES.includes(dec.info)) throw new Error(`Login failed (info=${dec.info})`);

  // Step 2: DES encrypt validateInfo
  const vi = desEncrypt(JSON.stringify({
    userip: '127.0.0.1', deviceid: deviceId, isdebug: '0',
    channel_id: cfg.channelId, opcode: '10001',
    token: dec.data.token, oid: dec.data.userid,
    sdkversion: '1.1', system: 'UNITYPC',
    gamechannel: cfg.channelId, app_key: cfg.appKey,
    region: 'US', ecid: cfg.ecid,
  }), cfg.desKey);

  // Step 3: POST to server list
  const originData = Buffer.from(JSON.stringify({
    opcode: '10001',
    data: JSON.stringify({ validateInfo: vi }),
  })).toString('base64');

  const fields = {
    deviceId, device: '', osType: 'windows',
    osVersion: 'Windows 11 (10.0.26200) 64bit',
    pushToken: '', channelAlias: cfg.channelId,
    operators: cfg.ecid + '_', language: 'en',
    originData, sdkVersion: '',
    clientVersion: cfg.clientVersion,
    clientSubVersion: cfg.clientSubVersion,
    telecomOper: '', network: 'wifi', sdkType: 'Zlong',
  };

  const slBody = Object.entries(fields)
    .map(([k, v]) => encodeURIComponent(k) + '=' + encodeURIComponent(v)).join('&');
  const slRes = await httpPost(cfg.serverListUrl, {
    'Content-Type': 'application/x-www-form-urlencoded',
    'Content-Length': Buffer.byteLength(slBody),
  }, slBody);

  const slParsed = JSON.parse(slRes.body);
  if (slParsed.errorCode !== 1000000) {
    throw new Error(`Server list error: ${slParsed.errorDesc} (${slParsed.errorCode})`);
  }

  const serverData = typeof slParsed.data === 'string' ? JSON.parse(slParsed.data) : slParsed.data;

  return {
    userId: dec.data.userid,
    token: dec.data.token,
    deviceId,
    serverData,
    config: cfg,
  };
}

// ── Helpers to extract server info from serverData ───────────────────────────

export function findServer(serverData, serverId) {
  for (const [, group] of Object.entries(serverData.groups || {})) {
    const s = group.sections?.[serverId];
    if (s) return s;
  }
  return null;
}

export function getLastPlayedRole(serverData) {
  if (!serverData.roles || serverData.roles.length === 0) return null;
  return serverData.roles.sort((a, b) => b.logoutTime - a.logoutTime)[0];
}

export function getAllServers(serverData) {
  const servers = [];
  for (const [groupName, group] of Object.entries(serverData.groups || {})) {
    for (const [sid, s] of Object.entries(group.sections || {})) {
      servers.push({ ...s, serverId: sid, groupName, groupIndex: group.index });
    }
  }
  return servers.sort((a, b) => a.groupIndex - b.groupIndex || a.index - b.index);
}

export { DEFAULTS as LOGIN_DEFAULTS, generateDeviceId };
