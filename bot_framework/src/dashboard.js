// Web dashboard — HTTP REST + WebSocket for real-time account monitoring + proxy packet view
import http from 'http';
import { readFileSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';
import { WebSocketServer } from 'ws';
import { createAccount, createAccounts } from './register.js';
import { getHotReloader } from './hot.js';
import { gameTreeLogin, getAllServers, findServer } from './login.js';
import { getGameDB } from './gamedb.js';

const __dirname = dirname(fileURLToPath(import.meta.url));

export function createDashboard(manager, opts = {}) {
  const { port = 9999, proxy = null } = opts;

  // Proxy packet ring buffer (keep last N for new WS clients)
  const pktRing = [];
  const PKT_RING_MAX = 500;

  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://localhost');
    const path = url.pathname;

    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET,POST,DELETE,OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
    if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }

    // ── Serve HTML ───────────────────────────────────────────────────────────
    if (req.method === 'GET' && (path === '/' || path === '/index.html')) {
      try {
        const html = readFileSync(join(__dirname, '..', 'web', 'dashboard.html'), 'utf-8');
        res.writeHead(200, { 'Content-Type': 'text/html' });
        res.end(html);
      } catch (e) {
        res.writeHead(500); res.end('Failed to load dashboard.html: ' + e.message);
      }
      return;
    }

    // ── REST API ─────────────────────────────────────────────────────────────

    // GET /api/status — all accounts status + proxy info
    if (req.method === 'GET' && path === '/api/status') {
      const status = manager.getStatus();
      status.proxy = !!proxy;
      status.proxySession = proxy?.hasSession ?? false;
      json(res, status);
      return;
    }

    // GET /api/packets — recent proxy packets
    if (req.method === 'GET' && path === '/api/packets') {
      const since = parseInt(url.searchParams.get('since') || '0');
      const limit = Math.min(parseInt(url.searchParams.get('limit') || '200'), 500);
      const filtered = since > 0 ? pktRing.filter(p => p.seq > since) : pktRing;
      json(res, filtered.slice(-limit));
      return;
    }

    // GET /api/accounts/:id/logs — account logs
    if (req.method === 'GET' && path.match(/^\/api\/accounts\/(\d+)\/logs$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      const limit = parseInt(url.searchParams.get('limit') || '100');
      json(res, manager.getLogs(id, limit));
      return;
    }

    // POST /api/accounts — add account { email, password, serverId? }
    if (req.method === 'POST' && path === '/api/accounts') {
      readBody(req, (body) => {
        if (!body.email || !body.password) {
          json(res, { error: 'email and password required' }, 400);
          return;
        }
        const acc = manager.addAccount(body.email, body.password, {
          serverId: body.serverId || null,
        });
        json(res, acc.toJSON(), 201);
      });
      return;
    }

    // DELETE /api/accounts/:id — remove account
    if (req.method === 'DELETE' && path.match(/^\/api\/accounts\/(\d+)$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      if (manager.removeAccount(id)) json(res, { ok: true });
      else json(res, { error: 'not found' }, 404);
      return;
    }

    // POST /api/accounts/:id/start — start one account
    if (req.method === 'POST' && path.match(/^\/api\/accounts\/(\d+)\/start$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      try {
        manager.startAccount(id).catch(() => {});
        json(res, { ok: true });
      } catch (e) { json(res, { error: e.message }, 400); }
      return;
    }

    // POST /api/accounts/:id/stop — stop one account
    if (req.method === 'POST' && path.match(/^\/api\/accounts\/(\d+)\/stop$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      try { manager.stopAccount(id); json(res, { ok: true }); }
      catch (e) { json(res, { error: e.message }, 400); }
      return;
    }

    // POST /api/accounts/:id/start-bot — start account + auto-play immediately
    if (req.method === 'POST' && path.match(/^\/api\/accounts\/(\d+)\/start-bot$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      try {
        const acc = manager.getAccount(id);
        if (!acc) { json(res, { error: 'not found' }, 404); return; }
        // Start connection, then auto-play once online
        manager.startAccount(id).then(() => {
          acc.startAutoPlay({});
        }).catch(() => {});
        json(res, { ok: true });
      } catch (e) { json(res, { error: e.message }, 400); }
      return;
    }

    // POST /api/start-all
    if (req.method === 'POST' && path === '/api/start-all') {
      manager.startAll().catch(() => {});
      json(res, { ok: true });
      return;
    }

    // POST /api/stop-all
    if (req.method === 'POST' && path === '/api/stop-all') {
      manager.stopAll();
      json(res, { ok: true });
      return;
    }

    // POST /api/send — send packet to one account { id, name, data }
    if (req.method === 'POST' && path === '/api/send') {
      readBody(req, (body) => {
        try {
          manager.sendTo(body.id, body.name, body.data || {});
          json(res, { ok: true });
        } catch (e) { json(res, { error: e.message }, 400); }
      });
      return;
    }

    // POST /api/send-all — send packet to all { name, data }
    if (req.method === 'POST' && path === '/api/send-all') {
      readBody(req, (body) => {
        try {
          manager.sendToAll(body.name, body.data || {});
          json(res, { ok: true });
        } catch (e) { json(res, { error: e.message }, 400); }
      });
      return;
    }

    // POST /api/claim-daily-all — claim every daily task for all online accounts
    if (req.method === 'POST' && path === '/api/claim-daily-all') {
      import('./claims.js').then(async ({ claimDailyAll }) => {
        try {
          const results = await claimDailyAll(manager);
          json(res, { ok: true, results });
        } catch (e) { json(res, { error: e.message }, 500); }
      });
      return;
    }

    // POST /api/accounts/:id/claim-daily — claim daily tasks for one account
    if (req.method === 'POST' && path.match(/^\/api\/accounts\/(\d+)\/claim-daily$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      const acc = manager.getAccount(id);
      if (!acc) { json(res, { error: 'not found' }, 404); return; }
      import('./claims.js').then(async ({ claimDailyTasks }) => {
        try {
          const result = await claimDailyTasks(acc);
          json(res, { ok: true, ...result });
        } catch (e) { json(res, { error: e.message }, 500); }
      });
      return;
    }

    // GET /api/messages — message list for autocomplete/datalist
    if (req.method === 'GET' && path === '/api/messages') {
      const protocol = manager.protocol;
      const list = [];
      for (const [name, msgId] of protocol.nameToMsgId) {
        const dir = name.startsWith('CS') ? 'C2S' : name.startsWith('SC') ? 'S2C' : null;
        list.push({ name, msgId, direction: dir });
      }
      list.sort((a, b) => a.name.localeCompare(b.name));
      json(res, list);
      return;
    }

    // GET /api/schema?name=X — schema + sub-schemas + enums for form builder
    if (req.method === 'GET' && path === '/api/schema') {
      const name = url.searchParams.get('name');
      const protocol = manager.protocol;
      const schema = protocol.getSchema(name);
      if (!schema) { json(res, { error: 'not found' }); return; }
      const subs = {};
      const enums = {};
      const collect = (s) => {
        for (const f of s.fields || []) {
          if (f.t === 'message' && f.sub && !subs[f.sub]) {
            const sub = protocol.schemas[f.sub];
            if (sub) { subs[f.sub] = sub; collect(sub); }
          }
          if (f.t === 'enum' && f.sub && protocol.enums?.[f.sub]) {
            enums[f.sub] = protocol.enums[f.sub];
          }
        }
      };
      collect(schema);
      json(res, { schema, subs, enums });
      return;
    }

    // POST /api/load-file — load accounts from file { path }
    if (req.method === 'POST' && path === '/api/load-file') {
      readBody(req, (body) => {
        try {
          const count = manager.loadFile(body.path);
          json(res, { loaded: count });
        } catch (e) { json(res, { error: e.message }, 400); }
      });
      return;
    }

    // POST /api/register — create account(s) { count?, serverId?, password? }
    if (req.method === 'POST' && path === '/api/register') {
      readBody(req, (body) => {
        const count = Math.min(parseInt(body.count) || 1, 50);
        const logLines = [];
        const log = (msg) => {
          logLines.push(msg);
          broadcast({ type: 'register_log', msg });
        };

        (count === 1
          ? createAccount({ serverId: body.serverId, password: body.password, log })
              .then(r => [r])
          : createAccounts(count, {
              serverId: body.serverId,
              outputFile: body.outputFile || 'accounts.txt',
              log,
            })
        ).then((results) => {
          // Auto-add successful registrations to the manager
          for (const r of results) {
            if (!r.error && r.email && r.password) {
              manager.addAccount(r.email, r.password, { serverId: body.serverId || null });
            }
          }
          json(res, { results, logs: logLines });
        }).catch((e) => {
          json(res, { error: e.message, logs: logLines }, 500);
        });
      });
      return;
    }

    // POST /api/accounts/:id/main-story — do one main story dungeon battle
    if (req.method === 'POST' && path.match(/^\/api\/accounts\/(\d+)\/main-story$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      const acc = manager.getAccount(id);
      if (!acc) { json(res, { error: 'not found' }, 404); return; }
      acc.doMainStory().then(result => {
        json(res, result);
      }).catch(e => {
        json(res, { error: e.message }, 400);
      });
      return;
    }

    // POST /api/accounts/:id/autoplay — start/stop/configure autoplay
    // { action: "start"|"stop"|"configure", settings?: {...} }
    if (req.method === 'POST' && path.match(/^\/api\/accounts\/(\d+)\/autoplay$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      readBody(req, (body) => {
        const acc = manager.getAccount(id);
        if (!acc) { json(res, { error: 'not found' }, 404); return; }
        try {
          const action = body.action || 'start';
          if (action === 'start') {
            acc.startAutoPlay(body.settings || {});
            json(res, { ok: true, autoPlay: acc.autoPlaySettings });
          } else if (action === 'stop') {
            acc.stopAutoPlay();
            json(res, { ok: true });
          } else if (action === 'configure') {
            Object.assign(acc.autoPlaySettings, body.settings || {});
            acc.emit('state', { account: acc.id, state: acc.state, prev: acc.state });
            json(res, { ok: true, autoPlay: acc.autoPlaySettings });
          } else {
            json(res, { error: 'Unknown action. Use start, stop, or configure' }, 400);
          }
        } catch (e) { json(res, { error: e.message }, 400); }
      });
      return;
    }

    // POST /api/accounts/:id/mine — start auto-mining { activityId?, delayMs?, maxClicks? }
    if (req.method === 'POST' && path.match(/^\/api\/accounts\/(\d+)\/mine$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      readBody(req, async (body) => {
        const acc = manager.getAccount(id);
        if (!acc) { json(res, { error: 'not found' }, 404); return; }
        try {
          const { autoMine } = await import('./mining.js');
          json(res, { ok: true, started: true });
          autoMine(acc, body).catch(e => acc._log(`[mining] ${e.message}`));
        } catch (e) { json(res, { error: e.message }, 400); }
      });
      return;
    }

    // DELETE /api/accounts/:id/mine — stop mining
    if (req.method === 'DELETE' && path.match(/^\/api\/accounts\/(\d+)\/mine$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      const acc = manager.getAccount(id);
      if (!acc) { json(res, { error: 'not found' }, 404); return; }
      import('./mining.js').then(({ stopMining }) => {
        stopMining(acc);
        json(res, { ok: true });
      }).catch(e => json(res, { error: e.message }, 500));
      return;
    }

    // GET /api/mining/map — get shared mining map stats + columns
    if (req.method === 'GET' && path === '/api/mining/map') {
      import('./mining_map.js').then(({ getMapStats, getMapColumns }) => {
        const stats = getMapStats();
        const from = parseInt(url.searchParams.get('from') || stats.xRange[0]);
        const to = parseInt(url.searchParams.get('to') || stats.xRange[1]);
        const columns = getMapColumns(from, Math.min(to, from + 200));
        json(res, { stats, columns });
      }).catch(e => json(res, { error: e.message }, 500));
      return;
    }

    // GET /api/mining/path — compute optimal path from startX to targetX
    if (req.method === 'GET' && path === '/api/mining/path') {
      import('./mining_map.js').then(({ computeOptimalPath }) => {
        const startX = parseInt(url.searchParams.get('from') || '0');
        const targetX = parseInt(url.searchParams.get('to') || '100');
        const result = computeOptimalPath(startX, targetX);
        json(res, result);
      }).catch(e => json(res, { error: e.message }, 500));
      return;
    }

    // GET /api/accounts/:id/heroes — hero roster + equipment + items + formation + enemies
    if (req.method === 'GET' && path.match(/^\/api\/accounts\/(\d+)\/heroes$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      const acc = manager.getAccount(id);
      if (!acc) { json(res, { error: 'not found' }, 404); return; }
      const gdb = getGameDB();
      // Enrich heroes with names and Post
      const heroes = (acc._heroRoster || []).map(h => {
        const post = gdb.heroPost(h.heroId);
        return { ...h, name: gdb.heroName(h.heroId), post: post?.Post || 0, quality: post?.Quality || 0 };
      });
      // Get next dungeon enemy info
      const nextDungeon = acc._getNextDungeonId();
      const enemies = nextDungeon ? gdb.dungeonEnemies(nextDungeon) : null;
      const dungeonMeta = nextDungeon ? gdb.dungeonInfo(nextDungeon) : null;
      // Level-up affordability per hero
      const levelCosts = heroes.map(h => {
        const fromLv = h.level || 1;
        const maxLv = acc.playerLevel || 10;
        if (fromLv >= maxLv) return { heroId: h.heroId, atMax: true };
        const maxAfford = gdb.maxAffordableLevel(acc._currencies, fromLv, maxLv);
        const nextCost = gdb.levelUpCost(fromLv, fromLv + 1);
        return { heroId: h.heroId, fromLv, maxLv, maxAfford, nextCost };
      });
      // Get current formation positions from SCFighterBagMsg (stored as lineup)
      json(res, {
        heroes,
        teamLineup: acc._teamLineup,
        equipBag: acc._equipBag,
        items: acc._items,
        currencies: acc._currencies,
        playerLevel: acc.playerLevel,
        enemies,
        dungeonMeta,
        levelCosts,
      });
      return;
    }

    // GET /api/accounts/:id/servers — fetch server list for account
    if (req.method === 'GET' && path.match(/^\/api\/accounts\/(\d+)\/servers$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      const acc = manager.getAccount(id);
      if (!acc) { json(res, { error: 'not found' }, 404); return; }
      (async () => {
        try {
          const loginData = acc.loginData || await gameTreeLogin(acc.email, acc.password, {
            deviceId: acc.loginData?.deviceId,
          });
          const servers = getAllServers(loginData.serverData);
          const roles = loginData.serverData.roles || [];
          json(res, {
            current: acc.serverId,
            servers: servers.map(s => ({
              serverId: s.serverId,
              name: s.name,
              groupName: s.groupName,
              hasRole: roles.some(r => r.serverId === s.serverId),
            })),
          });
        } catch (e) { json(res, { error: e.message }, 500); }
      })();
      return;
    }

    // POST /api/accounts/:id/server — change server { serverId }
    if (req.method === 'POST' && path.match(/^\/api\/accounts\/(\d+)\/server$/)) {
      const id = parseInt(path.match(/(\d+)/)[1]);
      readBody(req, (body) => {
        try {
          const acc = manager.getAccount(id);
          if (!acc) { json(res, { error: 'not found' }, 404); return; }
          if (acc.state === 'online' || acc.state === 'connecting' || acc.state === 'logging_in') {
            acc.stop();
          }
          acc.serverId = body.serverId;
          acc.serverInfo = null;
          acc.roleInfo = null;
          acc._log(`Server changed to: ${body.serverId}`);
          acc.emit('state', { account: acc.id, state: acc.state, prev: acc.state });
          json(res, { ok: true });
        } catch (e) { json(res, { error: e.message }, 400); }
      });
      return;
    }

    // POST /api/proxy/inject — inject packet through proxy { direction, name, data }
    if (req.method === 'POST' && path === '/api/proxy/inject') {
      if (!proxy) { json(res, { error: 'Proxy not running' }, 400); return; }
      readBody(req, (body) => {
        try {
          proxy.inject(body.direction, body.name, body.data || {});
          json(res, { ok: true });
        } catch (e) { json(res, { error: e.message }, 400); }
      });
      return;
    }

    // GET /api/proxy/rules
    if (req.method === 'GET' && path === '/api/proxy/rules') {
      if (!proxy) { json(res, []); return; }
      json(res, proxy.getRules());
      return;
    }

    // POST /api/proxy/rules — add rule
    if (req.method === 'POST' && path === '/api/proxy/rules') {
      if (!proxy) { json(res, { error: 'Proxy not running' }, 400); return; }
      readBody(req, (body) => {
        json(res, proxy.addRule(body), 201);
      });
      return;
    }

    // DELETE /api/proxy/rules/:id
    if (req.method === 'DELETE' && path.match(/^\/api\/proxy\/rules\/(\d+)$/)) {
      if (!proxy) { json(res, { error: 'Proxy not running' }, 400); return; }
      const id = parseInt(path.match(/(\d+)/)[1]);
      proxy.removeRule(id);
      json(res, { ok: true });
      return;
    }

    // GET /api/proxy/target — get current proxy target
    if (req.method === 'GET' && path === '/api/proxy/target') {
      if (!proxy) { json(res, { error: 'Proxy not running' }, 400); return; }
      json(res, proxy.getTarget());
      return;
    }

    // POST /api/proxy/target — change proxy target { host, port }
    if (req.method === 'POST' && path === '/api/proxy/target') {
      if (!proxy) { json(res, { error: 'Proxy not running' }, 400); return; }
      readBody(req, (body) => {
        if (!body.host || !body.port) { json(res, { error: 'host and port required' }, 400); return; }
        proxy.setTarget(body.host, parseInt(body.port));
        json(res, { ok: true, host: body.host, port: parseInt(body.port) });
      });
      return;
    }

    // POST /api/proxy/target-server — resolve server ID to ip:port via account login data { accountId, serverId }
    if (req.method === 'POST' && path === '/api/proxy/target-server') {
      if (!proxy) { json(res, { error: 'Proxy not running' }, 400); return; }
      readBody(req, async (body) => {
        try {
          const acc = manager.getAccount(body.accountId);
          if (!acc) { json(res, { error: 'Account not found' }, 404); return; }
          const loginData = acc.loginData || await gameTreeLogin(acc.email, acc.password, {
            deviceId: acc.loginData?.deviceId,
          });
          const server = findServer(loginData.serverData, body.serverId);
          if (!server) { json(res, { error: `Server ${body.serverId} not found` }, 404); return; }
          proxy.setTarget(server.ip, server.port);
          json(res, { ok: true, host: server.ip, port: server.port, name: server.name });
        } catch (e) { json(res, { error: e.message }, 500); }
      });
      return;
    }

    // ── GameDB API ─────────────────────────────────────────────────────────

    // GET /api/gamedb/resolve — resolve IDs to names
    // ?heroes=4301,2301&items=1015,1000&equips=210000&currencies=1,3,5
    if (req.method === 'GET' && path === '/api/gamedb/resolve') {
      const gdb = getGameDB();
      const result = {};
      const parse = (s) => s ? s.split(',').map(Number).filter(n => n > 0) : [];
      const heroIds = parse(url.searchParams.get('heroes'));
      const itemIds = parse(url.searchParams.get('items'));
      const equipIds = parse(url.searchParams.get('equips'));
      const currencyIds = parse(url.searchParams.get('currencies'));
      if (heroIds.length) result.heroes = gdb.resolveHeroes(heroIds);
      if (itemIds.length) result.items = gdb.resolveItems(itemIds);
      if (equipIds.length) result.equips = gdb.resolveEquipments(equipIds);
      if (currencyIds.length) result.currencies = gdb.resolveItems(currencyIds);
      json(res, result);
      return;
    }

    // GET /api/gamedb/hero/:id
    if (req.method === 'GET' && path.startsWith('/api/gamedb/hero/')) {
      const id = parseInt(path.split('/').pop());
      json(res, getGameDB().heroInfo(id) || { error: 'not found' });
      return;
    }

    // GET /api/gamedb/item/:id
    if (req.method === 'GET' && path.startsWith('/api/gamedb/item/')) {
      const id = parseInt(path.split('/').pop());
      json(res, getGameDB().itemInfo(id) || { error: 'not found' });
      return;
    }

    // GET /api/gamedb/tables?q=pattern
    if (req.method === 'GET' && path === '/api/gamedb/tables') {
      const q = url.searchParams.get('q') || '';
      const tables = q ? getGameDB().searchTables(q) : getGameDB().listTables();
      json(res, tables);
      return;
    }

    // GET /api/gamedb/query?table=X&sql=SELECT...&limit=100
    if (req.method === 'GET' && path === '/api/gamedb/query') {
      const table = url.searchParams.get('table');
      const sql = url.searchParams.get('sql');
      if (!table || !sql) { json(res, { error: 'table and sql required' }, 400); return; }
      // Safety: only allow SELECT
      if (!sql.trim().toUpperCase().startsWith('SELECT')) {
        json(res, { error: 'Only SELECT queries allowed' }, 400);
        return;
      }
      json(res, getGameDB().query(table, sql));
      return;
    }

    res.writeHead(404); res.end('Not found');
  });

  // ── WebSocket for real-time updates ────────────────────────────────────────

  const wss = new WebSocketServer({ server });

  wss.on('connection', (ws) => {
    ws.send(JSON.stringify({
      type: 'init',
      ...manager.getStatus(),
      proxy: !!proxy,
      proxySession: proxy?.hasSession ?? false,
    }));
    // Send recent packets so client has context
    if (pktRing.length > 0) {
      ws.send(JSON.stringify({ type: 'pkt_batch', packets: pktRing.slice(-100) }));
    }
  });

  function broadcast(obj) {
    const data = JSON.stringify(obj);
    for (const ws of wss.clients) {
      if (ws.readyState === 1) ws.send(data);
    }
  }

  // Forward manager events to WebSocket clients
  manager.on('state', (e) => broadcast({ type: 'state', ...e }));
  manager.on('log', (e) => broadcast({ type: 'log', ...e }));
  manager.on('added', (e) => broadcast({ type: 'added', ...e }));
  manager.on('removed', (e) => broadcast({ type: 'removed', ...e }));
  manager.on('mining', (e) => broadcast({ type: 'mining', ...e }));

  // Forward proxy events
  let pktSeq = 0;
  if (proxy) {
    proxy.on('packet', (pkt) => {
      const entry = { seq: ++pktSeq, source: 'proxy', ...pkt };
      pktRing.push(entry);
      if (pktRing.length > PKT_RING_MAX) pktRing.shift();
      broadcast({ type: 'pkt', ...entry });
    });
    proxy.on('drop', (pkt) => {
      const entry = { seq: ++pktSeq, source: 'proxy', dropped: true, ...pkt };
      pktRing.push(entry);
      if (pktRing.length > PKT_RING_MAX) pktRing.shift();
      broadcast({ type: 'pkt', ...entry });
    });
    proxy.on('session', (info) => {
      broadcast({ type: 'proxy_session', ...info });
    });
  }

  // Also forward bot account packets into the packet log
  manager.on('packet', (e) => {
    const acc = manager.getAccount(e.account);
    const entry = {
      seq: ++pktSeq,
      source: 'bot',
      account: e.account,
      accountEmail: acc?.email || '?',
      direction: e.direction || 'S2C',
      msgId: e.msgId,
      name: e.name,
      data: sanitizeObj(e.data),
      size: e.raw?.length || 0,
      ts: Date.now(),
    };
    pktRing.push(entry);
    if (pktRing.length > PKT_RING_MAX) pktRing.shift();
    broadcast({ type: 'pkt', ...entry });
  });

  // Periodic full status broadcast (every 5s for uptime/ping counters)
  setInterval(() => {
    broadcast({ type: 'status', ...manager.getStatus(), proxy: !!proxy, proxySession: proxy?.hasSession ?? false });
  }, 5000);

  // Hot reload — notify browser when web/data/proto files change
  const hot = getHotReloader();
  hot.on('change', ({ type, filename, version }) => {
    if (type === 'web' || type === 'data' || type === 'proto') {
      broadcast({ type: 'reload', fileType: type, filename, version });
    }
  });

  server.listen(port, () => {
    console.log(`\x1b[36m[dashboard]\x1b[0m http://localhost:${port}`);
  });

  return server;
}

// ── Helpers ──────────────────────────────────────────────────────────────────

function json(res, data, status = 200) {
  res.writeHead(status, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify(data));
}

function readBody(req, cb) {
  const chunks = [];
  req.on('data', c => chunks.push(c));
  req.on('end', () => {
    try { cb(JSON.parse(Buffer.concat(chunks).toString())); }
    catch { cb({}); }
  });
}

function sanitizeObj(obj) {
  if (obj === null || obj === undefined) return obj;
  if (Buffer.isBuffer(obj) || obj instanceof Uint8Array) {
    return '0x' + Buffer.from(obj).toString('hex').substring(0, 64) + (obj.length > 32 ? '...' : '');
  }
  if (Array.isArray(obj)) return obj.slice(0, 200).map(sanitizeObj);
  if (typeof obj === 'object') {
    const out = {};
    for (const [k, v] of Object.entries(obj)) out[k] = sanitizeObj(v);
    return out;
  }
  return obj;
}
