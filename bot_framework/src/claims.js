// Modular reward claiming system — each claim type is a separate handler
// All handlers follow: send packet → wait for response → never send next until response is back
import { getGameDB } from './gamedb.js';

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// Send a packet and race between success response and rejection
// Listens for SCNotifyMessage and any SCNotify* packets as rejections
// Returns the success packet data, throws on rejection or timeout
async function sendOrReject(acc, sendName, sendData, okName, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, timeoutMs);
    const rejectHandlers = [];
    const cleanup = () => {
      clearTimeout(timeout);
      acc.client?.off?.(okName, onOk);
      for (const [name, fn] of rejectHandlers) acc.client?.off?.(name, fn);
    };
    const onOk = (pkt) => { cleanup(); resolve(pkt.data); };
    const onErr = (name) => (pkt) => {
      cleanup();
      reject(new Error(`rejected (${name}): ${pkt.data?.messageCode || '?'}`));
    };
    acc.client.handle(okName, onOk);
    // Listen for common rejection/notification packets
    for (const rn of ['SCNotifyMessage', 'SCNotifyVoucherNum', 'SCNotifyRedPoints']) {
      if (rn === okName) continue; // don't reject on the success packet
      const fn = onErr(rn);
      rejectHandlers.push([rn, fn]);
      acc.client.handle(rn, fn);
    }
    acc.client.send(sendName, sendData);
  });
}

// ── Claim Handlers ──────────────────────────────────────────────────────────
// Each: { id, name, run(acc) }
// run() should log via acc._log('[claim:ID] ...'), throw on fatal, internal try/catch for loops

