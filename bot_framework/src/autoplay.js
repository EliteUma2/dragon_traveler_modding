// Auto-play engine — hot-reloadable module for automated game progression
// Strategy: advance main story, level heroes, equip gear, pull gacha, optimize formation
// Smart lineup: 1 hero per class (Post), highest star/quality, auto-replace + reset old
import { getGameDB } from './gamedb.js';
import { claimAll } from './claims.js';

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// Static operates blob — hardcoded fake battle data (matches game plugin)
const OPERATES_HEX = '20000000c609000008aa1412130a1111000000010000000ce2b83768728e700120002802';

// Static tower event operates blob — slightly different from dungeon operates
const TOWER_OPERATES_HEX = '20000000c609000008aa1412130a111100000001000000e94c3bd6037616750120002802';

// Teleport special position name for entering a chapter map
// Chapter 2 uses "DungeonTeleport20" (old format), chapter 3+ uses "DungeonTeleport{N}"
function teleportPos(chapter) {
  if (chapter >= 3) return `DungeonTeleport${chapter}`;
  return `DungeonTeleport${chapter}0`;
}

// ── Grid position rules per Post (class) ────────────────────────────────────
// Post 1=Guardian(front), 2=Support(mid+back), 3=Assassin(any),
// 4=Warrior(front+mid), 5=Archer(mid+back), 6=Mage(mid+back)
// Positions: 1,2,3 = front row; 4,5,6 = mid row; 7,8,9 = back row
// Columns: Left=1,4,7; Center=2,5,8; Right=3,6,9
const POST_POSITIONS = {
  1: [1, 2, 3],                   // Guardian/tank — front only
  2: [7, 8, 9, 4, 5, 6],         // Support — back preferred, mid ok
  3: [4, 5, 6, 1, 2, 3, 7, 8, 9], // Assassin — mid preferred
  4: [1, 2, 3, 4, 5, 6],         // Warrior — front preferred, mid ok
  5: [7, 8, 9, 4, 5, 6],         // Archer — back preferred
  6: [4, 5, 6, 7, 8, 9],         // Mage — mid preferred
};
// Row assignments:
//   Front row: Tanks (Post 1) + Fighters (Post 4)
//   Mid row:   Assassins (Post 3) + Mages (Post 6)
//   Back row:  Supports (Post 2) + Archers (Post 5)
// Column alignment preferences:
//   - Assassin in front of Archer (same column)
//   - Tank in front of Support (same column)
const POST_PREFERRED = {
  1: [2, 1, 3],                   // Tank: front, center preferred
  2: [8, 7, 9],                   // Support: back row only
  3: [5, 4, 6],                   // Assassin: mid row (column aligned at placement time)
  4: [1, 3, 2],                   // Warrior: front corners
  5: [8, 7, 9],                   // Archer: back row
  6: [5, 4, 6],                   // Mage: mid row
};

// Get column (0=left, 1=center, 2=right) for a position 1-9
function posColumn(pos) { return (pos - 1) % 3; }

// ── Default settings ────────────────────────────────────────────────────────
export const DEFAULT_SETTINGS = {
  enabled: false,
  allowGacha: true,       // pull heroes when stuck (use all tickets)
  allowLevelUp: true,     // spend gold/exp to level heroes
  allowEquip: true,       // auto-equip gear
  allowFormation: true,   // auto-optimize formation
  collectIdle: true,      // collect hangup rewards
  stopOnFail: false,      // stop auto-play entirely on first fail
  allowReset: true,       // reset replaced heroes to reclaim resources
  targetChapter: 3,       // keep retrying until this chapter is reached
};

// ── Hero rarity (Quality field) ──────────────────────────────────────────────
// 2=N (can't breakthrough!), 3=R, 4=SR, 5=SSR, 6=SSR+
const QUALITY_N = 2;

// ── Hero scoring for smart team selection ───────────────────────────────────
// Priority: Quality > Star > Level > FightPower
// A 5-6★ Lv1 ALWAYS beats a 3★ Lv20 (star dominates over level/fp)
function heroScore(h, postInfo) {
  const star = h.star || 1;
  const quality = postInfo?.Quality || 2;
  const fp = h.fightPower || 0;
  const lv = h.level || 1;
  // N heroes get massive penalty — can't breakthrough, dead weight
  if (quality <= QUALITY_N) return fp + lv;
  // Quality: 10M weight (SSR=60M >> SR=50M >> R=40M)
  // Star: 100K weight (6★=600K >> 3★=300K) — dominates fp (typically < 100K)
  // Level: 100 weight
  // FP: 1 weight (tiebreaker only)
  return quality * 10000000 + star * 100000 + lv * 100 + fp;
}

// ── Formation slot limits based on dungeon progress ─────────────────────────
// Slot 5 unlocks at Stage 1-9 (1000109), Slot 6 at Stage 2-16 (1000216)
function getMaxSlots(acc) {
  const cleared = acc.lastClearedDungeon || 0;
  const current = acc.mainDungeonId || 0;
  const best = Math.max(cleared, current - 1); // current is uncleared, so cleared = current-1 at best
  if (best >= 1000216) return 6;
  if (best >= 1000109) return 5;
  return 4;
}

// ── Select best N heroes (balanced team) ──────────────────────────────────────
// Class limits: tanks (Post 1) and fighters (Post 4) can have up to 2, others max 1.
// If no tank/fighter available, fill with best-scoring heroes.
const POST_MAX_COUNT = { 1: 2, 2: 1, 3: 1, 4: 2, 5: 1, 6: 1 };

