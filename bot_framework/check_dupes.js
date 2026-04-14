import fs from 'fs';
const d = JSON.parse(fs.readFileSync('E:/Users/Admin/packet modifier/task_reward_claim_and_level_strategy_gear.json', 'utf8'));
const bag = d.packets.find(p => p.name === 'SCFighterBagMsg');
const fighters = bag.data?.fighterBagMsg?.baseFighters || [];
const byHero = {};
for (const f of fighters) {
  const hid = f.heroId;
  if (!byHero[hid]) byHero[hid] = [];
  byHero[hid].push({ id: f.id, heroId: f.heroId, star: f.star || f.starLv || 3 });
}
for (const [hid, copies] of Object.entries(byHero)) {
  if (copies.length > 1) console.log('Dup heroId', hid, ':', copies.map(c => 'instId=' + c.id + '/star=' + c.star).join(', '));
}
console.log('Total fighters:', fighters.length);
