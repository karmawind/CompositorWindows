// 提取第二轮:纯单词类 UI 标签(无空格、无&、字母组成),仍未翻译的
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
const uiHit = /(addAction|addMenu|QLabel|setWindowTitle|setToolTip|setPlaceholderText|QMessageBox|setText|setAccessibleName|showMessage|addItem|QDockWidget|setTitle|QComboBox|addTab|setTabText|setButtonText|QMessageBox|QInputDialog|QCheckBox|QRadioButton|warning|information)/;

const found = new Map();
const walk = dir => {
  for (const name of fs.readdirSync(dir)) {
    const p = path.join(dir, name);
    const st = fs.statSync(p);
    if (st.isDirectory()) { if (name === 'third_party') continue; walk(p); continue; }
    if (!/\.(cpp|h)$/.test(p)) continue;
    const src = fs.readFileSync(p, 'utf8').replace(/R"[^"]*"/g, '""');
    for (const m of src.matchAll(litRe)) {
      const inner = m[0].slice(1, -1);
      // 纯字母单词(可含连字符),非 camelCase 标识符,未翻译
      if (!/^[A-Za-z][A-Za-z\-]*$/.test(inner)) continue;
      if (dict.has(inner)) continue;
      if (inner.length < 2 || inner.length > 22) continue;
      // 排除 camelCase(小写后跟大写)与全大写缩写与全小写
      if (/[a-z][A-Z]/.test(inner)) continue;          // camelCase
      if (/^[a-z]+$/.test(inner)) continue;             // 全小写多为标识符
      if (/^[A-Z]{2,}$/.test(inner)) continue;          // RGB/PNG 等缩写
      if (!/[A-Z][a-z]/.test(inner)) continue;          // 必须是首字母大写单词
      const before = src.slice(Math.max(0, m.index - 300), m.index);
      const after = src.slice(m.index + m[0].length, m.index + m[0].length + 180);
      if (!uiHit.test(before) && !uiHit.test(after)) continue;
      if (!found.has(inner)) found.set(inner, new Set());
      found.get(inner).add(path.relative(root, p).replace(/\\/g, '/'));
    }
  }
};
walk(path.join(root, 'src/ui'));
walk(path.join(root, 'src/update'));

const arr = [...found.entries()].map(([s, f]) => ({ s, f: [...f].slice(0, 3) })).sort((a, b) => a.s.localeCompare(b.s));
fs.writeFileSync(path.join(outDir, 'bare-words.json'), JSON.stringify(arr, null, 1), 'utf8');
console.log('纯单词待译:', arr.length);
console.log(arr.map(e => e.s).join(' | '));
