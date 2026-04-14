// Mining event auto-digger — optimal pathfinding with shared map intelligence
// Grid: 8 columns × 6 rows per floor window
// Types: GridType 1=empty(0 clicks), 2=grass(1 click), 3=rock(2 clicks)
// Empty cells are fog passthrough — never click, auto-cleared
// Rocks need 2 clicks (cost=2), remaining = cost - clickNum

import { recordGridData, getMapStats, getMapColumns, rewardName } from './mining_map.js';

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

const GRID_H = 6;

// ── Type helpers ───────────────────────────────────────────────────────────

function typeCost(typeId) {
  if (typeId <= 0) return 0;
  const mod = typeId <= 9 ? ((typeId - 1) % 3) : ((typeId % 10 - 1) % 3);
  return [0, 1, 2][mod];
}

function isValuableReward(rewardId) {
  if (!rewardId || rewardId <= 0) return false;
  const mod = rewardId % 10;
  return (mod === 5 || mod === 6 || mod === 7 || mod === 8);
}

// ── Inventory ──────────────────────────────────────────────────────────────

const SHOVEL_IDS = [1051603,1051606,1051609,1051612,1051615,1051618,1051621,1051624,1051627,1051630,1051633,1051636,1051639,1051642,1051645,1051648,1051651,1051654,1051657,1051660];
const BOMB_IDS = [1051601,1051604,1051607,1051610,1051613,1051616,1051619,1051622,1051625,1051628,1051631,1051634,1051637,1051640,1051643,1051646,1051649,1051652,1051655,1051658];
const ROCKET_IDS = [1051602,1051605,1051608,1051611,1051614,1051617,1051620,1051623,1051626,1051629,1051632,1051635,1051638,1051641,1051644,1051647,1051650,1051653,1051656,1051659];

function getInventory(items) {
  let shovels = 0, bombs = 0, rockets = 0, shovelId = 0, bombId = 0, rocketId = 0;
  for (const item of items) {
    const k = item.iKey || item.key;
    const v = item.iValue || item.value || 0;
    if (SHOVEL_IDS.includes(k)) { shovels += v; shovelId = k; }
    if (BOMB_IDS.includes(k)) { bombs += v; bombId = k; }
    if (ROCKET_IDS.includes(k)) { rockets += v; rocketId = k; }
  }
  return { shovels, bombs, rockets, shovelId, bombId, rocketId };
}

// ── Grid building ──────────────────────────────────────────────────────────

function buildGrid(grids, judgGrids) {
  const cells = new Map();   // grass/rock cells that need clicking
  const cleared = new Set();  // fully cleared cells (fog passthrough)
  let maxX = -Infinity;

  // JudgGrid = server-confirmed cleared cells
  // Only use judgGrid cells within the current grid window (ignore ancient ones)
  const gridXs = grids.map(g => g.x);
  const gridMinX = gridXs.length > 0 ? Math.min(...gridXs) : 0;
  for (const j of judgGrids) {
    if (j.x >= gridMinX - 1) { // only cells at or near the current window
      cleared.add(`${j.x},${j.y}`);
    }
  }

  for (const g of grids) {
    const cost = typeCost(g.type);
    if (g.x > maxX) maxX = g.x;
    const remaining = cost - (g.clickNum || 0);

    if (remaining <= 0) {
      // Fully cleared: empty (cost=0) OR clicked enough times
      cleared.add(`${g.x},${g.y}`);
    } else {
      // Needs more clicks — remaining is the actual cost left
      cells.set(`${g.x},${g.y}`, {
        x: g.x, y: g.y, type: g.type,
        cost: remaining,        // shovels still needed
        fullCost: cost,         // original cost
        rewardId: g.rewardId || 0,
        valuable: isValuableReward(g.rewardId),
      });
    }
  }

  return { cells, cleared, maxX };
}

// ── Pathfinding ────────────────────────────────────────────────────────────

function isAdjacentToCleared(x, y, cleared) {
  return [[x-1,y],[x+1,y],[x,y-1],[x,y+1]].some(
    ([nx, ny]) => ny >= 0 && ny < GRID_H && cleared.has(`${nx},${ny}`)
  );
}