function selectBestTeam(heroes, db, maxSlots = 6) {
  // Enrich with Post data
  const enriched = heroes.map(h => {
    const postInfo = db.heroPost(h.heroId);
    return { ...h, post: postInfo?.Post || 0, quality: postInfo?.Quality || 2, score: heroScore(h, postInfo) };
  });

  // Group by Post (class) and sort each group by score desc
  const byPost = {};
  for (const h of enriched) {
    const p = h.post || 0;
    if (!byPost[p]) byPost[p] = [];
    byPost[p].push(h);
  }
  for (const p of Object.keys(byPost)) {
    byPost[p].sort((a, b) => b.score - a.score);
  }

  const team = [];
  const usedIds = new Set();
  const postCount = {};

  // 5★+ heroes bypass per-class caps (too strong to leave on the bench)
  const bypassesCap = (h) => (h.star || 1) >= 5;

  const tryAdd = (h) => {
    if (usedIds.has(h.heroId)) return false;
    if (team.length >= maxSlots) return false;
    if (!bypassesCap(h)) {
      const max = POST_MAX_COUNT[h.post] || 1;
      if ((postCount[h.post] || 0) >= max) return false;
    }
    team.push(h);
    usedIds.add(h.heroId);
    postCount[h.post] = (postCount[h.post] || 0) + 1;
    return true;
  };

  // Pass 1: best hero from each class (1 per class, all 6 roles)
  for (let post = 1; post <= 6; post++) {
    const candidates = byPost[post];
    if (candidates?.length) tryAdd(candidates[0]);
  }

  // Pass 2: add any 5★+ heroes that weren't picked yet (bypasses per-class caps)
  const elites = enriched.filter(h => !usedIds.has(h.heroId) && bypassesCap(h))
    .sort((a, b) => b.score - a.score);
  for (const h of elites) {
    if (team.length >= maxSlots) break;
    tryAdd(h);
  }

  // Pass 3: 2nd tank (Post 1) and 2nd fighter (Post 4) if slots remain
  for (const post of [1, 4]) {
    const candidates = byPost[post];
    if (candidates && candidates.length >= 2) tryAdd(candidates[1]);
  }

  // Pass 4: fill any leftover slots with best-scoring remaining (respecting per-post caps for < 5★)
  if (team.length < maxSlots) {
    const remaining = enriched
      .filter(h => !usedIds.has(h.heroId))
      .sort((a, b) => b.score - a.score);
    for (const h of remaining) {
      if (team.length >= maxSlots) break;
      tryAdd(h);
    }
  }

  // Last-resort fill ignoring per-post caps (e.g. no tank/fighter at all)
  if (team.length < maxSlots) {
    const remaining = enriched
      .filter(h => !usedIds.has(h.heroId))
      .sort((a, b) => b.score - a.score);
    for (const h of remaining) {
      if (team.length >= maxSlots) break;
      team.push(h);
      usedIds.add(h.heroId);
    }
  }

  return { team, allEnriched: enriched };
}

// ── Power-up cycle: formation → level → equip ──────────────────────────────
async function powerUp(acc, s) {
  if (s.allowFormation) await optimizeFormation(acc);
  if (s.allowReset) await resetNonTeamHeroes(acc);
  // Force gacha if team has N-rarity heroes (can't breakthrough, dead weight)
  if (teamHasNHeroes(acc)) {
    acc._log('[autoplay] Team has N-rarity heroes — force pulling gacha');
    await useHeroShards(acc);
    await doGachaPulls(acc); // Pull all tickets to replace N heroes
    if (s.allowFormation) await optimizeFormation(acc); // Re-evaluate team after pulls
  } else if (s.allowGacha) {
    await useHeroShards(acc);
  }
  await autoStarUp(acc);
  await claimAll(acc, { only: ['myths-levelup', 'protagonist-rewards'] });
  if (s.allowLevelUp) await levelAllHeroes(acc);
  if (s.allowEquip) await autoEquip(acc);
}

// Check if current team lineup includes any N-rarity (quality 2) heroes
function teamHasNHeroes(acc) {
  const db = getGameDB();
  const heroes = acc._heroRoster || [];
  const teamIds = acc._teamLineup || [];
  for (const tid of teamIds) {
    const h = heroes.find(h2 => h2.heroId === tid);
    if (!h) continue;
    const post = db.heroPost(h.heroId);
    if ((post?.Quality || 2) <= QUALITY_N) return true;
  }
  return false;
}

// ── Use hero shards to summon heroes ────────────────────────────────────────
// Generic shards: item 1029/1030/1031 (R/SR/SSR), 60 = 1 random hero
// Hero-specific shards: item 6xxxx (e.g. 61403 = shards for hero 1403), 60 = 1 specific hero
async function useHeroShards(acc) {
  const items = acc._items || {};
  const GENERIC_SHARD_IDS = [1029, 1030, 1031];

  for (const entry of Object.values(items)) {
    if (entry.count < 60) continue;
    const isGeneric = GENERIC_SHARD_IDS.includes(entry.itemId);
    const isHeroShard = entry.itemId >= 60000 && entry.itemId < 70000; // 6xxxx pattern
    if (!isGeneric && !isHeroShard) continue;

    const useQty = Math.floor(entry.count / 60) * 60;
    if (useQty < 60) continue;
    try {
      acc.client.send('CSUseItem', {
        useInfo: { itemId: entry.itemId, itemNum: useQty },
        selectItemKey: entry.itemKey,
      });
      const desc = isHeroShard ? `Hero ${entry.itemId - 60000} shards` : `Luminary Shards (${entry.itemId})`;
      acc._log(`[autoplay] Used ${useQty}x ${desc} → ${useQty / 60} hero summons`);
      entry.count -= useQty;
      await sleep(100);
    } catch (e) {
      acc._log(`[autoplay] Use shards failed: ${e.message}`);
    }
  }
}

// mythsLevelUp + protagonist rewards moved to claims.js (myths-levelup, protagonist-rewards)

// Reset a single hero
async function resetHero(acc, hero) {
  try {
    await acc.sendAndWait('CSResetFighterReq', { fightTid: hero.heroId }, 'SCResetFighterSuccess', 5000);
    acc._log(`[autoplay] Reset hero ${hero.heroId} (was Lv${hero.level}) — resources reclaimed`);
    hero.level = 1;
    hero.fightPower = 0;
    return true;
  } catch (e) {
    acc._log(`[autoplay] Reset hero ${hero.heroId} failed: ${e.message}`);
    return false;
  }
}

