// 提取 src 下用户可见的字符串字面量 → out/strings.json
import fs from 'node:fs';
import path from 'node:path';

const root = 'G:/CompositorZH';
const dirs = ['src/ui', 'src/update', 'src/persistence', 'src/editing', 'src/imaging', 'src/platform'];
const files = [];
for (const d of dirs) {
  const abs = path.join(root, d);
  if (!fs.existsSync(abs)) continue;
  const walk = dir => {
    for (const name of fs.readdirSync(dir)) {
      const p = path.join(dir, name);
      const st = fs.statSync(p);
      if (st.isDirectory()) walk(p);
      else if (/\.(cpp|h)$/.test(p)) files.push(p);
    }
  };
  walk(abs);
}

const map = new Map();
const rawRe = /R"[^"]*"/g;
const litRe = /"(?:[^"\\\n]|\\.)*"/g;

for (const p of files) {
  let src = fs.readFileSync(p, 'utf8');
  src = src.replace(rawRe, '""');
  for (const m of src.matchAll(litRe)) {
    const lit = m[0].slice(1, -1);
    if (lit.length < 2) continue;
    if (!/[A-Za-z]/.test(lit)) continue;
    if (!map.has(lit)) map.set(lit, { count: 0, files: new Set() });
    const e = map.get(lit);
    e.count++; e.files.add(path.relative(root, p).replace(/\\/g, '/'));
  }
}

const uiOnly = f => f.startsWith('src/ui') || f.startsWith('src/update');
const entries = [...map.entries()].map(([lit, info]) => ({
  s: lit,
  n: info.count,
  ui: [...info.files].some(uiOnly),
  f: [...info.files].slice(0, 5),
})).sort((a, b) => b.n - a.n || a.s.localeCompare(b.s));

fs.mkdirSync(path.join(root, 'l10n-tools/out'), { recursive: true });
fs.writeFileSync(path.join(root, 'l10n-tools/out/strings.json'), JSON.stringify(entries, null, 1), 'utf8');
console.log('files:', files.length);
console.log('unique:', entries.length);
console.log('ui-flagged:', entries.filter(e => e.ui).length);
console.log('total-occurrences:', entries.reduce((s, e) => s + e.n, 0));
