// Account — state machine wrapping login + game client + ping loop
// States: idle → logging_in → connecting → online → disconnected / error
import { EventEmitter } from 'events';
import { gameTreeLogin, findServer, getLastPlayedRole, getAllServers } from './login.js';
import { createClient } from './client.js';
import { Protocol } from './protocol.js';
import { getHotReloader } from './hot.js';
import { getGameDB } from './gamedb.js';
import { decodeProtoToObject } from './protobuf.js';
import { DEFAULT_SETTINGS, runAutoPlay, stopAutoPlay as _stopAutoPlay } from './autoplay.js';

const PING_INTERVAL = 15000;
const RECONNECT_DELAY = 10000;

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// Byte-reverse a 64-bit value given as {hi, lo} uint32s → hex string
function byteReverse64(hi, lo) {
  const buf = Buffer.alloc(8);
  buf.writeUInt32LE(lo, 0);
  buf.writeUInt32LE(hi, 4);
  // Reverse all 8 bytes
  const reversed = Buffer.from(buf).reverse();
  return reversed.toString('hex');
}

// Shared protocol instance (loaded once, reused by all accounts)
let sharedProtocol = null;
export function getSharedProtocol() {
  if (!sharedProtocol) sharedProtocol = new Protocol();
  return sharedProtocol;
}

// ── Character creation helpers ──────────────────────────────────────────────

function randomName(len = 8) {
  const chars = 'abcdefghijklmnopqrstuvwxyz';
  const first = chars[Math.floor(Math.random() * chars.length)].toUpperCase();
  let name = first;
  for (let i = 1; i < len; i++) name += chars[Math.floor(Math.random() * chars.length)];
  return name;
}

export class Account extends EventEmitter {
  constructor(id, email, password, opts = {}) {
    super();
    this.id = id;
    this.email = email;
    this.password = password;
    this.serverId = opts.serverId || null;  // null = auto (last played)
    this.autoReconnect = opts.autoReconnect ?? true;
    this.protocol = opts.protocol || getSharedProtocol();

    // State
    this.state = 'idle';
    this.error = null;
    this.client = null;
    this.loginData = null;   // { userId, token, deviceId, serverData, config }
    this.serverInfo = null;  // { ip, port, name, ... }
    this.roleInfo = null;    // { rid, name, level, serverId, ... }
    this.isFirstLogin = false; // true when no existing role on target server
    this.loginResponse = null; // SCLogin decoded data
    this.pingTimer = null;
    this.pingCount = 0;
    this.lastPing = 0;
    this.lastPong = 0;
    this.connectedAt = 0;
    this.logs = [];          // ring buffer of recent log entries
    this._maxLogs = 200;
    this._reconnectTimer = null;
    this._stopping = false;

    // Main story progression
    this.curMap = null;        // current chapter from SCMapInfo
    this.mainDungeonId = null; // current dungeon from SCLogin.player.dungeonInfo.mainDungeonId
    this.lastClearedDungeon = null;  // last dungeonId from SCUpdateDungeon
    this._doingMainStory = false;

    // Hero/equip/item state (populated from login packets + mid-game updates)
    this._heroRoster = [];     // [{heroId, level, fightPower, star, id}]
    this._teamLineup = [];     // [heroId, ...] currently in formation
    this._equipBag = [];       // [{id, templateId, num, fighterTid}]
    this._items = {};          // {itemId: {itemKey, itemId, count}}
    this._currencies = {};     // {currencyId: count}
    this.playerLevel = 1;      // player level (hero level cap)

    // Task/activity state (populated from login packets)
    this._taskData = null;       // SCRspActiveTask data (daily/weekly/achievement tasks)
    this._activityList = null;   // SCAvSimpleList data (active activities)
    this._guideData = null;      // guide task completion state from SCLogin

    // Auto-play
    this.autoPlaySettings = { ...DEFAULT_SETTINGS };
    this._autoPlayRunning = false;
    this._autoPlayStats = { wins: 0, losses: 0, retries: 0, stage: null, status: 'idle' };
  }

  // ── Public API ─────────────────────────────────────────────────────────────

