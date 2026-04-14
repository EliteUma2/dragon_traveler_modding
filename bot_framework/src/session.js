// Session analyzer — enriches recorded packet sessions with resolved game data names
import { getGameDB } from './gamedb.js';
import { readFileSync, writeFileSync } from 'fs';

export function analyzeSession(sessionPath, opts = {}) {
  const raw = JSON.parse(readFileSync(sessionPath, 'utf-8'));
  const db = getGameDB();
  const packets = raw.packets || raw;

  const analysis = {
    recorded: raw.recorded,
    duration: raw.duration,
    packetCount: raw.packetCount,
    c2s: raw.c2s,
    s2c: raw.s2c,
    loginFlow: [],
    player: null,
    heroes: {},
    items: {},
    equipment: {},
    currencies: {},
    timeline: [],
  };

  // Collect all IDs for batch resolution
  const heroIds = new Set();
  const itemIds = new Set();
  const equipIds = new Set();

  for (const pkt of packets) {
    const entry = {
      t: pkt.t,
      dir: pkt.dir,
      name: pkt.name,
      msgId: pkt.msgId,
      size: pkt.size,
    };

    // Login flow packets (msgId 1001-1011)
    if (pkt.msgId >= 1001 && pkt.msgId <= 1011) {
      analysis.loginFlow.push(entry);
    }

    // Extract player info from SCLogin
    if (pkt.name === 'SCLogin' && pkt.data?.player) {
      const p = pkt.data.player;
      analysis.player = {
        name: p.name,
        level: p.level,
        exp: p.exp,
        fightPower: p.fightPower,
        curHeadIcon: p.curHeadIcon,
        currencies: (p.currency || []).map(c => {
          itemIds.add(c.id);
          return { id: c.id, count: c.count, name: db.currencyName(c.id) };
        }),
      };
    }

    // Extract items from SCBagMsg
    if (pkt.name === 'SCBagMsg' && pkt.data?.itemBagMsg?.items) {
      for (const item of pkt.data.itemBagMsg.items) {
        itemIds.add(item.itemId);
        analysis.items[item.itemId] = {
          name: db.itemName(item.itemId),
          count: item.itemNum,
          key: item.itemKey,
        };
      }
    }

    // Extract equipment from SCEquipmentBagMsg
    if (pkt.name === 'SCEquipmentBagMsg' && pkt.data?.equipBagMsg?.equipments) {
      for (const eq of pkt.data.equipBagMsg.equipments) {
        equipIds.add(eq.templateId);
        analysis.equipment[eq.templateId] = {
          name: db.equipName(eq.templateId),
          count: eq.num,
        };
      }
    }

    // Extract heroes from SCFighterBagMsg
    if (pkt.name === 'SCFighterBagMsg' && pkt.data?.f1) {
      const bag = pkt.data.f1;
      // f1 = hero roster (f2 = heroId)
      if (bag.f1) {
        for (const h of bag.f1) {
          const heroId = h.f2;
          if (heroId) {
            heroIds.add(heroId);
            analysis.heroes[heroId] = {
              name: db.heroName(heroId),
              info: db.heroInfo(heroId),
              slot: h.f1,
              star: h.f3,
            };
          }
        }
      }
      // f5 = hero details (f1 = heroId, f5 = fightPower)
      if (bag.f5) {
        for (const h of bag.f5) {
          const heroId = h.f1;
          if (heroId && analysis.heroes[heroId]) {
            analysis.heroes[heroId].fightPower = h.f5;
            analysis.heroes[heroId].level = h.f4;
          }
        }
      }
      // f7 = team lineup
      if (bag.f7) {
        analysis.teamLineup = bag.f7.map(id => ({
          id,
          name: db.heroName(id),
        }));
      }
    }

    // Build timeline of all C2S actions (what the player did)
    if (pkt.dir === 'C2S') {
      analysis.timeline.push({
        t: pkt.t,
        name: pkt.name,
        msgId: pkt.msgId,
        data: pkt.data,
      });
    }
  }

  // Summarize
  analysis.summary = {
    loginPackets: analysis.loginFlow.map(p => p.name),
    heroCount: Object.keys(analysis.heroes).length,
    itemCount: Object.keys(analysis.items).length,
    equipCount: Object.keys(analysis.equipment).length,
    playerActions: analysis.timeline.map(t => t.name),
  };

  return analysis;
}

// CLI usage
if (process.argv[1] && process.argv[1].endsWith('session.js') && process.argv[2]) {
  const result = analyzeSession(process.argv[2]);
  const outPath = process.argv[3] || process.argv[2].replace('.json', '.analyzed.json');
  writeFileSync(outPath, JSON.stringify(result, null, 2));
  console.log(`Analyzed ${result.packetCount} packets → ${outPath}`);
  console.log(`Player: ${result.player?.name} (Lv${result.player?.level})`);
  console.log(`Heroes: ${Object.values(result.heroes).map(h => h.name).join(', ')}`);
  console.log(`Items: ${Object.keys(result.items).length} types`);
  console.log(`Actions: ${result.timeline.length} C2S packets`);
}