// Reset all heroes NOT in the current team that have levels > 1
async function resetNonTeamHeroes(acc) {
  const heroes = acc._heroRoster || [];
  const teamSet = new Set(acc._teamLineup || []);
  const toReset = heroes.filter(h => !teamSet.has(h.heroId) && (h.level || 1) > 1);
  for (const hero of toReset) {
    try {
      await acc.sendAndWait('CSResetFighterReq', { fightTid: hero.heroId }, 'SCResetFighterSuccess', 5000);
      acc._log(`[autoplay] Reset hero ${hero.heroId} (was Lv${hero.level}) — resources reclaimed`);
      hero.level = 1;
      hero.fightPower = 0;
    } catch (e) {
      acc._log(`[autoplay] Reset hero ${hero.heroId} failed: ${e.message}`);
    }
  }
}

// ── Auto star-up: feed duplicate copies to raise star level ─────────────────
// Strategy: ANY rarity → 6★ (just costs duplicates). Purple+ only for SSR (quality 5+).
// CSFighterStarUp { starMsg: [{ fighterId: <keep_inst_id>, selfIds: [<feed_inst_ids>] }] }
// HeroesStar DB table defines how many copies needed per star transition (varies by quality).
async function autoStarUp(acc) {
  const heroes = acc._heroRoster || [];
  if (heroes.length < 2) return;

  const db = getGameDB();
  const teamSet = new Set(acc._teamLineup || []);

  // Group by heroId (template) — find duplicates
  const byTemplate = {};
  for (const h of heroes) {
    if (!byTemplate[h.heroId]) byTemplate[h.heroId] = [];
    byTemplate[h.heroId].push(h);
  }

  let starred = 0;
  for (const [heroId, copies] of Object.entries(byTemplate)) {
    if (copies.length < 2) continue;

    const quality = db.heroPost(Number(heroId))?.Quality || 2;
    // Only star up to 5★ for non-SSR (quality < 5) — beyond 5★ needs 6★ fodder (ItselfBaseNum)
    // For SSR+ (quality >= 5), allow higher stars
    const maxStar = quality >= 5 ? 99 : 5;

    // Sort: team members first (by instance id match), then by star desc, then by id desc
    copies.sort((a, b) => {
      // Team member = instance that's on team (all copies share heroId, so check instance)
      const aOnTeam = teamSet.has(a.heroId) ? 1 : 0;
      const bOnTeam = teamSet.has(b.heroId) ? 1 : 0;
      // Prefer higher star first, then higher instance id
      if ((a.star || 3) !== (b.star || 3)) return (b.star || 3) - (a.star || 3);
      if (aOnTeam !== bOnTeam) return bOnTeam - aOnTeam;
      return b.id - a.id;
    });

    const keep = copies[0]; // Best copy to keep

    // Try star-up rounds until we run out of copies
    let feedIdx = 1;
    while (feedIdx < copies.length) {
      if (!acc._autoPlayRunning && !acc._claimingRewards) break;
      const currentStar = keep.star || 3;
      if (currentStar >= maxStar) break;

      // Look up how many self-copies needed for this star transition
      const copiesNeeded = db.starUpCopiesNeeded(currentStar, quality);
      if (copiesNeeded <= 0) break; // No data or no copies needed (needs fodder instead)

      // Check if we have enough remaining copies
      const remaining = copies.length - feedIdx;
      if (remaining < copiesNeeded) {
        acc._log(`[autoplay] Star-up ${heroId}: need ${copiesNeeded} copies for ${currentStar}→${currentStar+1}★, only have ${remaining}`);
        break;
      }

      // Collect the required number of feed instance IDs
      const selfIds = [];
      for (let i = 0; i < copiesNeeded; i++) {
        selfIds.push(copies[feedIdx + i].id);
      }

      try {
        const resp = await new Promise((resolve, reject) => {
          const timeout = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 3000);
          const cleanup = () => {
            clearTimeout(timeout);
            acc.client?.off?.('SCFighterStarUp', onOk);
            acc.client?.off?.('SCNotifyMessage', onErr);
          };
          const onOk = (pkt) => { cleanup(); resolve(pkt); };
          const onErr = (pkt) => { cleanup(); reject(new Error(`rejected: ${pkt.data?.messageCode || '?'}`)); };
          acc.client.handle('SCFighterStarUp', onOk);
          acc.client.handle('SCNotifyMessage', onErr);
          acc.client.send('CSFighterStarUp', {
            starMsg: [{ fighterId: keep.id, selfIds }],
          });
        });
        const newStar = resp.data?.fighterStarMsg?.[0]?.starLevel || currentStar + 1;
        const heroName = db.heroName(Number(heroId));
        acc._log(`[autoplay] Star-up ${heroName} (${heroId}): ${currentStar}→${newStar}★ (fed ${copiesNeeded} copies: [${selfIds.join(',')}])`);
        keep.star = newStar;
        starred++;
        // Remove fed copies from roster
        for (let i = 0; i < copiesNeeded; i++) {
          const fed = copies[feedIdx + i];
          const idx = acc._heroRoster.indexOf(fed);
          if (idx >= 0) acc._heroRoster.splice(idx, 1);
        }
        feedIdx += copiesNeeded;
        await sleep(50);
      } catch (e) {
        acc._log(`[autoplay] Star-up ${heroId} failed: ${e.message}`);
        break;
      }
    }

    // Also do album star up (CSAlbumStarLvUp) — separate system, uses template ID
    try {
      acc.client.send('CSAlbumStarLvUp', { fighterTid: Number(heroId) });
      await sleep(50);
    } catch {}
  }
  if (starred > 0) acc._log(`[autoplay] Star-up complete: ${starred} merges`);
}

