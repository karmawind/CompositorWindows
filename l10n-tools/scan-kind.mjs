import fs from 'node:fs';
import path from 'node:path';
const root = 'G:/CompositorZH/src';
const hits = [];
function scan(d) {
  for (const n of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, n.name);
    if (n.isDirectory()) { if (n.name !== 'third_party') scan(p); continue; }
    if (!/\.(cpp|h)$/.test(n.name)) continue;
    const s = fs.readFileSync(p, 'utf8');
    for (const m of s.matchAll(/.{50}kind.{70}/g)) hits.push(path.relative('G:/CompositorZH', p).replace(/\\/g, '/') + ' :: ' + m[0].replace(/\n/g, ' '));
  }
}
scan(root);
console.log(hits.join('\n'));
console.log('total', hits.length);
