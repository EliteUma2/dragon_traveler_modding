// GameDB — reads the game's SQLite data tables for ID→name resolution
// DB files are in data/db/ (copied from the game client's local_data/db/)
import { createRequire } from 'module';
import { dirname, join } from 'path';
import { fileURLToPath, pathToFileURL } from 'url';
import { readdirSync, existsSync } from 'fs';

const require = createRequire(import.meta.url);
const Database = require('better-sqlite3');

const __dirname = dirname(fileURLToPath(import.meta.url));
const DB_DIR = join(__dirname, '..', 'data', 'db');

// Binary string decoder — game stores localized strings as [4B LE length][UTF-8 data]
function decodeBinStr(buf) {
  if (buf === null || buf === undefined) return '';
  const b = Buffer.isBuffer(buf) ? buf : Buffer.from(buf);
  if (b.length < 4) return '';
  const len = b.readUInt32LE(0);
  if (len <= 0 || 4 + len > b.length) return '';
  return b.toString('utf-8', 4, 4 + len);
}

export class GameDB {
  constructor(dbDir) {
    this._dir = dbDir || DB_DIR;
    this._dbs = new Map();   // filename → Database
    this._cache = new Map(); // cacheKey → value
    this._open();
  }

  _open() {
    if (!existsSync(this._dir)) return;
    for (const f of readdirSync(this._dir).filter(f => f.endsWith('.db'))) {
      try {
        this._dbs.set(f, new Database(join(this._dir, f), { readonly: true }));
      } catch {}
    }
  }

  // Find which db has a given table
  _findTable(tableName) {
    for (const [file, db] of this._dbs) {
      try {
        const exists = db.prepare(
          "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?"
        ).get(tableName);
        if (exists) return db;
      } catch {}
    }
    return null;
  }

  // Get a localized name from a _Lang table
  // pattern: "TableName$LangFieldName_id" → __BIN__enUS column
  getLangString(table, langField, id, lang = 'enUS') {
    const cacheKey = `${table}:${langField}:${id}:${lang}`;
    if (this._cache.has(cacheKey)) return this._cache.get(cacheKey);

    const langTable = table + '_Lang';
    const db = this._findTable(langTable);
    if (!db) return null;

    const key = `${table}$${langField}_${id}`;
    const col = `__BIN__${lang}`;
    try {
      const row = db.prepare(`SELECT [${col}] as v FROM [${langTable}] WHERE id = ?`).get(key);
      const val = row ? decodeBinStr(row.v) : null;
      this._cache.set(cacheKey, val);
      return val;
    } catch {
      return null;
    }
  }

  // ── Convenience lookups ──────────────────────────────────────────────────────

  heroName(id) {
    // LangTitle = character name (Siegfried), LangHeroName = class name (Armored Princess)
    return this.getLangString('Heroes', 'LangTitle', id)
      || this.getLangString('Heroes', 'LangHeroName', id)
      || `Hero#${id}`;
  }

  heroInfo(id) {
    const db = this._findTable('Heroes');
    if (!db) return null;
    try {
      const row = db.prepare('SELECT id, Quality, Type, HeroGroup FROM Heroes WHERE id = ?').get(id);
      if (!row) return null;
      return { ...row, name: this.heroName(id) };
    } catch { return null; }
  }

  itemName(id) {
    return this.getLangString('Item', 'LangName', id) || `Item#${id}`;
  }

  itemInfo(id) {
    const db = this._findTable('Item');
    if (!db) return null;
    try {
      const row = db.prepare('SELECT id, ItemType, ItemQuality, SubType, Icon FROM Item WHERE id = ?').get(id);
      if (!row) return null;
      return { ...row, name: this.itemName(id) };
    } catch { return null; }
  }

  equipName(id) {
    return this.getLangString('Equipments', 'LangName', id) || `Equip#${id}`;
  }

  equipInfo(id) {
    const db = this._findTable('Equipments');
    if (!db) return null;
    try {
      const row = db.prepare('SELECT * FROM Equipments WHERE id = ?').get(id);
      if (!row) return null;
      return { id: row.id, name: this.equipName(id) };
    } catch { return null; }
  }

  // Currency IDs (1=Diamond, 3=Gold, etc.) — these are actually items with type=1
  currencyName(id) {
    return this.itemName(id);
  }

  // ── Hero class/post ────────────────────────────────────────────────────────

  heroPost(id) {
    const cacheKey = `heroPost:${id}`;
    if (this._cache.has(cacheKey)) return this._cache.get(cacheKey);
    const db = this._findTable('Heroes');
    if (!db) return null;
    try {
      const row = db.prepare('SELECT Post, Quality, AtkType, JobRangeType FROM Heroes WHERE id = ?').get(id);
      if (row) this._cache.set(cacheKey, row);
      return row || null;
    } catch { return null; }
  }