// ── Main auto-play loop ─────────────────────────────────────────────────────
export async function runAutoPlay(acc) {
  if (acc._autoPlayRunning) return;
  acc._autoPlayRunning = true;
  const s = acc.autoPlaySettings;
  acc._autoPlayStats = { wins: 0, losses: 0, retries: 0, stage: null, status: 'running' };

  try {
    acc._log('[autoplay] Starting auto-play');
    acc.emit('state', { account: acc.id, state: acc.state, prev: acc.state });

    // Detect fresh account: still in chapter 1 (dungeon < 1000200)
    const startDungeon = acc._getNextDungeonId() || 0;
    const isFreshChapter1 = startDungeon > 0 && startDungeon < 1000200;
    let ch1FirstFail = false; // track if we already did gather/powerup after first ch1 fail

    if (isFreshChapter1) {
      acc._log('[autoplay] Fresh account (chapter 1) — advancing levels first, gather on first fail');
    } else {
      // Established account: always start with gather + powerup
      if (s.collectIdle) await claimFreebies(acc);
      await powerUp(acc, s);
    }

    // Target dungeon — stop when next dungeon is at/past this (default: 3-17 i.e. 3-16 cleared)
    const targetDungeon = s.targetDungeon || (1000000 + (s.targetChapter || 3) * 100 + 17);

    // Main dungeon loop
    let consecutiveFails = 0;

    while (acc._autoPlayRunning && s.enabled && acc.state === 'online') {
      const currentDungeon = acc._getNextDungeonId();
      const stillChapter1 = isFreshChapter1 && !ch1FirstFail && currentDungeon > 0 && currentDungeon < 1000200;

      // Check if we reached the target chapter
      if (currentDungeon >= targetDungeon) {
        acc._autoPlayStats.status = 'target_reached';
        acc._log(`[autoplay] Reached chapter ${s.targetChapter || 3} (dungeon ${currentDungeon}) — done`);
        break;
      }

      // Periodic claim cycle (~every 60s) — skip for fresh ch1 pre-first-fail
      if (!stillChapter1 && s.collectIdle && shouldClaim(acc)) {
        await claimFreebies(acc);
        await powerUp(acc, s);
      }

      const dungeonId = currentDungeon;
      if (!dungeonId) {
        acc._log('[autoplay] No dungeon to attempt');
        break;
      }

      acc._autoPlayStats.stage = dungeonId;
      acc.emit('state', { account: acc.id, state: acc.state, prev: acc.state });

      acc._log(`[autoplay] Attempting dungeon ${dungeonId}...`);
      const result = await attemptDungeon(acc, dungeonId);
      acc._log(`[autoplay] Dungeon result: ${JSON.stringify(result).substring(0, 200)}`);

      if (result.win) {
        acc._autoPlayStats.wins++;
        consecutiveFails = 0;
        acc._log(`[autoplay] WIN ${dungeonId} (total: ${acc._autoPlayStats.wins})`);
        acc.emit('state', { account: acc.id, state: acc.state, prev: acc.state });
        await sleep(50);
      } else {
        acc._autoPlayStats.losses++;
        consecutiveFails++;
        acc._autoPlayStats.retries = consecutiveFails;
        acc._log(`[autoplay] FAIL ${dungeonId} (attempt ${consecutiveFails})`);

        // Fresh chapter 1: on first fail (including level_too_low), do full gather + powerup
        if (isFreshChapter1 && !ch1FirstFail) {
          ch1FirstFail = true;
          acc._log('[autoplay] First chapter 1 fail — gathering rewards and powering up');
          if (s.collectIdle) await claimFreebies(acc);
          await powerUp(acc, s);
          if (s.allowGacha) {
            await doGachaPulls(acc);
            await powerUp(acc, s);
          }
          consecutiveFails = 0;
          await sleep(100);
          continue;
        }

        // level_too_low: gather + powerup to try raising player level, then retry
        if (result.error === 'level_too_low') {
          acc._log(`[autoplay] Need player Lv${result.levelRequired} (currently Lv${acc.playerLevel}) — gathering and retrying`);
          if (s.collectIdle) await claimFreebies(acc);
          await powerUp(acc, s);
          await sleep(100);
          continue;
        }

        if (s.stopOnFail) {
          acc._autoPlayStats.status = 'stuck';
          acc._log(`[autoplay] Stuck at ${dungeonId} (stopOnFail)`);
          break;
        }

        // Gather rewards from newly cleared dungeons, then power up
        acc._log('[autoplay] Gathering rewards and powering up...');
        if (s.collectIdle) await claimFreebies(acc);
        await powerUp(acc, s);
        if (s.allowGacha && consecutiveFails >= 2) {
          await doGachaPulls(acc);
          await powerUp(acc, s);
        }
        await sleep(100);
      }
    }
  } catch (e) {
    acc._log(`[autoplay] Error: ${e.message}`);
    acc._autoPlayStats.status = 'error';
  } finally {
    acc._autoPlayRunning = false;
    if (acc._autoPlayStats.status === 'running') {
      acc._autoPlayStats.status = 'stopped';
    }
    acc._log(`[autoplay] Stopped (${acc._autoPlayStats.wins}W/${acc._autoPlayStats.losses}L)`);
    acc.emit('state', { account: acc.id, state: acc.state, prev: acc.state });
  }
}

export function stopAutoPlay(acc) {
  acc._autoPlayRunning = false;
  acc.autoPlaySettings.enabled = false;
}