export const CLAIMS = [

  // ── Idle / Patrol / Mail ────────────────────────────────────────────────
  {
    id: 'idle',
    name: 'Idle Rewards',
    async run(acc) {
      try {
        await sendOrReject(acc, 'CSHangUpAward', {}, 'SCHangUpAward', 3000);
      } catch { return; } // no idle rewards pending
      try {
        await sendOrReject(acc, 'CSHangUpAwardDraw', {}, 'SCHangUpAwardDraw', 3000);
        acc._log('[claim:idle] Collected');
      } catch {}
    },
  },

  {
    id: 'quick-patrol',
    name: 'Quick Patrol',
    async run(acc) {
      let count = 0;
      while (acc.state === 'online') {
        let info;
        try {
          info = await sendOrReject(acc, 'CSReqQuickHangUpInfo', {}, 'SCRspQuickHangUpInfo', 3000);
        } catch { break; }
        const cost = info?.needDiamondNum || 0;
        const remain = info?.remainBuyTimes;
        if (remain !== undefined && remain <= 0) break;
        const diamonds = acc._currencies?.[1] || 0;
        if (cost > 0 && diamonds < cost) {
          if (count === 0) acc._log(`[claim:quick-patrol] Need ${cost} diamonds, only have ${diamonds}`);
          break;
        }
        try {
          await sendOrReject(acc, 'CSQuickHangUpBuy', {}, 'SCQuickHangUpBuy', 3000);
        } catch { break; }
        try { await sendOrReject(acc, 'CSHangUpAward', {}, 'SCHangUpAward', 3000); } catch {}
        try { await sendOrReject(acc, 'CSHangUpAwardDraw', {}, 'SCHangUpAwardDraw', 3000); } catch {}
        count++;
        if (cost > 0 && acc._currencies?.[1]) acc._currencies[1] -= cost;
        await sleep(50);
      }
      if (count > 0) acc._log(`[claim:quick-patrol] Collected ${count} patrols`);
    },
  },

  {
    id: 'mail',
    name: 'Mail Rewards',
    async run(acc) {
      const mailData = await sendOrReject(acc, 'CSReqMailList', {}, 'SCRspMailList', 3000);
      const mails = mailData?.mails || [];
      const unclaimed = mails.filter(m => m.mailStatus !== 3 && m.mailId);
      if (unclaimed.length === 0) return;
      for (const m of unclaimed) {
        if (acc.state !== 'online') break;
        await sendOrReject(acc, 'CSMailOperate', { opType: 1, mailId: m.mailId }, 'SCMailOperate', 3000)
          .catch(() => {}); // some mails may have no attachment
        await sleep(50);
      }
      acc._log(`[claim:mail] Claimed ${unclaimed.length} mails`);
    },
  },

  // ── Tasks (daily/weekly/lifetime) ───────────────────────────────────────
  // CSTaskAwardOnKey claims all finished tasks, then CSReqActiveNumAward claims milestones
  {
    id: 'tasks',
    name: 'Daily/Weekly/Lifetime Tasks',
    async run(acc) {
      await claimAllTaskTypes(acc);
    },
  },

  {
    id: 'achievement-milestones',
    name: 'Achievement Point Milestones',
    async run(acc) {
      let count = 0;
      for (let i = 0; i < 20; i++) {
        if (acc.state !== 'online') break;
        try {
          const data = await sendOrReject(acc, 'CSAchNumReward', {}, 'SCAchNumReward', 2000);
          count++;
          acc._log(`[claim:achievement-milestones] Level=${data?.drawMaxLevel}`);
          await sleep(50);
        } catch {
          break;
        }
      }
      if (count > 0) acc._log(`[claim:achievement-milestones] Claimed ${count}`);
    },
  },

  {
    id: 'activity-milestones',
    name: 'Activity Point Milestones',
    async run(acc) {
      const taskData = acc._taskData;
      if (!taskData?.tasks) return;

      let claimed = 0;
      for (const group of taskData.tasks) {
        const claimable = (group.activeStatus || []).filter(a => a.activeStatus === 1);
        for (const active of claimable) {
          if (acc.state !== 'online') break;
          try {
            await sendOrReject(acc, 'CSReqActiveTaskAward',
              { taskType: group.taskType, activeId: active.activeId },
              'SCReqActiveTaskAward', 2000);
            claimed++;
          } catch {}
          await sleep(50);
        }
      }
      if (claimed > 0) acc._log(`[claim:activity-milestones] Claimed ${claimed}`);
    },
  },

  // ── Guide Tasks ─────────────────────────────────────────────────────────
  {
    id: 'guide-tasks',
    name: 'Guide/Tutorial Tasks',
    async run(acc) {
      const db = getGameDB();
      const dungeonId = acc.mainDungeonId || acc.lastClearedDungeon || 1000100;
      const chapterNum = Math.floor((dungeonId - 1000000) / 100);
      const maxGuideChapter = 9000 + chapterNum + 1;
      const allGuide = db.query('TaskGuide',
        'SELECT id, Condition FROM TaskGuide WHERE Condition <= ? ORDER BY Condition, id',
        [maxGuideChapter]);
      if (allGuide.length === 0) return;

      const byChapter = {};
      for (const g of allGuide) {
        if (!byChapter[g.Condition]) byChapter[g.Condition] = [];
        byChapter[g.Condition].push(g.id);
      }

      let total = 0;
      for (const [chapterId, taskIds] of Object.entries(byChapter)) {
        if (acc.state !== 'online') break;
        try {
          const data = await sendOrReject(acc, 'CSReqTakeGuideAward',
            { taskId: taskIds }, 'SCRspTakeGuideAward', 2000);
          const claimed = data?.taskId || [];
          if (claimed.length > 0) {
            total += claimed.length;
            acc._log(`[claim:guide-tasks] Chapter ${chapterId}: ${claimed.length} tasks`);
          }
        } catch {}
      }
      if (total > 0) acc._log(`[claim:guide-tasks] Total: ${total}`);
    },
  },

  // ── Activity Rewards (server-pushed data) ───────────────────────────────
  {
    id: 'carnival',
    name: 'Carnival Tasks',
    async run(acc) {
      // Find carnival activity from the activity list
      const avList = acc._activityList?.avSimpleInfo || [];
      const carnivalAv = avList.find(a => String(a.id).startsWith('1007'));
      if (!carnivalAv) return;
      // Query carnival state
      try {
        await sendOrReject(acc, 'CSAv', { id: [carnivalAv.id] }, 'SCAvCarnival', 3000);
      } catch { return; }
      const carnival = acc._carnivalData;
      if (!carnival?.id) return;
      const allTasks = [];
      for (const di of (carnival.dayInfo || [])) {
        for (const t of (di.task || [])) {
          if (t.state === 1) allTasks.push(t.tid);
        }
      }
      if (allTasks.length === 0) return;
      try {
        await sendOrReject(acc, 'CSAvCarnivalTaskReward',
          { activityId: carnival.id, taskTid: allTasks, group: carnival.day || 1 },
          'SCAvCarnivalTaskReward', 3000);
        acc._log(`[claim:carnival] Claimed ${allTasks.length} tasks`);
      } catch (e) {
        acc._log(`[claim:carnival] ${e.message}`);
      }
    },
  },

  {
    id: 'grand-login',
    name: 'Grand Login Rewards',
    async run(acc) {
      // Find grand login activity from the activity list
      const avList = acc._activityList?.avSimpleInfo || [];
      const grandAv = avList.find(a => String(a.id).startsWith('1006'));
      if (!grandAv) return;
      // Query grand state
      try {
        await sendOrReject(acc, 'CSAv', { id: [grandAv.id] }, 'SCAvGrand', 3000);
      } catch { return; }
      const grand = acc._grandData;
      if (!grand?.activityId) return;
      const claimableDays = (grand.state || []).filter(s => s.state === 1).map(s => s.tid);
      if (claimableDays.length === 0) return;
      try {
        await sendOrReject(acc, 'CSAvGrandReward',
          { activityId: grand.activityId, day: claimableDays },
          'SCAvGrandReward', 3000);
        acc._log(`[claim:grand-login] Claimed days [${claimableDays.join(',')}]`);
      } catch {}
    },
  },

  {
    id: 'biweekly-signin',
    name: 'Bi-Weekly Sign-In Milestones',
    async run(acc) {
      // Find bi-weekly activity from the activity list
      const avList = acc._activityList?.avSimpleInfo || [];
      const bwAv = avList.find(a => String(a.id).startsWith('1008'));
      if (!bwAv) return;
      // Query bi-weekly state
      try {
        await sendOrReject(acc, 'CSAv', { id: [bwAv.id] }, 'SCAvBiWeeklySignIn', 3000);
      } catch { return; }
      const biweekly = acc._biWeeklyData;
      if (!biweekly?.activityId) return;
      const gathered = new Set(biweekly.gatheredRewardIds || []);
      let claimed = 0;
      for (let tier = 1; tier <= 6; tier++) {
        if (gathered.has(tier)) continue;
        if (acc.state !== 'online') break;
        try {
          await sendOrReject(acc, 'CSAvBiWeeklySignInGatherReward',
            { activityId: biweekly.activityId, rewardId: tier },
            'SCAvBiWeeklySignIn', 2000);
          claimed++;
        } catch { break; }
      }
      if (claimed > 0) acc._log(`[claim:biweekly-signin] Claimed ${claimed} tiers`);
    },
  },

  // ── World Tree ──────────────────────────────────────────────────────────
  {
    id: 'world-tree',
    name: 'World Tree Tasks',
    async run(acc) {
      const wt = await sendOrReject(acc, 'CSWorldTree', { wordTreeId: 1 }, 'SCWorldTree', 3000);
      if (!wt?.tasks) return;
      let claimed = 0;
      for (const task of wt.tasks) {
        if (acc.state !== 'online') break;
        if (task.args > 0 && task.gatheredScore < task.args) {
          try {
            await sendOrReject(acc, 'CSWorldTreeGatherTaskReward',
              { worldTreeId: wt.id || 1, worldTreeTaskId: task.id },
              'SCWorldTreeGatherTaskReward', 2000);
            claimed++;
          } catch {}
          await sleep(50);
        }
      }
      if (claimed > 0) acc._log(`[claim:world-tree] Claimed ${claimed} tasks`);
    },
  },

  // ── Rank Tasks ──────────────────────────────────────────────────────────
  {
    id: 'rank-tasks',
    name: 'Rank Milestone Rewards',
    async run(acc) {
      let total = 0;
      for (const rankType of [16, 17, 18]) {
        const baseId = 100000 + rankType * 1000; // 116000, 117000, 118000
        for (let i = 1; i <= 25; i++) {
          if (acc.state !== 'online') break;
          try {
            await sendOrReject(acc, 'CSDrawRankTaskReward',
              { rankType, taskId: baseId + i },
              'SCDrawRankTaskReward', 1500);
            total++;
            await sleep(50);
          } catch {
            break; // Not enough score for this rank type
          }
        }
      }
      if (total > 0) acc._log(`[claim:rank-tasks] Claimed ${total} (30 diamonds each)`);
    },
  },

  // ── Free Gift Packs ─────────────────────────────────────────────────────
  {
    id: 'free-gifts',
    name: 'Free Gift Packs',
    async run(acc) {
      // Find all gift-shop activities (1003xxxx) from activity list
      const avList = acc._activityList?.avSimpleInfo || [];
      const giftActivityIds = avList
        .filter(a => String(a.id).startsWith('1003'))
        .map(a => a.id);
      if (giftActivityIds.length === 0) return;

      for (const actId of giftActivityIds) {
        if (acc.state !== 'online') break;
        try {
          // Open activity UI to get gift info
          const giftData = await sendOrReject(acc, 'CSOpenActivityUI',
            { baseActivityId: [actId], openActivityId: [actId], privilegeCard: false, diamondShop: false },
            'SCGiftInfo', 3000);
          const gifts = giftData?.giftItems || giftData?.gifts || [];
          for (const g of gifts) {
            const giftId = g.giftId;
            const price = g.origPrice || 0;
            const limit = g.purchaseLimit || 0;
            const buyTime = g.buyTime || 0;
            if (price === 0 && limit > 0 && buyTime < limit && g.currency === 3) {
              try {
                await sendOrReject(acc, 'CSReqCharge',
                  { id: giftId, type: g.avType || 1003, registerId: '', currencyType: 0, funcId: actId, buyNum: 1 },
                  'SCGiftBuyResult', 3000);
                acc._log(`[claim:free-gifts] Activity ${actId}, gift ${giftId}`);
              } catch {}
              await sleep(50);
            }
          }
        } catch {}
      }
    },
  },

  // ── Activity Sign-In ────────────────────────────────────────────────────
  {
    id: 'activity-signin',
    name: 'Activity Sign-In',
    async run(acc) {
      // Query activity state first — CSAv response is SCAvSignIn (activity data push)
      try {
        await sendOrReject(acc, 'CSAv', { id: [10180001] }, 'SCAvSignIn', 3000);
      } catch { return; } // activity not available
      try {
        await sendOrReject(acc, 'CSAvSignInSign', { id: 10180001 }, 'SCAvSignInSign', 2000);
        acc._log('[claim:activity-signin] Claimed');
      } catch {}
    },
  },

  // ── Theme Task Rewards ──────────────────────────────────────────────────
  {
    id: 'theme-tasks',
    name: 'Theme Task Rewards',
    async run(acc) {
      const avList = acc._activityList?.avSimpleInfo || [];
      const themeAv = avList.find(a => String(a.id).startsWith('1041'));
      if (!themeAv) return;
      let themeData;
      try {
        themeData = await sendOrReject(acc, 'CSAv', { id: [themeAv.id] }, 'SCAvThemeTask', 3000);
      } catch { return; }
      const tasks = (themeData?.task || []).filter(t => t.state === 1);
      if (tasks.length === 0) return;
      // Group by classId (first digit of tid × 1000 + remainder pattern)
      // All tasks share the same classId derived from their tid range
      const byClass = {};
      for (const t of tasks) {
        // classId = tid with last digit zeroed (e.g. 9011 → 9021 pattern not clear, batch all)
        const classId = Math.floor(t.tid / 10) * 10 + 1; // approximate grouping
        if (!byClass[classId]) byClass[classId] = [];
        byClass[classId].push(t.tid);
      }
      let claimed = 0;
      // Try claiming all claimable tasks at once per class, fall back to one batch
      const allTids = tasks.map(t => t.tid);
      try {
        await sendOrReject(acc, 'CSAvThemeTaskReward',
          { activityId: themeAv.id, classId: 9021, taskId: allTids },
          'SCAvThemeTaskReward', 3000);
        claimed = allTids.length;
      } catch {
        // Try individually
        for (const tid of allTids) {
          try {
            await sendOrReject(acc, 'CSAvThemeTaskReward',
              { activityId: themeAv.id, classId: 9021, taskId: [tid] },
              'SCAvThemeTaskReward', 2000);
            claimed++;
          } catch {}
          await sleep(50);
        }
      }
      if (claimed > 0) acc._log(`[claim:theme-tasks] Claimed ${claimed} tasks`);
    },
  },

  // ── Wedding Daily Main Reward ──────────────────────────────────────────
  {
    id: 'wedding-daily',
    name: 'Wedding Daily Main Reward',
    async run(acc) {
      const avList = acc._activityList?.avSimpleInfo || [];
      const weddingAv = avList.find(a => String(a.id).startsWith('1074'));
      if (!weddingAv) return;
      try {
        await sendOrReject(acc, 'CSAvWeddingDailyMainReward',
          { activityId: weddingAv.id },
          'SCAvWeddingDailyMainReward', 3000);
        acc._log('[claim:wedding-daily] Claimed');
      } catch {}
    },
  },

  // ── Map Pass Tower ──────────────────────────────────────────────────────
  {
    id: 'map-pass-tower',
    name: 'Map Pass Tower',
    async run(acc) {
      try {
        await sendOrReject(acc, 'CSMapPassTower', {}, 'SCMapPassTower', 3000);
        acc._log('[claim:map-pass-tower] Collected');
      } catch {}
    },
  },

  // ── Myths / Protagonist ─────────────────────────────────────────────────
  {
    id: 'myths-levelup',
    name: 'Myths Level-Up',
    async run(acc) {
      let leveled = 0;
      let lastLevel = 0;
      for (let i = 0; i < 200; i++) {
        if (acc.state !== 'online') break;
        try {
          const data = await sendOrReject(acc, 'CSMythsLevelUp', {}, 'SCMythsLevelUp', 2000);
          lastLevel = data?.level || 0;
          leveled++;
        } catch {
          break;
        }
      }
      if (leveled > 0) acc._log(`[claim:myths-levelup] ${leveled} ticks → Lv${lastLevel}`);
    },
  },

  {
    id: 'protagonist-rewards',
    name: 'Protagonist Attribute Rewards',
    async run(acc) {
      let claimed = 0;
      for (const attrId of [921, 922, 923, 924]) {
        for (let level = 1; level <= 10; level++) {
          if (acc.state !== 'online') break;
          try {
            await sendOrReject(acc, 'CSDrawProtagonistReward',
              { attrId, level }, 'SCDrawProtagonistRewardRsp', 2000);
            claimed++;
            await sleep(50);
          } catch {
            break; // This attr not earned yet
          }
        }
      }
      if (claimed > 0) acc._log(`[claim:protagonist-rewards] Claimed ${claimed} (100 diamonds each)`);
    },
  },

  // ── Album Star Level-Up ─────────────────────────────────────────────────
  {
    id: 'album-stars',
    name: 'Album Star Level-Up',
    async run(acc) {
      // Get unique hero TIDs from roster
      const heroes = acc._heroRoster || [];
      const tids = [...new Set(heroes.map(h => h.heroId).filter(Boolean))];
      if (tids.length === 0) return;
      let claimed = 0;
      // Multiple passes — each hero can have multiple star levels to claim
      for (let pass = 0; pass < 10; pass++) {
        let passCount = 0;
        for (const tid of tids) {
          if (acc.state !== 'online') break;
          try {
            await sendOrReject(acc, 'CSAlbumStarLvUp', { fighterTid: tid }, 'SCAlbumStarLvUp', 2000);
            claimed++;
            passCount++;
          } catch {}
        }
        if (passCount === 0) break;
      }
      if (claimed > 0) acc._log(`[claim:album-stars] Upgraded ${claimed} star levels`);
    },
  },

  // ── Task Re-Check (chain completions) ───────────────────────────────────
  {
    id: 'task-recheck',
    name: 'Chain Task Re-Check',
    async run(acc) {
      await claimAllTaskTypes(acc);
    },
  },
];

