// Persistent shared mining map — accumulates grid data across all bot runs
// Every account contributes to the same map; future runs use the full picture
// File: data/mining_map.json

import { readFileSync, writeFileSync, existsSync } from 'fs';
import { dirname, join } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const MAP_PATH = join(__dirname, '..', 'data', 'mining_map.json');

// ── In-memory map ──────────────────────────────────────────────────────────

let map = null;

function loadMap() {
  if (map) return map;
  try {
    if (existsSync(MAP_PATH)) {
      map = JSON.parse(readFileSync(MAP_PATH, 'utf-8'));
    }
  } catch {}
  if (!map) map = { activityId: 0, maxFloor: 0, floors: {}, contributors: [], lastUpdate: null };
  return map;
}

function saveMap() {
  if (!map) return;
  map.lastUpdate = new Date().toISOString();
  try {
    writeFileSync(MAP_PATH, JSON.stringify(map, null, 2));
  } catch (e) {
    console.error('[mining_map] Save failed:', e.message);
  }
}

// ── Type helpers ───────────────────────────────────────────────────────────

function typeCost(typeId) {
  if (typeId <= 0) return 0;
  const mod = typeId <= 9 ? ((typeId - 1) % 3) : ((typeId % 10 - 1) % 3);
  return [0, 1, 2][mod];
}

function typeName(typeId) {
  const cost = typeCost(typeId);
  return ['empty', 'grass', 'rock'][cost];
}

function isValuableReward(rewardId) {
  if (!rewardId || rewardId <= 0) return false;
  const mod = rewardId % 10;
  return (mod === 5 || mod === 6 || mod === 7 || mod === 8);
}

function rewardName(rewardId) {
  if (!rewardId || rewardId <= 0) return null;
  const mod = rewardId % 10;
  if (mod === 1) return 'star5';
  if (mod === 2) return 'star10';
  if (mod === 3) return 'star20';
  if (mod === 4) return 'goldstar';
  if (mod === 5 || mod === 7) return 'bomb';
  if (mod === 6 || mod === 8) return 'rocket';
  return 'unknown';
}

// ── Recording grid data ────────────────────────────────────────────────────

/**
 * Record grid data from SCAvMiningGame into the persistent map.
 * @param {number} floor - current floor number
 * @param {Array} grids - grid cells from server (DMiningGrid objects)
 * @param {Array} judgGrids - cleared cells from server
 * @param {string} contributor - account identifier
 */
export function recordGridData(floor, grids, judgGrids, contributor) {
  const m = loadMap();

  // Process each cell — keyed by absolute x coordinate
  // A "floor" in our map = one column (x value)
  for (const g of grids) {
    const colKey = String(g.x);
    if (!m.floors[colKey]) {
      m.floors[colKey] = { cells: {}, discovered: new Date().toISOString(), by: contributor };
    }
    const col = m.floors[colKey];
    const yKey = String(g.y);

    // Only update if we have new info (don't overwrite with stale data)
    const existing = col.cells[yKey];
    const cell = {
      type: g.type,
      cost: typeCost(g.type),
      typeName: typeName(g.type),
      rewardId: g.rewardId || 0,
      rewardName: rewardName(g.rewardId),
      clicked: g.clickNum > 0,
      valuable: isValuableReward(g.rewardId),
    };

    if (!existing || !existing.clicked) {
      col.cells[yKey] = cell;
    }
  }

  // Also record judg (cleared) cells
  for (const j of judgGrids) {
    const colKey = String(j.x);
    if (!m.floors[colKey]) {
      m.floors[colKey] = { cells: {}, discovered: new Date().toISOString(), by: contributor };
    }
    const yKey = String(j.y);
    if (!m.floors[colKey].cells[yKey]) {
      m.floors[colKey].cells[yKey] = {
        type: j.type,
        cost: typeCost(j.type),
        typeName: typeName(j.type),
        rewardId: j.rewardId || 0,
        rewardName: rewardName(j.rewardId),
        clicked: true,
        valuable: isValuableReward(j.rewardId),
      };
    } else {
      m.floors[colKey].cells[yKey].clicked = true;
    }
  }

  // Update max floor
  const allX = Object.keys(m.floors).map(Number);
  m.maxFloor = Math.max(m.maxFloor, ...allX);

  // Track contributor
  if (contributor && !m.contributors.includes(contributor)) {
    m.contributors.push(contributor);
  }

  saveMap();
}

// ── Querying the map ───────────────────────────────────────────────────────

/**
 * Get the full known map for pathfinding
 * @returns {{ columns: {[x: string]: {[y: string]: cell}}, maxFloor: number }}
 */
export function getFullMap() {
  const m = loadMap();
  const columns = {};
  for (const [xKey, col] of Object.entries(m.floors)) {
    columns[xKey] = col.cells;
  }
  return { columns, maxFloor: m.maxFloor, contributors: m.contributors };
}

/**
 * Compute optimal path from startFloor to targetFloor using known map data.
 * Uses Dijkstra across known columns.
 * @param {number} startX - leftmost known cleared x
 * @param {number} targetX - how far to plan
 * @returns {{ path: [{x,y,cost,reward}], totalCost, reachable, unknownFrom }}
 */
