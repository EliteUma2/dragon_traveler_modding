// AccountManager — load accounts from file, manage lifecycle, broadcast events
import { readFileSync } from 'fs';
import { EventEmitter } from 'events';
import { Account, getSharedProtocol } from './account.js';

export class AccountManager extends EventEmitter {
  constructor(opts = {}) {
    super();
    this.accounts = new Map();  // id → Account
    this.protocol = opts.protocol || getSharedProtocol();
    this._nextId = 1;
    this._staggerMs = opts.staggerMs ?? 2000; // delay between logins to avoid rate limiting
  }

  // ── Load accounts from file ────────────────────────────────────────────────
  // Format: email:password[:serverId]  (one per line, # comments, blank lines ok)

  loadFile(path) {
    const lines = readFileSync(path, 'utf-8').split('\n');
    let added = 0;
    for (const raw of lines) {
      const line = raw.trim();
      if (!line || line.startsWith('#')) continue;
      const parts = line.split(':');
      if (parts.length < 2) continue;
      const email = parts[0].trim();
      const password = parts.slice(1, -1).join(':').trim() || parts[1].trim();
      // If last part looks like a serverId (numeric), use it
      const lastPart = parts[parts.length - 1].trim();
      let serverId = null;
      let pw = password;
      if (parts.length >= 3 && /^\d+$/.test(lastPart)) {
        serverId = lastPart;
        pw = parts.slice(1, -1).join(':').trim();
      } else {
        pw = parts.slice(1).join(':').trim();
      }
      this.addAccount(email, pw, { serverId });
      added++;
    }
    return added;
  }

  // ── Account management ─────────────────────────────────────────────────────

  addAccount(email, password, opts = {}) {
    const id = this._nextId++;
    const account = new Account(id, email, password, {
      ...opts,
      protocol: this.protocol,
    });

    // Forward events
    account.on('state', (e) => this.emit('state', e));
    account.on('log', (e) => this.emit('log', e));
    account.on('packet', (e) => this.emit('packet', e));
    account.on('mining', (e) => this.emit('mining', { account: id, ...e }));

    this.accounts.set(id, account);
    this.emit('added', { id, email });
    return account;
  }

  removeAccount(id) {
    const acc = this.accounts.get(id);
    if (!acc) return false;
    acc.stop();
    acc.removeAllListeners();
    this.accounts.delete(id);
    this.emit('removed', { id });
    return true;
  }

  getAccount(id) {
    return this.accounts.get(id) || null;
  }

  // ── Bulk operations ────────────────────────────────────────────────────────

  async startAll() {
    const accs = [...this.accounts.values()].filter(a =>
      a.state === 'idle' || a.state === 'disconnected' || a.state === 'error'
    );
    for (let i = 0; i < accs.length; i++) {
      accs[i].start().catch(() => {});
      if (i < accs.length - 1 && this._staggerMs > 0) {
        await sleep(this._staggerMs);
      }
    }
  }

  stopAll() {
    for (const acc of this.accounts.values()) acc.stop();
  }

  async restartAll() {
    this.stopAll();
    await sleep(1000);
    await this.startAll();
  }

  // Start a specific account
  async startAccount(id) {
    const acc = this.accounts.get(id);
    if (!acc) throw new Error(`Account ${id} not found`);
    await acc.start();
  }

  stopAccount(id) {
    const acc = this.accounts.get(id);
    if (!acc) throw new Error(`Account ${id} not found`);
    acc.stop();
  }

  // ── Send packet to one or all accounts ─────────────────────────────────────

  sendTo(id, name, data = {}) {
    const acc = this.accounts.get(id);
    if (!acc) throw new Error(`Account ${id} not found`);
    acc.send(name, data);
  }

  sendToAll(name, data = {}) {
    for (const acc of this.accounts.values()) {
      if (acc.state === 'online') {
        try { acc.send(name, data); } catch {}
      }
    }
  }

  // Register handler on all accounts (current + future)
  handleAll(packetName, fn) {
    for (const acc of this.accounts.values()) {
      acc.handle(packetName, (pkt) => fn(pkt, acc));
    }
    // TODO: track for future accounts
    return this;
  }

  // ── Status ─────────────────────────────────────────────────────────────────

  getStatus() {
    const accs = [];
    for (const acc of this.accounts.values()) accs.push(acc.toJSON());
    return {
      total: accs.length,
      online: accs.filter(a => a.state === 'online').length,
      error: accs.filter(a => a.state === 'error').length,
      accounts: accs,
    };
  }

  getLogs(id, limit = 50) {
    const acc = this.accounts.get(id);
    if (!acc) return [];
    return acc.logs.slice(-limit);
  }
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