// ── Attempt a single dungeon ────────────────────────────────────────────────
async function attemptDungeon(acc, dungeonId) {
  try {
    const db = getGameDB();

    // Ensure we're on the correct chapter map before entering
    const chapter = Math.floor((dungeonId - 1000000) / 100);
    const destMap = 1000 + chapter;
    if (acc._currentMap !== destMap) {
      acc._log(`[autoplay] Transferring to map ${destMap} for chapter ${chapter}`);
      acc.client.send('CSTransferRequest', {
        destMap,
        type: 3,
        specialPos: teleportPos(chapter),
      });
      acc._currentMap = destMap;
      await sleep(100);
    }

    // Pre-check: dungeon level requirement
    const dInfo = db.dungeonInfo(dungeonId);
    if (dInfo?.Limit_Level && acc.playerLevel < dInfo.Limit_Level) {
      acc._log(`[autoplay] Dungeon ${dungeonId} requires Lv${dInfo.Limit_Level}, player is Lv${acc.playerLevel}`);
      return { win: false, error: 'level_too_low', levelRequired: dInfo.Limit_Level };
    }

    // Try enter + draw up to 5x before giving up — transient failures often resolve
    let result = null;
    let lastError = null;
    for (let attempt = 1; attempt <= 5; attempt++) {
      if (!acc._autoPlayRunning) break;

      // Enter dungeon
      const enterResult = await new Promise((resolve) => {
        const timeout = setTimeout(() => { cleanup(); resolve({ entered: false, error: 'timeout' }); }, 10000);
        const cleanup = () => {
          clearTimeout(timeout);
          acc.client?.off?.('SCRspEnterDungeon', onEnter);
          acc.client?.off?.('SCNotifyBatlleInterrupt', onReject);
          acc.client?.off?.('SCNotifyMessage', onMsg);
        };
        let rejectMsg = '';
        const onMsg = (pkt) => { rejectMsg = JSON.stringify(pkt.data); };
        const onEnter = (pkt) => { cleanup(); resolve({ entered: true, data: pkt.data }); };
        const onReject = (_pkt) => { cleanup(); resolve({ entered: false, error: rejectMsg || 'rejected' }); };
        acc.client.handle('SCNotifyMessage', onMsg);
        acc.client.handle('SCRspEnterDungeon', onEnter);
        acc.client.handle('SCNotifyBatlleInterrupt', onReject);
        acc.client.send('CSReqEnterDungeon', {
          dungeonId, copyId: dungeonId, useHelpFighter: false, playerId: '',
          helpFighterId: -1, helpFighterSquadId: -1, pos: -1, sortIndex: -1,
          closeRocker: -1, seq: 1,
        });
      });

      if (!enterResult.entered) {
        lastError = enterResult.error;
        acc._log(`[autoplay] Dungeon ${dungeonId} entry rejected (try ${attempt}/5): ${enterResult.error}`);
        await sleep(200);
        continue;
      }

      // Draw award
      await sleep(100);
      const drawResult = await new Promise((resolve) => {
        const timeout = setTimeout(() => { cleanup(); resolve({ win: false, data: { error: 'timeout' } }); }, 15000);
        const cleanup = () => {
          clearTimeout(timeout);
          acc.client?.off?.('SCDrawDungeonAward', onWin);
          acc.client?.off?.('SCNotifyBatlleInterrupt', onFail);
        };
        const onWin = (pkt) => { cleanup(); resolve({ win: true, data: pkt.data }); };
        const onFail = (pkt) => { cleanup(); resolve({ win: false, data: pkt.data }); };
        acc.client.handle('SCDrawDungeonAward', onWin);
        acc.client.handle('SCNotifyBatlleInterrupt', onFail);
        acc.client.send('CSDrawDungeonAward', {
          dungeonId,
          battleEnd: {
            copyId: dungeonId,
            result: 1,
            operates: OPERATES_HEX,
          },
        });
      });

      result = drawResult;
      if (drawResult.win) break; // success — stop retrying
      lastError = drawResult.data?.messageCode || drawResult.data?.error || 'fail';
      acc._log(`[autoplay] Dungeon ${dungeonId} draw failed (try ${attempt}/5): ${JSON.stringify(drawResult.data).substring(0, 100)}`);
      await sleep(200);
    }

    if (!result) result = { win: false, error: lastError || 'all retries failed' };

    if (result.win) {
      const prevChapter = Math.floor((dungeonId - 1000000) / 100);
      acc.lastClearedDungeon = dungeonId;
      const rows = db.query('MainDungeon', 'SELECT NextStageId FROM MainDungeon WHERE id = ?', [dungeonId]);
      if (rows.length && rows[0].NextStageId > 0) {
        acc.mainDungeonId = rows[0].NextStageId;
        // Chapter transition — send CSTransferRequest to move to the new map
        const nextChapter = Math.floor((rows[0].NextStageId - 1000000) / 100);
        if (nextChapter !== prevChapter) {
          const nextMap = 1000 + nextChapter;
          acc._log(`[autoplay] Chapter ${prevChapter}→${nextChapter}, transferring to map ${nextMap}`);
          acc.client.send('CSTransferRequest', {
            destMap: nextMap,
            type: 3,
            specialPos: teleportPos(nextChapter),
          });
          acc._currentMap = nextMap;
          await sleep(100);
          // First time reaching chapter 3 — run the tower intro sequence
          if (nextChapter === 3 && !acc._towerCh3Done) {
            await runTowerChapter3Intro(acc);
          }
        }
      }
    }
    return result;
  } catch (e) {
    acc._log(`[autoplay] Dungeon ${dungeonId} error: ${e.message}`);
    return { win: false, error: e.message };
  }
}