// Helper: claim daily/weekly/lifetime milestones
// 1. Fire CSTaskAwardOnKey for all 3 types (fire-and-forget)
// 2. Wait 1s, then fetch task state via CSReqActiveTask → SCRspActiveTask
// 3. Per group, batch activeIds where activeStatus === 0 and send CSReqActiveNumAward
async function claimAllTaskTypes(acc) {
  // Step 1: fire all 3 trigger packets
  for (const type of [1, 2, 3]) {
    if (acc.state !== 'online') return;
    try { acc.client.send('CSTaskAwardOnKey', { type, taskId: [] }); } catch {}
  }
  await sleep(1000);

  // Step 2: fetch task state
  if (acc.state !== 'online') return;
  let taskData;
  try {
    const resp = await acc.sendAndWait('CSReqActiveTask', {}, 'SCRspActiveTask', 5000);
    taskData = resp.data;
  } catch { return; }
  acc._taskData = taskData;
  if (!taskData?.tasks) return;

  // Step 3: per group, send 2 CSReqActiveNumAward packets — one for activeStatus===1, one for ===0
  let totalClaimed = 0;
  for (const grp of taskData.tasks) {
    const type = grp.taskType;
    if (![1, 2, 3].includes(type)) continue;
    for (const status of [1, 0]) {
      const activeIds = (grp.activeStatus || [])
        .filter(a => a.activeStatus === status)
        .map(a => a.activeId);
      if (activeIds.length === 0) continue;
      try {
        acc.client.send('CSReqActiveNumAward', { taskType: type, activeId: activeIds });
        totalClaimed += activeIds.length;
      } catch {}
      await sleep(30);
    }
  }
  if (totalClaimed > 0) acc._log(`[claim:tasks] Sent ${totalClaimed} active milestone claims`);
}

