// Hot-reload manager — watches src/, proto/, data/, web/ and reloads on change
import { watch, statSync } from 'fs';
import { join, relative, extname, dirname } from 'path';
import { pathToFileURL, fileURLToPath } from 'url';
import { EventEmitter } from 'events';

const __dirname = dirname(fileURLToPath(import.meta.url));
const ROOT = join(__dirname, '..');

export class HotReloader extends EventEmitter {
  constructor() {
    super();
    this.version = 0;
    this.modules = {};    // name → latest module exports
    this._watchers = [];
    this._debounce = new Map(); // path → timer
  }

  // ── Dynamic import with cache busting ──────────────────────────────────────

  async load(name, relPath) {
    const absPath = join(ROOT, relPath);
    const url = pathToFileURL(absPath).href + '?v=' + Date.now();
    const mod = await import(url);
    this.modules[name] = mod;
    return mod;
  }

  get(name) {
    return this.modules[name];
  }

  // ── File watching ──────────────────────────────────────────────────────────

  start() {
    const dirs = [
      { dir: join(ROOT, 'src'), type: 'src' },
      { dir: join(ROOT, 'proto'), type: 'proto' },
      { dir: join(ROOT, 'data'), type: 'data' },
      { dir: join(ROOT, 'web'), type: 'web' },
    ];

    for (const { dir, type } of dirs) {
      try {
        const w = watch(dir, { recursive: false }, (event, filename) => {
          if (!filename) return;
          const key = type + '/' + filename;

          // Debounce — multiple events fire for one save
          if (this._debounce.has(key)) clearTimeout(this._debounce.get(key));
          this._debounce.set(key, setTimeout(() => {
            this._debounce.delete(key);
            this._onChange(type, filename);
          }, 200));
        });
        this._watchers.push(w);
      } catch {}
    }

    console.log('\x1b[35m[hot]\x1b[0m Watching for changes...');
  }

  async _onChange(type, filename) {
    this.version++;
    const ext = extname(filename);
    console.log(`\x1b[35m[hot]\x1b[0m ${type}/${filename} changed (v${this.version})`);

    if (type === 'src' && ext === '.js') {
      // Reload the changed JS module
      const name = filename.replace('.js', '');
      try {
        await this.load(name, `src/${filename}`);
        console.log(`\x1b[35m[hot]\x1b[0m Reloaded module: ${name}`);
        this.emit('module', { name, filename });
      } catch (e) {
        console.error(`\x1b[31m[hot]\x1b[0m Failed to reload ${name}: ${e.message}`);
      }
    }

    if (type === 'proto' || type === 'data') {
      this.emit('protocol', { filename });
    }

    if (type === 'web') {
      this.emit('web', { filename });
    }

    // Always emit generic change event
    this.emit('change', { type, filename, version: this.version });
  }

  stop() {
    for (const w of this._watchers) w.close();
    this._watchers = [];
  }
}

// Singleton
let instance = null;
export function getHotReloader() {
  if (!instance) instance = new HotReloader();
  return instance;
}
