const d = require('./data/msgdump.json');
const o = require('./data/overrides.json');

// Check all messages for duplicate field numbers after applying overrides
let totalDupes = 0;
for (const [msgName, schema] of Object.entries(d.schemas)) {
  const fields = JSON.parse(JSON.stringify(schema.fields));
  const ov = o[msgName];
  if (ov) {
    for (const [fn, ovData] of Object.entries(ov)) {
      const field = fields.find(f => f.name === fn);
      if (field && ovData.num !== undefined) field.n = ovData.num;
    }
  }
  const nums = fields.map(f => f.n);
  const seen = new Set();
  for (const n of nums) {
    if (seen.has(n)) {
      const dupeFields = fields.filter(f => f.n === n).map(f => f.name);
      console.log(`${msgName}: duplicate field number ${n} → ${dupeFields.join(', ')}`);
      totalDupes++;
    }
    seen.add(n);
  }
}
console.log(`\nTotal messages with dupes: ${totalDupes}`);