// ── Level all team heroes — no client-side cap, server decides max ───────────
// Level 1 at a time until server rejects. Multi-pass: strongest first.
export async function levelAllHeroes(acc) {
  const MAX_LV = 999; // no client cap — server enforces the real limit
  const heroes = acc._heroRoster || [];
  const teamIds = acc._teamLineup || [];

  // Only level team heroes (focus resources on the 6 we're using)
  // Deduplicate: if multiple copies share a heroId, only include one per teamId entry
  const usedHeroIds = new Set();
  const teamHeroes = [];
  for (const tid of teamIds) {
    if (usedHeroIds.has(tid)) continue;
    const h = heroes.find(h2 => h2.heroId === tid);
    if (h) { teamHeroes.push(h); usedHeroIds.add(tid); }
  }
  if (teamHeroes.length === 0) return;

  let leveled = 0;

  // Level one hero 1 level at a time, returns { progress, noResources }
  // No client-side cap — server decides the limit
  async function levelHero(h, targetLv) {
    const startLv = h.level || 1;
    if (startLv >= targetLv) return { progress: false, noResources: false };

    let anySuccess = false;
    let noResources = false;
    for (let lv = startLv + 1; lv <= targetLv; lv++) {
      if (!acc._autoPlayRunning) break;
      try {
        const resp = await new Promise((resolve, reject) => {
          const timeout = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 5000);
          const cleanup = () => {
            clearTimeout(timeout);
            acc.client?.off?.('SCFighterLevelUpResp', onOk);
            acc.client?.off?.('SCNotifyMessage', onErr);
          };
          const onOk = (pkt) => { cleanup(); resolve(pkt); };
          const onErr = (pkt) => {
            const code = pkt.data?.messageCode || 0;
            cleanup(); reject(Object.assign(new Error(`${code}`), { code }));
          };
          acc.client.handle('SCFighterLevelUpResp', onOk);
          acc.client.handle('SCNotifyMessage', onErr);
          acc.client.send('CSFighterLevelUp', { fighterTid: h.heroId, targetLv: lv });
        });
        h.level = resp.data?.level || lv;
        leveled++;
        anySuccess = true;
      } catch (e) {
        // 10017 = not enough resources → no point trying other heroes
        if (e.code === 10017) noResources = true;
        if (!anySuccess) acc._log(`[autoplay] Level ${h.heroId} Lv${lv-1}→${lv} rejected: ${e.message}`);
        break;
      }
    }
    if (anySuccess) acc._log(`[autoplay] Leveled ${h.heroId} Lv${startLv}→${h.level}`);
    return { progress: anySuccess, noResources };
  }

  // Multi-pass leveling: strongest first, level each until fail, repeat passes until all fail
  const db2 = getGameDB();
  teamHeroes.sort((a, b) => {
    const postA = db2.heroPost(a.heroId);
    const postB = db2.heroPost(b.heroId);
    return heroScore(b, postB) - heroScore(a, postA);
  });

  // Misallocation check: if a stronger team hero is at a lower level than a weaker team hero,
  // resources are distributed wrong. Reset all team heroes so the next pass funnels resources
  // to the strongest hero first.
  let misallocated = false;
  for (let i = 0; i < teamHeroes.length; i++) {
    for (let j = i + 1; j < teamHeroes.length; j++) {
      if ((teamHeroes[i].level || 1) < (teamHeroes[j].level || 1)) {
        misallocated = true; break;
      }
    }
    if (misallocated) break;
  }
  if (misallocated) {
    acc._log('[autoplay] Resource misallocation detected — resetting team heroes to redistribute');
    for (const h of teamHeroes) {
      if ((h.level || 1) > 1) await resetHero(acc, h);
    }
  }

  let passNum = 0;
  while (acc._autoPlayRunning) {
    passNum++;
    let anyProgress = false;
    for (const h of teamHeroes) {
      if (!acc._autoPlayRunning) break;
      if ((h.level || 1) >= MAX_LV) continue;
      const { progress, noResources } = await levelHero(h, MAX_LV);
      if (progress) anyProgress = true;
      if (noResources) break; // no gold/xp left — skip remaining heroes this pass
    }
    if (!anyProgress) break;
    acc._log(`[autoplay] Level pass ${passNum} complete, checking for more...`);
  }

  if (leveled > 0) acc._log(`[autoplay] Leveled ${leveled} total across ${passNum} passes`);
}

// ── Auto-equip unassigned gear to strongest heroes ──────────────────────────
// 6 equipment slots (Part 1-6 from Equipments DB). First digit of templateId = Part.
// CSEquipChange: { fighterTid, equipIds: [existing_instance_ids], equipTids: [new_template_ids] }
export async function autoEquip(acc) {
  const equips = acc._equipBag || [];
  const heroes = acc._heroRoster || [];
  const teamIds = acc._teamLineup || [];
  const db = getGameDB();

  // Find unequipped items (fighterTid === 0) grouped by Part (slot)
  const unequippedByPart = {}; // part → [equip entries]
  for (const e of equips) {
    if (e.fighterTid && e.fighterTid !== 0) continue;
    if (!e.templateId) continue;
    const info = db.equipInfo(e.templateId);
    const part = info?.part || Math.floor(e.templateId / 100000);
    if (!unequippedByPart[part]) unequippedByPart[part] = [];
    unequippedByPart[part].push(e);
  }
  if (Object.keys(unequippedByPart).length === 0) return;

  // Sort each part's gear by quality desc (best gear first)
  for (const part of Object.keys(unequippedByPart)) {
    unequippedByPart[part].sort((a, b) => {
      const qa = db.equipInfo(a.templateId)?.quality || 0;
      const qb = db.equipInfo(b.templateId)?.quality || 0;
      return qb - qa;
    });
  }

  const teamHeroes = heroes.filter(h => teamIds.includes(h.heroId));
  teamHeroes.sort((a, b) => (b.fightPower || 0) - (a.fightPower || 0));

  for (const hero of teamHeroes) {
    if (Object.values(unequippedByPart).every(arr => arr.length === 0)) break;

    const heroPost = db.heroPost(hero.heroId)?.Post || 0;

    // Get existing equipment on this hero (instance IDs + parts)
    const heroEquips = equips.filter(e => e.fighterTid === hero.heroId);
    const equippedParts = new Set();
    const existingEquipIds = [];
    for (const e of heroEquips) {
      const info = db.equipInfo(e.templateId);
      const part = info?.part || Math.floor(e.templateId / 100000);
      equippedParts.add(part);
      existingEquipIds.push(e.id);
    }

    // Find new gear for empty slots
    const newEquipTids = [];
    for (let part = 1; part <= 6; part++) {
      if (equippedParts.has(part)) continue;
      const available = unequippedByPart[part];
      if (!available || available.length === 0) continue;

      // Check job compatibility if DB has info
      let picked = null;
      for (let i = 0; i < available.length; i++) {
        const info = db.equipInfo(available[i].templateId);
        // If JobLimit exists and doesn't include this hero's Post, skip
        if (info?.jobs?.length > 0 && !info.jobs.includes(heroPost)) continue;
        picked = available.splice(i, 1)[0];
        break;
      }
      if (picked) newEquipTids.push(picked.templateId);
    }

    if (newEquipTids.length === 0) continue;

    try {
      const payload = { fighterTid: hero.heroId, equipTids: newEquipTids };
      // Include existing equip instance IDs if hero already has gear
      if (existingEquipIds.length > 0) payload.equipIds = existingEquipIds;

      const result = await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 3000);
        const cleanup = () => {
          clearTimeout(timeout);
          acc.client?.off?.('SCEquipChange', onOk);
          acc.client?.off?.('SCNotifyMessage', onErr);
        };
        const onOk = (pkt) => { cleanup(); resolve(pkt); };
        const onErr = (pkt) => { cleanup(); reject(new Error(`rejected: ${pkt.data?.messageCode || '?'}`)); };
        acc.client.handle('SCEquipChange', onOk);
        acc.client.handle('SCNotifyMessage', onErr);
        acc.client.send('CSEquipChange', payload);
      });
      const heroName = db.heroName(hero.heroId);
      acc._log(`[autoplay] Equipped [${newEquipTids.join(',')}] on ${heroName} (${hero.heroId}), kept [${existingEquipIds.join(',')}]`);
    } catch (e) {
      if (!e.message.includes('timeout')) {
        acc._log(`[autoplay] Equip hero ${hero.heroId} skipped: ${e.message}`);
      }
    }
  }
}