/**
 * Compute chain-advance potential: how many floors auto-advance if you arrive
 * at row `arrivalY` after clearing maxX. Uses shared map data for lookahead.
 * Chain advances happen when the NEXT column(s) have empty cells reachable
 * from the arrival row (±1 per column).
 */
function computeChainAdvance(maxX, arrivalY) {
  const mapData = getMapColumns(maxX + 1, maxX + 50); // look ahead up to 50 floors
  let chains = 0;
  let currentRows = new Set([arrivalY]);

  for (let x = maxX + 1; x <= maxX + 50; x++) {
    const col = mapData[x];
    if (!col) break; // unknown territory — can't predict

    // Check if any reachable row (±1 from current) has an empty cell
    const nextRows = new Set();
    let hasEmpty = false;

    for (const cy of currentRows) {
      for (let dy = -1; dy <= 1; dy++) {
        const ny = cy + dy;
        if (ny < 0 || ny >= GRID_H) continue;
        const cell = col[String(ny)];
        if (cell && typeCost(cell.type) === 0) {
          // Empty cell reachable — chain advance!
          hasEmpty = true;
          nextRows.add(ny);
        }
      }
    }

    if (!hasEmpty) break; // blocked — no more chaining
    chains++;
    currentRows = nextRows;
  }

  return chains;
}

/**
 * Dijkstra: cheapest path from cleared frontier to maxX.
 * Chain-aware: when multiple paths have the same cost, pick the row at maxX
 * that maximizes chain-advance potential using the shared map.
 * Returns { clicks: [{x,y,cost,...}], totalCost, reached, chainAdvance }
 * clicks only contains cells with cost > 0 (grass/rock to actually click)
 */
function findPath(gridData) {
  const { cells, cleared, maxX } = gridData;

  if (cells.size === 0) return { clicks: [], totalCost: 0, reached: true, chainAdvance: 0 };

  const dist = {};
  const prev = {};
  const arrivalY = {}; // track which row we arrive at for each cell
  const pq = [];

  // Seed: all cells adjacent to cleared
  for (const [key, cell] of cells) {
    if (isAdjacentToCleared(cell.x, cell.y, cleared)) {
      let ac = cell.cost;
      if (cell.valuable) ac = Math.max(0, ac - 2);
      dist[key] = ac;
      prev[key] = null;
      arrivalY[key] = cell.y;
      pq.push({ key, cost: ac });
    }
  }

  pq.sort((a, b) => a.cost - b.cost);
  const visited = new Set();

  while (pq.length > 0) {
    const { key, cost } = pq.shift();
    if (visited.has(key)) continue;
    visited.add(key);

    const [x, y] = key.split(',').map(Number);

    for (const [nx, ny] of [[x-1,y],[x+1,y],[x,y-1],[x,y+1]]) {
      if (ny < 0 || ny >= GRID_H) continue;
      const nk = `${nx},${ny}`;
      if (visited.has(nk) || cleared.has(nk)) continue;

      const ncell = cells.get(nk);
      if (!ncell) continue;

      let ac = ncell.cost;
      if (ncell.valuable) ac = Math.max(0, ac - 2);
      const tc = cost + ac;

      if (!(nk in dist) || tc < dist[nk]) {
        dist[nk] = tc;
        prev[nk] = key;
        arrivalY[nk] = ny;
        pq.push({ key: nk, cost: tc });
        pq.sort((a, b) => a.cost - b.cost);
      }
    }
  }

  // Collect all candidates at maxX and maxX-1 (indirect via empty at maxX)
  const candidates = []; // {key, cost, y, chainAdvance}

  for (let y = 0; y < GRID_H; y++) {
    // Direct: cell at maxX
    const k = `${maxX},${y}`;
    if (k in dist) {
      const chain = computeChainAdvance(maxX, y);
      candidates.push({ key: k, cost: dist[k], y, chainAdvance: chain });
    }

    // Indirect: cell at maxX-1 exposing empty at maxX
    const k2 = `${maxX - 1},${y}`;
    if (k2 in dist) {
      for (let dy = -1; dy <= 1; dy++) {
        const ny = y + dy;
        if (ny < 0 || ny >= GRID_H) continue;
        if (cleared.has(`${maxX},${ny}`)) {
          const chain = computeChainAdvance(maxX, ny);
          candidates.push({ key: k2, cost: dist[k2], y: ny, chainAdvance: chain });
        }
      }
    }
  }

  if (candidates.length === 0) return { clicks: [], totalCost: 0, reached: false, chainAdvance: 0 };

  // Pick best: minimize (cost - chainAdvance). More chains = more value per shovel.
  // If costs are equal, pick the one with more chain advances.
  // If a path costs 1 more shovel but chains 3+ more floors, prefer it.
  candidates.sort((a, b) => {
    const scoreA = a.cost - a.chainAdvance * 0.9; // each chain saves ~1 shovel
    const scoreB = b.cost - b.chainAdvance * 0.9;
    return scoreA - scoreB;
  });

  const best = candidates[0];

  // Reconstruct path
  const fullPath = [];
  let cur = best.key;
  while (cur) {
    const cell = cells.get(cur);
    if (cell) fullPath.unshift(cell);
    cur = prev[cur];
  }

  const clickPath = fullPath.filter(c => c.cost > 0);
  return {
    clicks: clickPath,
    totalCost: best.cost,
    reached: true,
    chainAdvance: best.chainAdvance,
    arrivalRow: best.y,
  };
}