export function computeOptimalPath(startX, targetX) {
  const m = loadMap();
  const GRID_H = 6;

  // Build cell lookup
  function getCell(x, y) {
    const col = m.floors[String(x)];
    if (!col) return null;
    return col.cells[String(y)] || null;
  }

  // Find cleared cells for starting adjacency
  const cleared = new Set();
  for (const [xKey, col] of Object.entries(m.floors)) {
    for (const [yKey, cell] of Object.entries(col.cells)) {
      if (cell.clicked) cleared.add(`${xKey},${yKey}`);
    }
  }

  // Dijkstra
  const dist = {};
  const prev = {};
  const pq = [];

  // Seed: all unclicked cells adjacent to cleared cells
  for (const [xKey, col] of Object.entries(m.floors)) {
    for (const [yKey, cell] of Object.entries(col.cells)) {
      const x = parseInt(xKey), y = parseInt(yKey);
      if (cell.clicked) continue;
      if (x < startX) continue;
      const key = `${x},${y}`;
      const neighbors = [[x-1,y],[x+1,y],[x,y-1],[x,y+1]];
      if (neighbors.some(([nx,ny]) => ny >= 0 && ny < GRID_H && cleared.has(`${nx},${ny}`))) {
        let adjCost = cell.cost;
        if (cell.valuable) adjCost = Math.max(0, adjCost - 2);
        dist[key] = adjCost;
        prev[key] = null;
        pq.push({ key, cost: adjCost });
      }
    }
  }

  pq.sort((a, b) => a.cost - b.cost);
  const visited = new Set();

  while (pq.length > 0) {
    const { key, cost } = pq.shift();
    if (visited.has(key)) continue;
    visited.add(key);

    const [x, y] = key.split(',').map(Number);
    if (x >= targetX) continue; // don't explore beyond target

    const neighbors = [[x-1,y],[x+1,y],[x,y-1],[x,y+1]];
    for (const [nx, ny] of neighbors) {
      if (ny < 0 || ny >= GRID_H) continue;
      const nk = `${nx},${ny}`;
      if (visited.has(nk) || cleared.has(nk)) continue;
      const ncell = getCell(nx, ny);
      if (!ncell) continue; // unknown territory
      let adjCost = ncell.cost;
      if (ncell.valuable) adjCost = Math.max(0, adjCost - 2);
      const totalCost = cost + adjCost;
      if (!(nk in dist) || totalCost < dist[nk]) {
        dist[nk] = totalCost;
        prev[nk] = key;
        pq.push({ key: nk, cost: totalCost });
        pq.sort((a, b) => a.cost - b.cost);
      }
    }
  }

  // Find cheapest target at each x from startX to targetX
  const pathByFloor = [];
  let lastReachable = startX;

  for (let tx = startX + 1; tx <= targetX; tx++) {
    let bestY = -1, bestCost = Infinity;
    for (let y = 0; y < GRID_H; y++) {
      const key = `${tx},${y}`;
      if (key in dist && dist[key] < bestCost) {
        bestCost = dist[key];
        bestY = y;
      }
    }
    if (bestY >= 0) {
      lastReachable = tx;
      pathByFloor.push({ x: tx, y: bestY, cost: bestCost });
    } else {
      break; // unknown territory
    }
  }

  // Reconstruct detailed path to the last reachable floor
  if (pathByFloor.length === 0) return { path: [], totalCost: 0, reachable: startX, unknownFrom: startX + 1 };

  const target = pathByFloor[pathByFloor.length - 1];
  const targetKey = `${target.x},${target.y}`;
  const detailedPath = [];
  let cur = targetKey;
  while (cur) {
    const [cx, cy] = cur.split(',').map(Number);
    const cell = getCell(cx, cy);
    detailedPath.unshift({
      x: cx, y: cy,
      cost: cell ? cell.cost : 0,
      reward: cell?.rewardName,
      valuable: cell?.valuable,
      typeName: cell?.typeName,
    });
    cur = prev[cur];
  }

  return {
    path: detailedPath,
    totalCost: target.cost,
    reachable: lastReachable,
    unknownFrom: lastReachable + 1,
    summary: {
      floors: pathByFloor.length,
      shovels: target.cost,
      efficiency: (pathByFloor.length / Math.max(1, target.cost)).toFixed(2),
      valuableOnPath: detailedPath.filter(p => p.valuable).length,
    },
  };
}

/**
 * Get map stats for dashboard
 */
export function getMapStats() {
  const m = loadMap();
  const allX = Object.keys(m.floors).map(Number).sort((a, b) => a - b);
  const totalCells = Object.values(m.floors).reduce((sum, col) => sum + Object.keys(col.cells).length, 0);
  const clickedCells = Object.values(m.floors).reduce((sum, col) =>
    sum + Object.values(col.cells).filter(c => c.clicked).length, 0);
  const valuableCells = Object.values(m.floors).reduce((sum, col) =>
    sum + Object.values(col.cells).filter(c => c.valuable && !c.clicked).length, 0);

  return {
    maxFloor: m.maxFloor,
    columnsKnown: allX.length,
    totalCells,
    clickedCells,
    valuableCells,
    contributors: m.contributors,
    xRange: allX.length > 0 ? [allX[0], allX[allX.length - 1]] : [0, 0],
    lastUpdate: m.lastUpdate,
  };
}

/**
 * Get column data for dashboard visualization
 * @param {number} fromX
 * @param {number} toX
 */
export function getMapColumns(fromX, toX) {
  const m = loadMap();
  const result = {};
  for (let x = fromX; x <= toX; x++) {
    const col = m.floors[String(x)];
    if (col) result[x] = col.cells;
  }
  return result;
}

export { typeCost, typeName, isValuableReward, rewardName };