// ── Tower chapter-3 intro sequence (one-time per account) ───────────────────
// On first reaching chapter 3 (or connecting with ch3+ already cleared), run:
//   1. For eventId 2001001..2001005:
//        CSTowerChallengeStart → wait SCTowerChallengeStart → CSTowerEvent (battleResult=1)
//   2. Send final CSTowerEvent for eventId 2001006 with no battle (skip event)
export async function runTowerChapter3Intro(acc) {
  if (acc._towerCh3Done) return;
  acc._towerCh3Done = true; // set immediately to prevent re-entry
  acc._log('[autoplay] Running tower chapter-3 intro sequence...');

  const chapterId = 0;
  const areaId = 1;

  for (let eventId = 2001001; eventId <= 2001005; eventId++) {
    if (acc.state !== 'online') return;
    try {
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 5000);
        const cleanup = () => {
          clearTimeout(timeout);
          acc.client?.off?.('SCTowerChallengeStart', onOk);
          acc.client?.off?.('SCNotifyMessage', onErr);
        };
        const onOk = (pkt) => { cleanup(); resolve(pkt.data); };
        const onErr = (pkt) => { cleanup(); reject(new Error(`rejected: ${pkt.data?.messageCode || '?'}`)); };
        acc.client.handle('SCTowerChallengeStart', onOk);
        acc.client.handle('SCNotifyMessage', onErr);
        acc.client.send('CSTowerChallengeStart', {
          chapterId, areaId, eventId, copyId: eventId, seq: 1,
        });
      });
    } catch (e) {
      acc._log(`[autoplay] Tower start ${eventId} rejected: ${e.message}`);
      continue;
    }
    await sleep(100);
    try {
      acc.client.send('CSTowerEvent', {
        chapterId, areaId, eventId,
        operates: TOWER_OPERATES_HEX,
        battleResult: 1,
        copyId: eventId,
      });
      acc._log(`[autoplay] Tower event ${eventId} cleared`);
    } catch (e) {
      acc._log(`[autoplay] Tower event ${eventId} send failed: ${e.message}`);
    }
    await sleep(200);
  }

  // Final skip event — no battle
  if (acc.state === 'online') {
    try {
      acc.client.send('CSTowerEvent', {
        chapterId, areaId, eventId: 2001006,
        operates: '',
        battleResult: 0,
        copyId: 0,
      });
      acc._log('[autoplay] Tower event 2001006 (skip) sent');
    } catch (e) {
      acc._log(`[autoplay] Tower event 2001006 send failed: ${e.message}`);
    }
  }
  acc._log('[autoplay] Tower chapter-3 intro done');
}

// ── Gacha pulls — pull 1 by 1 until server rejects ─────────────────────────
export async function doGachaPulls(acc) {
  let pulled = 0;
  acc._log('[autoplay] Pulling gacha until depleted...');

  while (acc._autoPlayRunning) {
    try {
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 5000);
        const cleanup = () => {
          clearTimeout(timeout);
          acc.client?.off?.('SCUpdateHeroGachaTimes', onOk);
          acc.client?.off?.('SCNotifyMessage', onErr);
        };
        const onOk = (pkt) => { cleanup(); resolve(pkt.data); };
        const onErr = (pkt) => {
          cleanup();
          reject(new Error(`rejected: ${pkt.data?.messageCode || '?'}`));
        };
        acc.client.handle('SCUpdateHeroGachaTimes', onOk);
        acc.client.handle('SCNotifyMessage', onErr);
        acc.client.send('CSHeroGacha', { type: 1, periodId: 1, multiple: false, hundred: false });
      });
      pulled++;
      await sleep(50);
    } catch {
      break; // No more tickets or server rejected
    }
  }

  if (pulled > 0) acc._log(`[autoplay] Gacha done: ${pulled} pulls`);
  else acc._log('[autoplay] No gacha pulls available');
}