// ── Runner ──────────────────────────────────────────────────────────────────
// Runs all (or filtered) claim handlers in order
export async function claimAll(acc, opts = {}) {
  const { only, skip } = opts;
  acc._claimingRewards = true;
  acc._lastClaimTime = Date.now();

  try {
    for (const claim of CLAIMS) {
      if (acc.state !== 'online') break;
      if (only && !only.includes(claim.id)) continue;
      if (skip && skip.includes(claim.id)) continue;

      try {
        await claim.run(acc);
      } catch (e) {
        acc._log(`[claim:${claim.id}] Failed: ${e.message}`);
      }
    }
  } finally {
    acc._claimingRewards = false;
  }
}

// List available claim IDs (for dashboard/debugging)
export function listClaims() {
  return CLAIMS.map(c => ({ id: c.id, name: c.name }));
}

// Claim daily (1), weekly (2), lifetime (3) tasks + activeNum milestones
export async function claimDailyTasks(acc) {
  if (acc.state !== 'online') return { ok: false };
  await claimAllTaskTypes(acc);
  return { ok: true };
}

// Claim daily/weekly/lifetime tasks for all online accounts in parallel
export async function claimDailyAll(manager) {
  const results = [];
  const accounts = [...manager.accounts.values()].filter(a => a.state === 'online');
  const promises = accounts.map(async (acc) => {
    const r = await claimDailyTasks(acc);
    results.push({ id: acc.id, email: acc.email, ...r });
  });
  await Promise.all(promises);
  return results;
}
