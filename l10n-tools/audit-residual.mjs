// 审计:全 src 扫描,列出仍未翻译的用户可见英文串(含所有子目录)
import fs from 'node:fs';
import path from 'node:path';

const root = 'G:/CompositorZH';
const outDir = path.join(root, 'l10n-tools/out');

const dict = new Set();
for (const f of fs.readdirSync(outDir).filter(f => /^dict-.*\.json$/.test(f))) {
  const obj = JSON.parse(fs.readFileSync(path.join(outDir, f), 'utf8'));
  for (const [k, v] of Object.entries(obj)) if (v !== null && typeof v === 'string' && v.trim()) dict.add(k);
}

const litRe = /"(?:[^"\\\n]|\\.)*"/g;
const uiHit = /(addAction|addMenu|QLabel|setWindowTitle|setToolTip|setPlaceholderText|QMessageBox|setText|setAccessibleName|showMessage|QInputDialog|addItem|QPushButton|QCheckBox|setTabText|QDockWidget|setTitle|warning|critical|information|about\()/;

const residual = new Map();
const walk = dir => {
  for (const name of fs.readdirSync(dir)) {
    const p = path.join(dir, name);
    const st = fs.statSync(p);
    if (st.isDirectory()) { if (name === 'third_party' || name.startsWith('.')) continue; walk(p); continue; }
    if (!/\.(cpp|h)$/.test(p)) continue;
    const src = fs.readFileSync(p, 'utf8');
    // 去掉原始字符串
    const cleaned = src.replace(/R"[^"]*"/g, '""');
    for (const m of cleaned.matchAll(litRe)) {
      const inner = m[0].slice(1, -1);
      if (dict.has(inner)) continue;
      if (inner.length < 3 || !/[A-Za-z]/.test(inner)) continue;
      if (!/[A-Za-z]{3}/.test(inner)) continue;
      if (/^[\w.:+\-\/\\]+$/.test(inner) && !inner.includes(' ')) continue; // 标识符
      // 上下文窗口里要有 UI 调用痕迹
      const before = cleaned.slice(Math.max(0, m.index - 260), m.index);
      const after = cleaned.slice(m.index + m[0].length, m.index + m[0].length + 160);
      if (!uiHit.test(before) && !uiHit.test(after)) continue;
      const key = inner;
      if (!residual.has(key)) residual.set(key, { n: 0, f: new Set() });
      residual.get(key).n++;
      residual.get(key).f.add(path.relative(root, p).replace(/\\/g, '/'));
    }
  }
};
walk(path.join(root, 'src'));

const arr = [...residual.entries()].map(([s, v]) => ({ s, n: v.n, f: [...v.f].slice(0, 4) }))
  .sort((a, b) => b.n - a.n);
fs.writeFileSync(path.join(outDir, 'residual.json'), JSON.stringify(arr, null, 1), 'utf8');
console.log('残留待译:', arr.length);
console.log(arr.slice(0, 30).map(e => `  ${e.n}x ${JSON.stringify(e.s)} @${e.f[0]}`).join('\n'));