// ── Bomb/Rocket targeting ──────────────────────────────────────────────────

function findBestBombTarget(gridData) {
  const { cells, cleared } = gridData;
  let bestSaving = 0, bestPos = null;

  for (const key of cleared) {
    const [cx, cy] = key.split(',').map(Number);
    let saving = 0;
    for (let dx = -1; dx <= 1; dx++) {
      for (let dy = -1; dy <= 1; dy++) {
        const nk = `${cx+dx},${cy+dy}`;
        const cell = cells.get(nk);
        if (cell && !cleared.has(nk)) {
          saving += cell.cost;
          if (cell.valuable) saving += 3;
        }
      }
    }
    if (saving > bestSaving) { bestSaving = saving; bestPos = { x: cx, y: cy, saving }; }
  }
  return bestPos;
}

function findBestRocketTarget(gridData) {
  const { cells, cleared, maxX } = gridData;
  let bestSaving = 0, bestTarget = null;

  for (let y = 0; y < GRID_H; y++) {
    let saving = 0, placeX = -1, reachesMax = false;
    for (const key of cleared) {
      const [cx, cy] = key.split(',').map(Number);
      if (cy === y) placeX = cx;
    }
    if (placeX < 0) continue;

    for (const [key, cell] of cells) {
      if (cell.y === y) {
        saving += cell.cost;
        if (cell.x === maxX || cell.x === maxX - 1) reachesMax = true;
      }
    }

    const score = reachesMax ? saving + 100 : saving;
    if (score > bestSaving) {
      bestSaving = score;
      bestTarget = { x: placeX, y, saving, reachesMax };
    }
  }
  return bestTarget;
}

// ── Send + refresh helper ──────────────────────────────────────────────────

async function sendAndRefresh(acc, activityId, sendName, sendData, delayMs) {
  acc.client.send(sendName, sendData);
  await sleep(delayMs + 200);

  // Re-query full game state
  acc.client.send('CSOpenActivityUI', {
    baseActivityId: [activityId], openActivityId: [activityId],
    privilegeCard: false, diamondShop: false,
  });

  const refresh = await acc.client.waitFor('SCAvMiningGame', 5000);
  return refresh?.data || null;
}

// ── Main loop ──────────────────────────────────────────────────────────────