  // ── Star-up requirements (HeroesStar table) ───────────────────────────────
  // Returns number of self-copies needed to go from currentStar to currentStar+1
  // Uses __BIN__ItselfNum which is quality→count pairs
  starUpCopiesNeeded(currentStar, quality) {
    const cacheKey = `starCopies:${currentStar}`;
    let row = this._cache.get(cacheKey);
    if (!row) {
      const db = this._findTable('HeroesStar');
      if (!db) return 0;
      try {
        row = db.prepare('SELECT [__BIN__ItselfNum] as itselfNum, ItselfBaseNum FROM HeroesStar WHERE id = ?').get(currentStar);
        if (row) this._cache.set(cacheKey, row);
      } catch { return 0; }
    }
    if (!row) return 0;
    // Parse __BIN__ItselfNum: array of {key, value} pairs where key=quality, value=count
    const blob = row.itselfNum;
    if (!blob || !Buffer.isBuffer(blob) || blob.length < 8) return 0;
    // Binary format: [4B count][repeated: 4B quality, 4B count]
    const numPairs = blob.readInt32LE(0);
    for (let i = 0; i < numPairs; i++) {
      const off = 4 + i * 8;
      if (off + 8 > blob.length) break;
      const q = blob.readInt32LE(off);
      const cnt = blob.readInt32LE(off + 4);
      if (q === quality) return cnt;
    }
    // Fallback: if quality not found, return first pair's count (all qualities usually same)
    if (numPairs > 0) return blob.readInt32LE(8);
    return 0;
  }

  // Max hero level at a given star level
  starMaxLevel(star) {
    const db = this._findTable('HeroesStar');
    if (!db) return 100;
    try {
      const row = db.prepare('SELECT HeroMaxLevel FROM HeroesStar WHERE id = ?').get(star);
      return row?.HeroMaxLevel || 100;
    } catch { return 100; }
  }

  // ── Equipment slot (Part) from template ID ────────────────────────────────
  // Returns {Part, Quality, JobLimit} for an equipment template
  equipInfo(templateId) {
    const cacheKey = `equip:${templateId}`;
    if (this._cache.has(cacheKey)) return this._cache.get(cacheKey);
    const db = this._findTable('Equipments');
    if (!db) return null;
    try {
      const row = db.prepare('SELECT Part, Quality, [__BIN__JobLimit] as jobLimit FROM Equipments WHERE id = ?').get(templateId);
      if (row) {
        // Parse JobLimit blob: [4B count][repeated 4B post values]
        const jobs = [];
        if (row.jobLimit && Buffer.isBuffer(row.jobLimit) && row.jobLimit.length >= 4) {
          const n = row.jobLimit.readInt32LE(0);
          for (let i = 0; i < n; i++) {
            const off = 4 + i * 4;
            if (off + 4 > row.jobLimit.length) break;
            jobs.push(row.jobLimit.readInt32LE(off));
          }
        }
        const result = { part: row.Part, quality: row.Quality, jobs };
        this._cache.set(cacheKey, result);
        return result;
      }
      return null;
    } catch { return null; }
  }

  // ── Level-up costs ─────────────────────────────────────────────────────────

  // Returns {gold, essence} cost to level from currentLv to targetLv
  // Gold = sum of 30*lv for each level, Essence = lookup table
  levelUpCost(fromLv, toLv) {
    const ESSENCE_TABLE = [0, 63, 88, 114, 140, 168, 196, 225, 255, 286,
      318, 351, 385, 420, 456, 493, 531, 570, 610, 651];
    // Gold: 30*lv per level
    let gold = 0, essence = 0;
    for (let lv = fromLv; lv < toLv; lv++) {
      gold += 30 * lv;
      essence += lv < ESSENCE_TABLE.length ? ESSENCE_TABLE[lv] : Math.round(0.5 * lv * lv + 23.5 * lv + 39);
    }
    return { gold, essence };
  }

  // Check if account can afford to level a hero
  canAffordLevel(currencies, fromLv, toLv) {
    const cost = this.levelUpCost(fromLv, toLv);
    const gold = currencies[3] || 0;    // currency 3 = Gold
    const ess = currencies[5] || 0;     // currency 5 = Wyrm Essence / Luminary EXP
    return gold >= cost.gold && ess >= cost.essence;
  }

