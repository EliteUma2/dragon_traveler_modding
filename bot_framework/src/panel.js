// Web panel for the intercepting proxy — HTTP + WebSocket server
import http from 'http';
import { readFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { WebSocketServer } from 'ws';

const __dirname = dirname(fileURLToPath(import.meta.url));

export function createPanel(proxy, opts = {}) {
  const { port = 9080 } = opts;

  // Build message list from protocol
  const messageList = [];
  for (const [name, msgId] of proxy.protocol.nameToMsgId) {
    const dir = name.startsWith('CS') ? 'C2S' : name.startsWith('SC') ? 'S2C' : null;
    messageList.push({ name, msgId, direction: dir });
  }
  messageList.sort((a, b) => a.name.localeCompare(b.name));

  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://localhost');
    const path = url.pathname;

    // CORS
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET,POST,PATCH,DELETE,OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
    if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }

    // Serve HTML
    if (req.method === 'GET' && (path === '/' || path === '/index.html')) {
      try {
        const html = readFileSync(join(__dirname, '..', 'web', 'proxy.html'), 'utf-8');
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(html);
      } catch (e) {
        res.writeHead(500); res.end('Failed to load proxy.html: ' + e.message);
      }
      return;
    }

    // API: message list
    if (req.method === 'GET' && path === '/api/messages') {
      json(res, messageList);
      return;
    }

    // API: schema for a message (with recursive sub-schemas + enums)
    if (req.method === 'GET' && path === '/api/schema') {
      const name = url.searchParams.get('name');
      const schema = proxy.protocol.getSchema(name);
      if (!schema) { json(res, { error: 'not found' }); return; }
      const subs = {};
      const enums = {};
      const collect = (s) => {
        for (const f of s.fields || []) {
          if (f.t === 'message' && f.sub && !subs[f.sub]) {
            const sub = proxy.protocol.schemas[f.sub];
            if (sub) { subs[f.sub] = sub; collect(sub); }
          }
          if (f.t === 'enum' && f.sub && proxy.protocol.enums?.[f.sub]) {
            enums[f.sub] = proxy.protocol.enums[f.sub];
          }
        }
      };
      collect(schema);
      json(res, { schema, subs, enums });
      return;
    }

    // API: list rules
    if (req.method === 'GET' && path === '/api/rules') {
      json(res, proxy.getRules());
      return;
    }

    // API: add rule
    if (req.method === 'POST' && path === '/api/rules') {
      readBody(req, (body) => {
        const rule = proxy.addRule(body);
        json(res, rule, 201);
      });
      return;
    }

    // API: toggle rule
    if (req.method === 'PATCH' && path.startsWith('/api/rules/')) {
      const id = parseInt(path.split('/').pop());
      readBody(req, (body) => {
        if (body && Object.keys(body).length > 0) {
          json(res, proxy.updateRule(id, body) || { error: 'not found' });
        } else {
          json(res, proxy.toggleRule(id) || { error: 'not found' });
        }
      });
      return;
    }

    // API: delete rule
    if (req.method === 'DELETE' && path.startsWith('/api/rules/')) {
      const id = parseInt(path.split('/').pop());
      proxy.removeRule(id);
      json(res, { ok: true });
      return;
    }

    // API: inject packet
    if (req.method === 'POST' && path === '/api/inject') {
      readBody(req, (body) => {
        try {
          proxy.inject(body.direction, body.name, body.data || {});
          json(res, { ok: true });
        } catch (e) {
          json(res, { error: e.message }, 400);
        }
      });
      return;
    }

    res.writeHead(404);
    res.end('Not found');
  });

  // WebSocket server
  const wss = new WebSocketServer({ server });

  wss.on('connection', (ws) => {
    // Send current state
    ws.send(JSON.stringify({
      type: 'init',
      session: proxy.hasSession,
      rules: proxy.getRules(),
    }));
  });

  function broadcast(obj) {
    const data = JSON.stringify(obj);
    for (const ws of wss.clients) {
      if (ws.readyState === 1) { // OPEN
        ws.send(data);
      }
    }
  }

  // Subscribe to proxy events
  proxy.on('packet', (pkt) => broadcast({ type: 'packet', ...pkt }));
  proxy.on('drop', (pkt) => broadcast({ type: 'drop', ...pkt }));
  proxy.on('session', (info) => broadcast({ type: 'session', ...info }));
  proxy.on('inject', (info) => broadcast({ type: 'inject', ...info }));

  server.listen(port, () => {
    console.log(`\x1b[36m[panel]\x1b[0m Web UI at http://localhost:${port}`);
  });

  return server;
}

// ── Helpers ─────────────────────────────────────────────────────────────────────

function json(res, data, status = 200) {
  res.writeHead(status, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(data));
}

function readBody(req, cb) {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    try { cb(JSON.parse(Buffer.concat(chunks).toString())); }
    catch { cb({}); }
  });
}