// ── Optimize formation — smart team selection ───────────────────────────────
// Strategy:
// 1. Pick best hero per class (Post 1-6) based on star/quality/FP
// 2. If we replaced a hero, reset the old one to reclaim resources
// 3. Assign positions based on class rules
// 4. Send CSSetFormation
export async function optimizeFormation(acc) {
  const heroes = acc._heroRoster || [];
  if (heroes.length === 0) return;

  const db = getGameDB();
  const s = acc.autoPlaySettings;
  const oldLineup = [...(acc._teamLineup || [])];

  // Select best team (1 per class, fill to max available slots)
  const maxSlots = getMaxSlots(acc);
  acc._log(`[autoplay] Formation: ${maxSlots} slots available`);
  const { team } = selectBestTeam(heroes, db, maxSlots);
  if (team.length === 0) return;

  const newLineupIds = team.map(h => h.heroId);

  // Check if lineup actually changed
  const changed = newLineupIds.length !== oldLineup.length ||
    newLineupIds.some(id => !oldLineup.includes(id));

  // Always send formation at least once per session (positions may differ from server)
  if (!changed && acc._formationSentThisSession) return;
  acc._formationSentThisSession = true;

  // Find heroes that were replaced
  const replaced = oldLineup.filter(id => !newLineupIds.includes(id));
  if (replaced.length > 0) {
    acc._log(`[autoplay] Team change: +[${newLineupIds.filter(id => !oldLineup.includes(id)).join(',')}] -[${replaced.join(',')}]`);
  }

  // Assign positions — row-staged (back → mid → front) to enable column alignment
  const usedPositions = new Set();
  const formation = [];
  const usedColumns = {}; // column → post of hero placed there (for alignment hints)

  const placeHero = (hero, preferredPositions) => {
    const valid = POST_POSITIONS[hero.post] || POST_POSITIONS[4];
    // Try preferred list first
    for (const pos of preferredPositions) {
      if (!usedPositions.has(pos) && valid.includes(pos)) {
        usedPositions.add(pos);
        usedColumns[posColumn(pos)] = hero.post;
        formation.push({ posId: pos, fighterTid: hero.heroId, sortIndex: team.indexOf(hero) });
        return true;
      }
    }
    // Fallback to any valid position
    for (const pos of valid) {
      if (!usedPositions.has(pos)) {
        usedPositions.add(pos);
        usedColumns[posColumn(pos)] = hero.post;
        formation.push({ posId: pos, fighterTid: hero.heroId, sortIndex: team.indexOf(hero) });
        return true;
      }
    }
    return false;
  };

  // Order preferred positions to prioritize a target column (for alignment)
  const alignedPreferred = (hero, targetCol) => {
    const pref = POST_PREFERRED[hero.post] || POST_PREFERRED[4];
    if (targetCol === undefined) return pref;
    return [...pref].sort((a, b) => {
      const aMatch = posColumn(a) === targetCol ? 0 : 1;
      const bMatch = posColumn(b) === targetCol ? 0 : 1;
      return aMatch - bMatch;
    });
  };

  // Stage 1: back row (Support=2, Archer=5) — establish columns first
  const backRow = team.filter(h => h.post === 2 || h.post === 5);
  for (const h of backRow) placeHero(h, POST_PREFERRED[h.post] || []);

  // Stage 2: mid row — Assassin prefers column with Archer behind, Mage prefers column with Support behind
  const archerCol = formation.find(f => team.find(h => h.heroId === f.fighterTid)?.post === 5);
  const supportCol = formation.find(f => team.find(h => h.heroId === f.fighterTid)?.post === 2);
  const assassinTargetCol = archerCol ? posColumn(archerCol.posId) : undefined;
  const mageTargetCol = supportCol ? posColumn(supportCol.posId) : undefined;
  for (const h of team.filter(hh => hh.post === 3)) placeHero(h, alignedPreferred(h, assassinTargetCol));
  for (const h of team.filter(hh => hh.post === 6)) placeHero(h, alignedPreferred(h, mageTargetCol));

  // Stage 3: front row — Tank prefers column with Support behind, Warrior fills rest
  for (const h of team.filter(hh => hh.post === 1)) placeHero(h, alignedPreferred(h, mageTargetCol ?? assassinTargetCol));
  for (const h of team.filter(hh => hh.post === 4)) placeHero(h, POST_PREFERRED[4] || []);

  // Stage 4: any remaining heroes (unknown/edge-case posts)
  for (const hero of team) {
    if (formation.find(f => f.fighterTid === hero.heroId)) continue;
    const preferred = POST_PREFERRED[hero.post] || POST_PREFERRED[4];
    let assigned = placeHero(hero, preferred);
    if (!assigned) {
      const valid = POST_POSITIONS[hero.post] || POST_POSITIONS[4];
      for (const pos of valid) {
        if (!usedPositions.has(pos)) {
          usedPositions.add(pos);
          formation.push({ posId: pos, fighterTid: hero.heroId, sortIndex: team.indexOf(hero) });
          break;
        }
      }
    }
  }

  if (formation.length === 0) return;

  // Safety: deduplicate by heroId (should never happen, but prevents game rejection)
  const seenHeroes = new Set();
  const dedupedFormation = formation.filter(f => {
    if (seenHeroes.has(f.fighterTid)) return false;
    seenHeroes.add(f.fighterTid);
    return true;
  });
  if (dedupedFormation.length !== formation.length) {
    acc._log(`[autoplay] WARNING: Removed ${formation.length - dedupedFormation.length} duplicate heroes from formation`);
  }

  // Update team lineup
  acc._teamLineup = dedupedFormation.map(f => f.fighterTid);

  try {
    await acc.sendAndWait('CSSetFormation', {
      formation: [{
        squadsId: 1,
        mythsTid: 8301, // wyrm protagonist
        fpos: dedupedFormation,
      }],
      updateEquipPreset: true,
    }, 'SCSetFormation', 5000);
    acc._log(`[autoplay] Formation: ${dedupedFormation.map(f => f.fighterTid + '@' + f.posId).join(', ')}`);
  } catch (e) {
    acc._log(`[autoplay] Formation update failed: ${e.message}`);
    return;
  }

}

// ── Claim all freebies: idle rewards, quick patrol, mail, tasks, activities ──
// Called once on login and then periodically (every ~60s) during autoplay
const CLAIM_INTERVAL = 60_000; // 1 minute

export async function claimFreebies(acc) {
  // Skip myths-levelup + protagonist-rewards here — those run in powerUp()
  await claimAll(acc, { skip: ['myths-levelup', 'protagonist-rewards'] });
}

// claimAllRewards moved to claims.js

// Check if it's time to claim again
function shouldClaim(acc) {
  return !acc._lastClaimTime || (Date.now() - acc._lastClaimTime) >= CLAIM_INTERVAL;
}

// Backward compat alias
export const collectIdleRewards = claimFreebies;