  // Find max affordable level given currencies
  maxAffordableLevel(currencies, fromLv, maxLv) {
    for (let lv = fromLv + 1; lv <= maxLv; lv++) {
      if (!this.canAffordLevel(currencies, fromLv, lv)) return lv - 1;
    }
    return maxLv;
  }

  // ── Enemy formation lookup ────────────────────────────────────────────────

  // Get enemy formation for a dungeon stage
  dungeonEnemies(dungeonId) {
    const cacheKey = `dungeonEnemies:${dungeonId}`;
    if (this._cache.has(cacheKey)) return this._cache.get(cacheKey);

    // CopyEnemy table: id matches dungeonId, role_id_1..10 = monster IDs
    const db = this._findTable('CopyEnemy');
    if (!db) return null;
    try {
      const row = db.prepare('SELECT * FROM CopyEnemy WHERE id = ?').get(dungeonId);
      if (!row) return null;

      const enemies = [];
      for (let i = 1; i <= 10; i++) {
        const monsterId = row[`role_id_${i}`];
        if (!monsterId || monsterId === 0) continue;
        // Look up monster → hero mapping
        const monster = this._getMonster(monsterId);
        enemies.push({
          slot: i,
          monsterId,
          heroId: monster?.HeroId || 0,
          heroName: monster?.HeroId ? this.heroName(monster.HeroId) : '?',
          post: monster?.HeroId ? (this.heroPost(monster.HeroId)?.Post || 0) : 0,
          fightScore: monster?.FightScore || 0,
        });
      }

      const result = { dungeonId, formation: row.formate, enemies, elite: row.elite || 0 };
      this._cache.set(cacheKey, result);
      return result;
    } catch { return null; }
  }

  _getMonster(monsterId) {
    const db = this._findTable('Monsters');
    if (!db) return null;
    try {
      return db.prepare('SELECT HeroId, FightScore, StarId, HeroSelfLevelId FROM Monsters WHERE id = ?').get(monsterId);
    } catch { return null; }
  }

  // Get dungeon info (chapter, stage, limits, etc.)
  dungeonInfo(dungeonId) {
    const db = this._findTable('MainDungeon');
    if (!db) return null;
    try {
      return db.prepare('SELECT id, Chapter, Stage, Number, NextStageId, LastStageId, Limit_Level, Limit_Score FROM MainDungeon WHERE id = ?').get(dungeonId);
    } catch { return null; }
  }

  // ── Query helpers ────────────────────────────────────────────────────────────

  // Run a raw query on any table
  query(tableName, sql, params = []) {
    const db = this._findTable(tableName);
    if (!db) return [];
    try {
      return db.prepare(sql).all(...params);
    } catch { return []; }
  }

  // Get all rows from a table (with optional limit)
  getAll(tableName, limit = 100) {
    const db = this._findTable(tableName);
    if (!db) return [];
    try {
      return db.prepare(`SELECT * FROM [${tableName}] LIMIT ?`).all(limit);
    } catch { return []; }
  }

  // Get columns info for a table
  getColumns(tableName) {
    const db = this._findTable(tableName);
    if (!db) return [];
    try {
      return db.prepare(`PRAGMA table_info([${tableName}])`).all();
    } catch { return []; }
  }

  // List all tables across all databases
  listTables() {
    const result = [];
    for (const [file, db] of this._dbs) {
      try {
        const tables = db.prepare("SELECT name FROM sqlite_master WHERE type='table'").all();
        for (const t of tables) {
          const count = db.prepare(`SELECT COUNT(*) as c FROM [${t.name}]`).get();
          result.push({ db: file, name: t.name, rows: count.c });
        }
      } catch {}
    }
    return result;
  }

  // Search tables by name pattern
  searchTables(pattern) {
    const p = pattern.toLowerCase();
    return this.listTables().filter(t => t.name.toLowerCase().includes(p));
  }

  // ── Batch resolution ─────────────────────────────────────────────────────────

  // Resolve a map of IDs to names for a given category
  resolveHeroes(ids) {
    const result = {};
    for (const id of ids) result[id] = this.heroName(id);
    return result;
  }

  resolveItems(ids) {
    const result = {};
    for (const id of ids) result[id] = this.itemName(id);
    return result;
  }

  resolveEquipments(ids) {
    const result = {};
    for (const id of ids) result[id] = this.equipName(id);
    return result;
  }

  // ── Lifecycle ────────────────────────────────────────────────────────────────

  close() {
    for (const db of this._dbs.values()) {
      try { db.close(); } catch {}
    }
    this._dbs.clear();
    this._cache.clear();
  }

  reload() {
    this.close();
    this._open();
  }
}

// Singleton
let instance = null;
export function getGameDB() {
  if (!instance) instance = new GameDB();
  return instance;
}