  async start() {
    if (this.state !== 'idle' && this.state !== 'disconnected' && this.state !== 'error') return;
    this._stopping = false;
    try {
      await this._doLogin();
      await this._doConnect();
    } catch (e) {
      this._setError(e.message);
      if (this.autoReconnect && !this._stopping) this._scheduleReconnect();
    }
  }

  stop() {
    this._stopping = true;
    clearInterval(this.pingTimer);
    clearTimeout(this._reconnectTimer);
    this.pingTimer = null;
    this._reconnectTimer = null;
    if (this.client) {
      this.client.close();
      this.client = null;
    }
    this._setState('idle');
    this._log('Stopped');
  }

  // Send any packet by name
  send(name, data = {}) {
    if (!this.client || this.state !== 'online') throw new Error('Not online');
    this.client.send(name, data);
    this._log(`C→S ${name}`);
  }

  // Send and wait for response
  async sendAndWait(sendName, data, waitName, timeoutMs = 10000) {
    if (!this.client || this.state !== 'online') throw new Error('Not online');
    this._log(`C→S ${sendName} (waiting for ${waitName})`);
    const resp = await this.client.sendAndWait(sendName, data, waitName, timeoutMs);
    this._log(`S→C ${waitName} (${resp.raw.length}B)`);
    return resp;
  }

  // Register a persistent handler for incoming packets
  handle(name, fn) {
    if (this.client) this.client.handle(name, fn);
    // Store for re-registration on reconnect
    if (!this._handlers) this._handlers = [];
    this._handlers.push({ name, fn });
    return this;
  }

  // ── Main Story Dungeon ──────────────────────────────────────────────────────