export async function autoMine(acc, opts = {}) {
  const activityId = opts.activityId || 10512002;
  const delayMs = opts.delayMs || 400;
  const maxClicks = opts.maxClicks || 10000;
  let clickCount = 0, startFloor = 0;

  acc._log(`[mining] Starting auto-miner (activity=${activityId})`);
  acc._mining = true;

  try {
    // Open activity
    acc.client.send('CSOpenActivityUI', {
      baseActivityId: [activityId], openActivityId: [activityId],
      privilegeCard: false, diamondShop: false,
    });
    const gameResp = await acc.client.waitFor('SCAvMiningGame', 5000);
    if (!gameResp?.data) { acc._log('[mining] Failed to get game data'); return; }

    let data = gameResp.data;
    let floor = data.floor || 0;
    startFloor = floor;
    let grids = data.grids || data.gridsList || [];
    let judgGrids = data.judgGrid || data.judgGridList || [];
    let items = data.item || data.itemList || [];
    let inv = getInventory(items);
    const contributor = acc.email || acc.roleRid || 'unknown';

    acc._log(`[mining] Floor ${floor} | ${inv.shovels} shovels, ${inv.bombs} bombs, ${inv.rockets} rockets`);
    recordGridData(floor, grids, judgGrids, contributor);
    acc.emit('mining', { floor, inv, grids, judgGrids, action: 'init' });

    const failedCells = new Set();
    let consecutiveFails = 0;

    while (acc._mining && acc.state === 'online' && clickCount < maxClicks) {
      // Filter out known-bad cells
      const filteredGrids = grids.filter(g => !failedCells.has(`${g.x},${g.y}`));
      const gridData = buildGrid(filteredGrids, judgGrids);

      // Compute cheapest dig path
      const pathResult = findPath(gridData);
      const digCost = pathResult.totalCost;

      // ── Decision: detour for valuable reward? ──
      // Find cheap valuable cells (bomb/rocket under grass = net positive)
      let detour = null;
      for (const [key, cell] of gridData.cells) {
        if (!cell.valuable || cell.cost > 1) continue;
        if (!isAdjacentToCleared(cell.x, cell.y, gridData.cleared)) continue;
        if (failedCells.has(key)) continue;
        detour = cell;
        break;
      }

      if (detour && detour.cost <= 1 && inv.shovels > 0) {
        acc._log(`[mining] Detour: (${detour.x},${detour.y}) cost=${detour.cost} → ${rewardName(detour.rewardId)}`);
        const refreshData = await sendAndRefresh(acc, activityId, 'CSAvMiningClickGrid',
          { activityId, x: detour.x, y: detour.y }, delayMs);

        if (refreshData) {
          if (refreshData.floor > floor) {
            floor = refreshData.floor;
            acc._log(`[mining] Floor ${floor}!`);
            failedCells.clear();
          }
          // Check if click worked
          const newGrids = refreshData.grids || refreshData.gridsList || [];
          const dc = newGrids.find(g => g.x === detour.x && g.y === detour.y);
          if (dc && typeCost(dc.type) - (dc.clickNum||0) <= 0) {
            // Worked — cell is now cleared
            consecutiveFails = 0;
          } else if (dc && (dc.clickNum||0) > 0) {
            // Partially clicked (rock) — progress made
            consecutiveFails = 0;
          } else if (refreshData.floor === floor) {
            failedCells.add(`${detour.x},${detour.y}`);
            consecutiveFails++;
          }
          grids = newGrids;
          items = refreshData.item || refreshData.itemList || items;
          inv = getInventory(items);
          judgGrids = refreshData.judgGrid || refreshData.judgGridList || judgGrids;
          clickCount++;
          recordGridData(floor, grids, judgGrids, contributor);
          acc.emit('mining', { floor, inv, grids, judgGrids, action: 'dig', x: detour.x, y: detour.y, cost: detour.cost });
        }
        continue;
      }

      // ── Decision: use rocket? ──
      if (inv.rockets > 0 && digCost >= 4) {
        const rocketTarget = findBestRocketTarget(gridData);
        if (rocketTarget && rocketTarget.reachesMax && rocketTarget.saving >= digCost + 3) {
          acc._log(`[mining] Rocket (${rocketTarget.x},${rocketTarget.y}) saves ${rocketTarget.saving} vs dig ${digCost}`);
          const refreshData = await sendAndRefresh(acc, activityId, 'CSAvMiningUseSkill',
            { activityId, x: rocketTarget.x, y: rocketTarget.y, itemId: inv.rocketId }, delayMs);
          if (refreshData) {
            if (refreshData.floor > floor) { floor = refreshData.floor; acc._log(`[mining] Floor ${floor}!`); failedCells.clear(); }
            grids = refreshData.grids || refreshData.gridsList || [];
            items = refreshData.item || refreshData.itemList || items;
            inv = getInventory(items);
            judgGrids = refreshData.judgGrid || refreshData.judgGridList || judgGrids;
            clickCount++;
            recordGridData(floor, grids, judgGrids, contributor);
            acc.emit('mining', { floor, inv, grids, judgGrids, action: 'rocket', x: rocketTarget.x, y: rocketTarget.y });
          }
          continue;
        }
      }

      // ── Decision: use bomb? ──
      if (inv.bombs > 0 && digCost >= 4) {
        const bombTarget = findBestBombTarget(gridData);
        if (bombTarget && bombTarget.saving >= digCost + 3) {
          acc._log(`[mining] Bomb (${bombTarget.x},${bombTarget.y}) saves ${bombTarget.saving} vs dig ${digCost}`);
          const refreshData = await sendAndRefresh(acc, activityId, 'CSAvMiningUseSkill',
            { activityId, x: bombTarget.x, y: bombTarget.y, itemId: inv.bombId }, delayMs);
          if (refreshData) {
            if (refreshData.floor > floor) { floor = refreshData.floor; acc._log(`[mining] Floor ${floor}!`); failedCells.clear(); }
            grids = refreshData.grids || refreshData.gridsList || [];
            items = refreshData.item || refreshData.itemList || items;
            inv = getInventory(items);
            judgGrids = refreshData.judgGrid || refreshData.judgGridList || judgGrids;
            clickCount++;
            recordGridData(floor, grids, judgGrids, contributor);
            acc.emit('mining', { floor, inv, grids, judgGrids, action: 'bomb', x: bombTarget.x, y: bombTarget.y });
          }
          continue;
        }
      }

      // ── Decision: dig ──
      if (pathResult.clicks.length === 0) {
        if (pathResult.reached) {
          acc._log(`[mining] All cells cleared, nothing to dig`);
        } else {
          acc._log(`[mining] No path found (${failedCells.size} failed, ${gridData.cells.size} cells, ${gridData.cleared.size} cleared)`);
        }
        break;
      }

      const click = pathResult.clicks[0];

      // Out of shovels?
      if (inv.shovels <= 0) {
        // Check if bombs/rockets can push to next shovel milestone
        const SHOVEL_FLOORS = [10,30,50,70,90,115,145,175,205,235,265,295,325,355,385,
          420,460,500,540,580,620,660,700,740,780,825,875,925,975,1025,
          1075,1125,1175,1225,1275,1325,1375,1425,1475,1530,1590,1650,
          1710,1770,1830,1890,1950,2010,2070,2130,2190,2250,2310,2370,2430,2490];
        const nextShovel = SHOVEL_FLOORS.find(f => f > floor);
        const dist = nextShovel ? nextShovel - floor : Infinity;

        if ((inv.bombs > 0 || inv.rockets > 0) && dist <= 10) {
          acc._log(`[mining] No shovels, using tools to reach milestone ${nextShovel} (${dist} away)`);
          if (inv.rockets > 0) {
            const rt = findBestRocketTarget(gridData);
            if (rt) {
              const rd = await sendAndRefresh(acc, activityId, 'CSAvMiningUseSkill',
                { activityId, x: rt.x, y: rt.y, itemId: inv.rocketId }, delayMs);
              if (rd) {
                if (rd.floor > floor) { floor = rd.floor; failedCells.clear(); }
                grids = rd.grids || rd.gridsList || [];
                items = rd.item || rd.itemList || items;
                inv = getInventory(items);
                judgGrids = rd.judgGrid || rd.judgGridList || judgGrids;
                clickCount++;
                recordGridData(floor, grids, judgGrids, contributor);
                acc.emit('mining', { floor, inv, grids, judgGrids, action: 'rocket' });
              }
              continue;
            }
          }
          if (inv.bombs > 0) {
            const bt = findBestBombTarget(gridData);
            if (bt) {
              const rd = await sendAndRefresh(acc, activityId, 'CSAvMiningUseSkill',
                { activityId, x: bt.x, y: bt.y, itemId: inv.bombId }, delayMs);
              if (rd) {
                if (rd.floor > floor) { floor = rd.floor; failedCells.clear(); }
                grids = rd.grids || rd.gridsList || [];
                items = rd.item || rd.itemList || items;
                inv = getInventory(items);
                judgGrids = rd.judgGrid || rd.judgGridList || judgGrids;
                clickCount++;
                recordGridData(floor, grids, judgGrids, contributor);
                acc.emit('mining', { floor, inv, grids, judgGrids, action: 'bomb' });
              }
              continue;
            }
          }
        }
        acc._log(`[mining] Out of shovels (floor ${floor})`);
        break;
      }

      // Adjacency safety check
      if (!isAdjacentToCleared(click.x, click.y, gridData.cleared)) {
        acc._log(`[mining] BUG: (${click.x},${click.y}) not adjacent! Skipping.`);
        failedCells.add(`${click.x},${click.y}`);
        continue;
      }

      // Click!
      const chainInfo = pathResult.chainAdvance ? ` chain=${pathResult.chainAdvance} row=${pathResult.arrivalRow}` : '';
      acc._log(`[mining] Dig (${click.x},${click.y}) cost=${click.cost} path=${pathResult.clicks.length} total=${digCost}${chainInfo}`);
      const refreshData = await sendAndRefresh(acc, activityId, 'CSAvMiningClickGrid',
        { activityId, x: click.x, y: click.y }, delayMs);

      if (refreshData) {
        if (refreshData.floor > floor) {
          const jump = refreshData.floor - floor;
          floor = refreshData.floor;
          acc._log(`[mining] Floor ${floor} (+${jump})!`);
          failedCells.clear();
          consecutiveFails = 0;
        }

        const newGrids = refreshData.grids || refreshData.gridsList || [];

        // Verify click worked
        const clickedCell = newGrids.find(g => g.x === click.x && g.y === click.y);
        if (clickedCell) {
          const newRemaining = typeCost(clickedCell.type) - (clickedCell.clickNum || 0);
          if (newRemaining < click.cost) {
            // Progress made (clickNum increased)
            consecutiveFails = 0;
          } else if (refreshData.floor === floor) {
            // No progress — rejected
            failedCells.add(`${click.x},${click.y}`);
            acc._log(`[mining] Click (${click.x},${click.y}) rejected (${failedCells.size} failed)`);
            consecutiveFails++;
          }
        } else {
          // Cell gone from grid — it was fully cleared or floor advanced
          consecutiveFails = 0;
        }

        grids = newGrids;
        items = refreshData.item || refreshData.itemList || items;
        inv = getInventory(items);
        judgGrids = refreshData.judgGrid || refreshData.judgGridList || judgGrids;
        clickCount++;
        recordGridData(floor, grids, judgGrids, contributor);
        acc.emit('mining', { floor, inv, grids, judgGrids, action: 'dig', x: click.x, y: click.y, cost: click.cost });
      } else {
        acc._log(`[mining] No refresh data, stopping`);
        break;
      }

      if (consecutiveFails >= 5) {
        acc._log(`[mining] 5 consecutive failures, stopping`);
        break;
      }
    }

    // Claim milestone rewards
    acc._log(`[mining] Claiming rewards...`);
    for (let i = 0; i < 50; i++) {
      try {
        acc.client.send('CSAvMiningProgressReward', { activityId });
        await acc.client.waitFor('SCAvMiningProgressReward', 2000);
        acc._log(`[mining] Milestone reward claimed`);
        await sleep(200);
      } catch { break; }
    }
    for (let i = 1; i <= 20; i++) {
      try {
        acc.client.send('CSAvMiningTaskReward', { activityId, taskId: i });
        await acc.client.waitFor('SCAvMiningTaskReward', 2000);
        acc._log(`[mining] Task reward #${i} claimed`);
        await sleep(200);
      } catch { break; }
    }

    const floorsAdvanced = floor - startFloor;
    const efficiency = clickCount > 0 ? (floorsAdvanced / clickCount).toFixed(2) : 'N/A';
    acc._log(`[mining] Done: floor ${startFloor}→${floor} (+${floorsAdvanced}), ${clickCount} clicks, efficiency=${efficiency}`);
    acc.emit('mining', { floor, inv, grids, judgGrids, action: 'done' });
    return { startFloor, finalFloor: floor, floorsAdvanced, clickCount, efficiency, inv };

  } catch (e) {
    acc._log(`[mining] Error: ${e.message}`);
    return { error: e.message };
  } finally {
    acc._mining = false;
  }
}

export function stopMining(acc) {
  acc._mining = false;
  acc._log('[mining] Stopped');
}

export { getInventory, buildGrid, findPath, findBestBombTarget, findBestRocketTarget };