  // Attempt the current main story dungeon. Returns { win, dungeonId, rewards?, error? }
  async doMainStory() {
    if (!this.client || this.state !== 'online') throw new Error('Not online');
    if (this._doingMainStory) throw new Error('Already running main story');
    this._doingMainStory = true;

    try {
      // Determine which dungeon to attempt
      const dungeonId = this._getNextDungeonId();
      if (!dungeonId) throw new Error('Cannot determine next dungeon (no curMap set — wait for SCMapInfo)');

      this._log(`Main Story: entering dungeon ${dungeonId}`);

      // Step 1: Enter dungeon
      const enterResp = await this.sendAndWait('CSReqEnterDungeon', {
        dungeonId,
        copyId: dungeonId,
        useHelpFighter: false,
        playerId: '',
        helpFighterId: -1,
        helpFighterSquadId: -1,
        pos: -1,
        sortIndex: -1,
        closeRocker: -1,
        seq: 1,
      }, 'SCRspEnterDungeon', 15000);

      // Extract battle UUID from response: f3.f1.f1.{lo, hi}
      const battleIdObj = enterResp.data?.f3?.f1?.f1;
      if (!battleIdObj || battleIdObj.lo === undefined) {
        throw new Error('SCRspEnterDungeon missing battle UUID');
      }

      // Byte-reverse the 64-bit UUID for the draw award packet
      const uuid = byteReverse64(battleIdObj.hi, battleIdObj.lo);
      this._log(`Main Story: battle UUID = ${uuid}, waiting for auto-battle...`);

      // Step 2: Send draw award (claim victory with auto-battle)
      this._log(`Main Story: claiming reward (result=1)...`);

      // Listen for both SCDrawDungeonAward (win) and SCNotifyBatlleInterrupt (fail)
      const result = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => {
          cleanup();
          reject(new Error('Draw award timeout'));
        }, 15000);

        const cleanup = () => {
          clearTimeout(timeout);
          this.client?.off?.('SCDrawDungeonAward', onWin);
          this.client?.off?.('SCNotifyBatlleInterrupt', onFail);
        };

        const onWin = (pkt) => {
          cleanup();
          resolve({ win: true, data: pkt.data });
        };
        const onFail = (pkt) => {
          cleanup();
          resolve({ win: false, data: pkt.data });
        };

        // Register one-shot handlers
        this.client.handle('SCDrawDungeonAward', onWin);
        this.client.handle('SCNotifyBatlleInterrupt', onFail);

        // Send the draw award
        this.client.send('CSDrawDungeonAward', {
          dungeonId,
          battleEnd: {
            copyId: dungeonId,
            result: 1,
            operates: [{
              header: 2502,
              itemId: 2602,
              entity: {
                size: 17,
                type: 1,
                typeName: 'hero',
                uuid,
                autoBattle: true,
              },
              seq: 0,
              tick: 2,
              emptyTick: false,
            }],
          },
        });
      });

      if (result.win) {
        this.lastClearedDungeon = dungeonId;
        this._log(`Main Story: WIN dungeon ${dungeonId}!`);
        return { win: true, dungeonId, rewards: result.data };
      } else {
        this._log(`Main Story: FAIL dungeon ${dungeonId} (battle interrupted)`);
        return { win: false, dungeonId, error: 'Battle failed' };
      }
    } finally {
      this._doingMainStory = false;
    }
  }

  // Compute next dungeon ID to attempt using DB NextStageId chain
  _getNextDungeonId() {
    if (this.lastClearedDungeon) {
      // Just cleared a dungeon — look up NextStageId from the database
      const db = getGameDB();
      const rows = db.query('MainDungeon',
        'SELECT NextStageId FROM MainDungeon WHERE id = ?', [this.lastClearedDungeon]);
      if (rows.length && rows[0].NextStageId > 0) {
        return rows[0].NextStageId;
      }
      // Fallback: try +1
      return this.lastClearedDungeon + 1;
    }
    // Use mainDungeonId from SCLogin — this is the current uncleared stage
    if (this.mainDungeonId) {
      return this.mainDungeonId;
    }
    if (this.curMap) {
      // Last resort: curMap is the chapter (e.g. 1002 = chapter 2)
      const chapter = this.curMap % 100;
      return 1000000 + chapter * 100 + 1;
    }
    return null;
  }

  // ── Auto-play controls ──────────────────────────────────────────────────────

  startAutoPlay(overrides = {}) {
    Object.assign(this.autoPlaySettings, overrides, { enabled: true });
    this._log(`[autoplay] Starting with: ${JSON.stringify(this.autoPlaySettings)}`);
    // Use hot-reloaded module if available
    const mod = getHotReloader().get('autoplay');
    const run = mod?.runAutoPlay || runAutoPlay;
    run(this).catch(e => this._log(`[autoplay] Fatal: ${e.message}`));
    this.emit('state', { account: this.id, state: this.state, prev: this.state });
  }

  stopAutoPlay() {
    const mod = getHotReloader().get('autoplay');
    const stop = mod?.stopAutoPlay || _stopAutoPlay;
    stop(this);
    this._log('[autoplay] Stopped by user');
    this.emit('state', { account: this.id, state: this.state, prev: this.state });
  }

  toJSON() {
    return {
      id: this.id,
      email: this.email,
      state: this.state,
      error: this.error,
      serverId: this.serverId,
      serverName: this.serverInfo?.name || null,
      serverAddr: this.serverInfo ? `${this.serverInfo.ip}:${this.serverInfo.port}` : null,
      roleName: this.roleInfo?.name || null,
      roleLevel: this.roleInfo?.level || null,
      roleRid: this.roleInfo?.rid || null,
      uid: this.loginData?.serverData?.uid || null,
      pingCount: this.pingCount,
      lastPing: this.lastPing,
      lastPong: this.lastPong,
      connectedAt: this.connectedAt,
      uptime: this.connectedAt ? Date.now() - this.connectedAt : 0,
      curMap: this.curMap,
      mainDungeonId: this.mainDungeonId,
      lastClearedDungeon: this.lastClearedDungeon,
      nextDungeonId: this._getNextDungeonId(),
      doingMainStory: this._doingMainStory,
      playerLevel: this.playerLevel,
      heroCount: this._heroRoster.length,
      teamLineup: this._teamLineup,
      autoPlay: {
        ...this.autoPlaySettings,
        running: this._autoPlayRunning,
        stats: this._autoPlayStats,
      },
    };
  }

  // ── Internal: parse SCFighterBagMsg ──────────────────────────────────────────

  _parseFighterBag(bag) {
    // Support both raw (fN) and named field access (protobufjs may decode either way)
    const base = bag.baseFighters || bag.f1 || [];
    const general = bag.general || bag.f5 || [];
    const formations = bag.formations || bag.f3;
    const lineup = bag.f7; // raw-only field

    // Build hero roster from baseFighters, enriched with general stats
    const generalMap = {};
    for (const g of general) {
      const heroId = g.heroId || g.f1;
      generalMap[heroId] = { fightPower: g.fightPower || g.f5 || 0, level: g.level || g.f8 || 1 };
    }

    this._heroRoster = base.map(h => ({
      id: h.id || h.f1,
      heroId: h.heroId || h.f2,
      star: h.star || h.f3 || 3,
      level: generalMap[h.heroId || h.f2]?.level || 1,
      fightPower: generalMap[h.heroId || h.f2]?.fightPower || 0,
    }));

    // Six fighter max level (breakthrough cap) — wire tag 18
    const sixMaxLv = bag.sixFighterMaxLv || bag.f18;
    if (sixMaxLv && typeof sixMaxLv === 'number') {
      this._sixFighterMaxLv = sixMaxLv;
    }

    // Team lineup — extract from formations (f3), NOT f7 (f7 = all owned hero IDs)
    if (formations) {
      const form = Array.isArray(formations) ? formations[0] : formations;
      const positions = form?.positions || form?.f4;
      if (positions && Array.isArray(positions)) {
        this._teamLineup = positions.map(p => p.fighterTid || p.f2).filter(Boolean);
      }
    }
  }

  // ── Internal flow ──────────────────────────────────────────────────────────

  async _doLogin() {
    this._setState('logging_in');
    this._log('Logging in to game-tree.com...');

    this.loginData = await gameTreeLogin(this.email, this.password, {
      deviceId: this.loginData?.deviceId, // reuse deviceId across reconnects
    });

    const sd = this.loginData.serverData;
    this._log(`Login OK — uid=${sd.uid}, ${sd.roles?.length || 0} roles`);

    // Determine which server + role to use
    if (!this.serverId) {
      const lastRole = getLastPlayedRole(sd);
      if (lastRole) {
        this.serverId = lastRole.serverId;
        this.roleInfo = lastRole;
      } else {
        // Fresh account — auto-pick newest/recommended server
        const servers = getAllServers(sd);
        const pick = servers.find(s => /venus.*2/i.test(s.name)) || servers[servers.length - 1];
        if (!pick) throw new Error('No servers available');
        this.serverId = pick.serverId;
        this._log(`Auto-selected server: ${pick.name} (${pick.serverId})`);
      }
    } else {
      this.roleInfo = sd.roles?.find(r => r.serverId === this.serverId) || null;
    }

    // First login = account has no existing role on the target server
    this.isFirstLogin = !this.roleInfo;

    this.serverInfo = findServer(sd, this.serverId);
    if (!this.serverInfo) throw new Error(`Server ${this.serverId} not found in server list`);

    this._log(`Target: ${this.serverInfo.name} (${this.serverInfo.ip}:${this.serverInfo.port})`);
    if (this.roleInfo) {
      this._log(`Role: ${this.roleInfo.name} Lv${this.roleInfo.level} (${this.roleInfo.rid})`);
    } else {
      this._log('No existing role — will create character after login');
    }
  }

  async _doConnect() {
    this._setState('connecting');
    const { ip, port } = this.serverInfo;
    this._log(`Connecting to ${ip}:${port}...`);

    this.client = createClient(this.protocol);

    // Re-register any persistent handlers
    if (this._handlers) {
      for (const h of this._handlers) this.client.handle(h.name, h.fn);
    }

    // Log unhandled S→C packets
    this.client.onDefault((pkt) => {
      this._log(`S→C ${pkt.name} (${pkt.raw.length}B)`);
      this.emit('packet', { account: this.id, ...pkt });
    });

    await this.client.connect(ip, port);
    this._log('TCP connected, waiting for handshake...');

    const hs = await this.client.waitFor('_handshake', 10000);
    this._log(`Handshake OK (${hs.data.keyLen}B key)`);

    // ── Phase 1: Authenticate ─────────────────────────────────────────────────
    await this._authenticate();

    // ── Phase 2: Customize character (first login only) ─────────────────────
    if (this.isFirstLogin) {
      // Wait for SCLoginFinish before sending character setup packets
      await this.client.waitFor('SCLoginFinish', 15000);
      this._log('S→C SCLoginFinish — server ready');
      await this._createCharacter();
    }

    // ── Phase 3: Go online + start keepalive ──────────────────────────────────
    this._setState('online');
    this.connectedAt = Date.now();
    this.pingCount = 0;
    this._startPingLoop();

    // Track main story progression
    this.client.handle('SCMapInfo', (pkt) => {
      this.curMap = pkt.data?.curMap;
      this._log(`S→C SCMapInfo: curMap=${this.curMap}`);
    });
    this.client.handle('SCUpdateDungeon', (pkt) => {
      const id = pkt.data?.dungeonId;
      if (id) {
        this.lastClearedDungeon = id;
        this._log(`S→C SCUpdateDungeon: cleared ${id}`);
        this.emit('state', { account: this.id, state: this.state, prev: this.state });
      }
    });

    // ── Hero/equip/item state tracking ──────────────────────────────────────────

    // SCFighterBagMsg — full hero roster from login
    this.client.handle('SCFighterBagMsg', (pkt) => {
      // Always try raw decode first — FighterBagMsg has wrong proto field numbers
      let bag = null;
      if (pkt.raw) {
        try {
          const raw = decodeProtoToObject(pkt.raw);
          bag = raw?.f1;
          this._log(`S→C SCFighterBagMsg (${pkt.raw.length}B) raw keys: ${bag ? Object.keys(bag).join(',') : 'null'}`);
        } catch (e) {
          this._log(`S→C SCFighterBagMsg raw decode error: ${e.message}`);
        }
      }
      // Fallback to protobufjs named decode
      if (!bag) bag = pkt.data?.fighterBagMsg || pkt.data?.f1;
      if (!bag) { this._log('S→C SCFighterBagMsg: no bag data'); return; }
      try {
        this._parseFighterBag(bag);
        this._log(`S→C SCFighterBagMsg: ${this._heroRoster.length} heroes, team=[${this._teamLineup.join(',')}]`);
      } catch (e) {
        this._log(`S→C SCFighterBagMsg parse error: ${e.message}`);
      }
      this.emit('state', { account: this.id, state: this.state, prev: this.state });
    });

    // SCBagMsg — item inventory from login
    this.client.handle('SCBagMsg', (pkt) => {
      const items = pkt.data?.itemBagMsg?.items || [];
      this._items = {};
      for (const it of items) {
        this._items[it.itemId] = { itemKey: it.itemKey, itemId: it.itemId, count: it.itemNum };
      }
      this._log(`S→C SCBagMsg: ${items.length} item types`);
    });

    // SCEquipmentBagMsg — equipment inventory from login
    this.client.handle('SCEquipmentBagMsg', (pkt) => {
      const equips = pkt.data?.equipBagMsg?.equipments || [];
      this._equipBag = equips.map(e => ({
        id: e.id, templateId: e.templateId, num: e.num || 1, fighterTid: e.fighterTid || 0,
      }));
      this._log(`S→C SCEquipmentBagMsg: ${equips.length} equipment pieces`);
    });

    // SCPlayerLevelUpdate — player level changes (hero level cap)
    this.client.handle('SCPlayerLevelUpdate', (pkt) => {
      if (pkt.data?.cuLv) {
        this.playerLevel = pkt.data.cuLv;
        this._log(`S→C SCPlayerLevelUpdate: Lv${this.playerLevel} (exp=${pkt.data.cuExp})`);
      }
    });

    // SCCurrencyChange — currency updates
    this.client.handle('SCCurrencyChange', (pkt) => {
      const currencies = pkt.data?.currency || [];
      for (const c of currencies) {
        this._currencies[c.id] = c.count;
      }
    });

    // SCFighterLevelUpResp — hero level up confirmation
    this.client.handle('SCFighterLevelUpResp', (pkt) => {
      const { fighterTid, level } = pkt.data || {};
      if (fighterTid && level) {
        const hero = this._heroRoster.find(h => h.heroId === fighterTid);
        if (hero) hero.level = level;
      }
    });

    // SCAddFighters — new heroes from gacha/rewards
    this.client.handle('SCAddFighters', (pkt) => {
      const added = pkt.data?.addFighter || [];
      for (const f of added) {
        // Check by instance ID (not heroId — duplicates of same hero are allowed)
        if (!this._heroRoster.find(h => h.id === f.id)) {
          this._heroRoster.push({
            id: f.id, heroId: f.templateId, star: f.starLv || 3,
            level: 1, fightPower: 0,
          });
        }
      }
      if (added.length) {
        this._log(`S→C SCAddFighters: +${added.length} heroes (${added.map(f => f.templateId).join(',')})`);
      }
    });

    // SCEquipChange — equipment state updates
    this.client.handle('SCEquipChange', (pkt) => {
      const updates = pkt.data?.updateEquips || [];
      for (const u of updates) {
        const idx = this._equipBag.findIndex(e => e.id === u.id && e.templateId === u.templateId);
        const entry = { id: u.id, templateId: u.templateId, num: u.num || 1, fighterTid: u.fighterTid || 0 };
        if (idx >= 0) {
          this._equipBag[idx] = entry;
        } else if (u.num > 0) {
          this._equipBag.push(entry);
        }
      }
    });

    // SCBagChange — mid-game item changes
    this.client.handle('SCBagChange', (pkt) => {
      const items = pkt.data?.items || [];
      for (const it of items) {
        this._items[it.itemId] = { itemKey: it.itemKey, itemId: it.itemId, count: it.itemNum };
      }
    });

    // SCSetFormation — formation update confirmation
    this.client.handle('SCSetFormation', (pkt) => {
      const form = pkt.data?.f1;
      if (form?.f4) {
        this._teamLineup = form.f4.map(p => p.f2).filter(Boolean);
        this._log(`S→C SCSetFormation: team=[${this._teamLineup.join(',')}]`);
      }
    });

    // SCUpdateFightPower — hero fight power changes
    this.client.handle('SCUpdateFightPower', (pkt) => {
      const tid = pkt.data?.fighterTid || pkt.data?.f1;
      const fp = pkt.data?.fightPower || pkt.data?.f2;
      if (tid && fp) {
        const hero = this._heroRoster.find(h => h.heroId === tid);
        if (hero) hero.fightPower = fp;
      }
    });

    // SCNewEquipments — new equipment acquired
    this.client.handle('SCNewEquipments', (pkt) => {
      const equips = pkt.data?.equipments || pkt.data?.f1 || [];
      for (const e of equips) {
        const id = e.id || e.f1;
        const templateId = e.templateId || e.f2;
        const num = e.num || e.f3 || 1;
        if (templateId) {
          this._equipBag.push({ id, templateId, num, fighterTid: 0 });
        }
      }
    });

    // ── Task/activity state tracking ───────────────────────────────────────────

    // SCRspActiveTask — full task dump (daily/weekly/achievements) with statuses
    this.client.handle('SCRspActiveTask', (pkt) => {
      this._taskData = pkt.data;
      const tasks = pkt.data?.tasks || [];
      let claimable = 0;
      for (const group of tasks) {
        const statuses = group.taskStatus || [];
        claimable += statuses.filter(t => t.taskStatus === 1).length;
        // Also count claimable activity milestones
        const actives = group.activeStatus || [];
        claimable += actives.filter(a => a.activeStatus === 1).length;
      }
      this._log(`S→C SCRspActiveTask: ${tasks.length} task groups, ${claimable} claimable`);
    });

    // SCAvSimpleList — active activities list
    this.client.handle('SCAvSimpleList', (pkt) => {
      this._activityList = pkt.data;
      const items = pkt.data?.list || pkt.data?.f1 || [];
      this._log(`S→C SCAvSimpleList: ${items.length} activities`);
    });

    // SCNotifyTaskFinish — new tasks become claimable mid-session
    this.client.handle('SCNotifyTaskFinish', (pkt) => {
      const infos = pkt.data?.taskInfo || [];
      const claimable = infos.filter(t => t.state === 1);
      if (claimable.length > 0) {
        this._log(`S→C SCNotifyTaskFinish: ${claimable.length} newly claimable tasks`);
      }
    });

    // Activity data pushed by server at login
    this.client.handle('SCAvCarnival', (pkt) => {
      this._carnivalData = pkt.data;
      this._log(`S→C SCAvCarnival: id=${pkt.data?.id}`);
    });
    this.client.handle('SCAvGrand', (pkt) => {
      this._grandData = pkt.data;
      this._log(`S→C SCAvGrand: activityId=${pkt.data?.activityId}`);
    });
    this.client.handle('SCAvBiWeeklySignIn', (pkt) => {
      this._biWeeklyData = pkt.data;
      this._log(`S→C SCAvBiWeeklySignIn: day=${pkt.data?.day}`);
    });

    this.client.handle('_disconnect', () => {
      this._onDisconnect('Server closed connection');
    });

    // ── Post-login gather: claim everything + power up (no auto-fight) ────────
    // Run in background so it doesn't block login
    setTimeout(() => this._postLoginGather().catch(e => this._log(`[login-gather] ${e.message}`)), 3000);
  }

  // Claim all rewards and run a full power-up pass (level/equip/star-up/etc)
  // Does NOT start auto-fight — only prepares the account
  async _postLoginGather() {
    if (this.state !== 'online') return;
    this._log('[login-gather] Running post-login gather pass...');
    try {
      const { claimAll } = await import('./claims.js');
      await claimAll(this);
    } catch (e) {
      this._log(`[login-gather] Claim failed: ${e.message}`);
    }
    if (this.state !== 'online') return;
    try {
      const mod = getHotReloader().get('autoplay');
      const autoplay = mod || await import('./autoplay.js');
      if (autoplay.optimizeFormation) await autoplay.optimizeFormation(this);
      if (autoplay.levelAllHeroes) await autoplay.levelAllHeroes(this);
      if (autoplay.autoEquip) await autoplay.autoEquip(this);
      if (autoplay.doGachaPulls) await autoplay.doGachaPulls(this);
      if (autoplay.levelAllHeroes) await autoplay.levelAllHeroes(this); // re-level after gacha

      // If we're already past the chapter-3 transition point, run the tower intro once
      const nextDungeon = this._getNextDungeonId?.() || 0;
      const chapter = nextDungeon ? Math.floor((nextDungeon - 1000000) / 100) : 0;
      if (chapter >= 3 && !this._towerCh3Done && autoplay.runTowerChapter3Intro) {
        await autoplay.runTowerChapter3Intro(this);
      }
    } catch (e) {
      this._log(`[login-gather] Power-up failed: ${e.message}`);
    }
    this._log('[login-gather] Done');
  }

  // ── Phase 1: Send CSLogin ─────────────────────────────────────────────────

  async _authenticate() {
    this._setState('authenticating');
    const sd = this.loginData.serverData;
    const cfg = this.loginData.config;

    // playerId = serverId + uid (e.g. "3746000e4f10")
    const playerId = this.roleInfo?.rid || (this.serverId + sd.uid);

    const csLogin = {
      accid: sd.uid,
      playerId,
      platform: 'WindowsPlayer',
      token: sd.token,
      reconnect: 0,
      sourceVersion: cfg.sourceVersion,
      dataVersion: cfg.dataVersion,
      language: 'en',
      guideLogin: this.isFirstLogin,
    };

    this._log(`C→S CSLogin (accid=${sd.uid}, firstLogin=${this.isFirstLogin})`);
    const loginResp = await this.client.sendAndWait('CSLogin', csLogin, 'SCLogin', 15000);
    this.loginResponse = loginResp.data;
    this._log(`S→C SCLogin OK (${loginResp.raw.length}B)`);

    // Send CSVideoCheck after login
    this.client.send('CSVideoCheck', {});
    this._log('C→S CSVideoCheck');

    // Extract dungeon progress from login
    // Named decode path (protobufjs): player.dungeonInfo.mainDungeonId
    // mainDungeonId = last cleared stage. Next to attempt = NextStageId(mainDungeonId)
    const dungeonInfo = loginResp.data?.player?.dungeonInfo;
    if (dungeonInfo?.mainDungeonId) {
      this.mainDungeonId = dungeonInfo.mainDungeonId;
      this.lastClearedDungeon = dungeonInfo.mainDungeonId;
    }
    // Fallback: raw decode — DungeonMsg is at wire field 10
    // f10.f1 = mainDungeonId (last cleared stage)
    if (!this.mainDungeonId) {
      try {
        const raw = decodeProtoToObject(loginResp.raw);
        const rawPlayer = raw?.f1;
        const rawDungeon = rawPlayer?.f10;
        if (rawDungeon?.f1 && rawDungeon.f1 >= 1000100) {
          this.mainDungeonId = rawDungeon.f1;
          this.lastClearedDungeon = rawDungeon.f1;
        }
      } catch {}
    }
    if (this.mainDungeonId) {
      this._log(`Dungeon progress: mainDungeonId=${this.mainDungeonId} (last cleared)`);
    }
    // Extract player level
    const pLevel = loginResp.data?.player?.level;
    if (pLevel) {
      this.playerLevel = pLevel;
      this._log(`Player level: ${this.playerLevel}`);
    }
  }

  // ── Phase 2: Customize character on first login ────────────────────────────
  // Server auto-creates a role with guideLogin=true; we rename + select class

  async _createCharacter() {
    const name = randomName(8);
    this._log(`Customizing character: name="${name}"...`);

    // Rename the auto-created role
    this.client.send('CSChangeNameOrSignature', {
      type: 1,                              // 1 = set name
      detail: name,                            // character name
      createRole: true,
    });
    this._log(`C→S CSChangeNameOrSignature (name=${name}, createRole=true)`);

    // Select character class
    this.client.send('CSMythsSelectSex', {
      tid: 8201,                            // character template id
    });
    this._log(`C→S CSMythsSelectSex (tid=8201)`);

    this.isFirstLogin = false;
  }

  _startPingLoop() {
    clearInterval(this.pingTimer);
    this.pingTimer = setInterval(async () => {
      if (this.state !== 'online' || !this.client?.connected) return;
      try {
        this.lastPing = Date.now();
        this.client.send('CSPing', {});
        this.pingCount++;
        // Don't wait for pong — just fire and forget to avoid blocking
      } catch (e) {
        this._log(`Ping failed: ${e.message}`);
      }
    }, PING_INTERVAL);

    // Track pongs
    this.client.handle('SCPong', () => {
      this.lastPong = Date.now();
    });
  }

  _onDisconnect(reason) {
    if (this._stopping) return;
    clearInterval(this.pingTimer);
    this.pingTimer = null;
    this._setState('disconnected');
    this._log(`Disconnected: ${reason}`);
    if (this.autoReconnect && !this._stopping) this._scheduleReconnect();
  }

  _scheduleReconnect() {
    clearTimeout(this._reconnectTimer);
    this._log(`Reconnecting in ${RECONNECT_DELAY / 1000}s...`);
    this._reconnectTimer = setTimeout(() => {
      if (this._stopping) return;
      this._log('Reconnecting...');
      this.start().catch(() => {}); // errors handled inside start()
    }, RECONNECT_DELAY);
  }

  _setState(state) {
    const prev = this.state;
    this.state = state;
    if (state !== 'error') this.error = null;
    this.emit('state', { account: this.id, state, prev });
  }

  _setError(msg) {
    this.error = msg;
    this._setState('error');
    this._log(`ERROR: ${msg}`);
  }

  _log(msg) {
    const entry = { ts: Date.now(), msg };
    this.logs.push(entry);
    if (this.logs.length > this._maxLogs) this.logs.shift();
    this.emit('log', { account: this.id, ...entry });
  }
}
