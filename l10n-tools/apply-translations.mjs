// 合并 dict-*.json 并把译文精确替换进源码字符串字面量
// 用法: node apply-translations.mjs [--dry]
import fs from 'node:fs';
import path from 'node:path';

const root = 'G:/CompositorZH';
const outDir = path.join(root, 'l10n-tools/out');
const dry = process.argv.includes('--dry');

// 1. 合并字典
const dict = new Map();
const dictFiles = fs.readdirSync(outDir).filter(f => /^dict-.*\.json$/.test(f));
for (const f of dictFiles) {
  const obj = JSON.parse(fs.readFileSync(path.join(outDir, f), 'utf8'));
  for (const [k, v] of Object.entries(obj)) {
    if (v === null) continue;
    if (typeof v !== 'string' || !v.trim()) continue;
    if (dict.has(k) && dict.get(k) !== v) console.log('冲突(取首个):', JSON.stringify(k));
    else if (!dict.has(k)) dict.set(k, v);
  }
}
console.log('字典条目:', dict.size, '来自', dictFiles.length, '个文件');

// 2. 把 JSON 值里的控制字符转换为 C++ 源码转义；已有转义序列保持
function cppEscape(value) {
  let out = '';
  for (let i = 0; i < value.length; i++) {
    const ch = value[i];
    if (ch === '\n') out += '\\n';
    else if (ch === '\r') out += '\\r';
    else if (ch === '\t') out += '\\t';
    else if (ch === '"') out += '\\"';
    else if (ch === '\\') {
      const next = value[i + 1];
      // 保留常见 C 转义与 Qt 占位符 %；其余双写反斜杠
      if (next && /[nrt"\\0]/.test(next)) { out += ch + next; i++; }
      else out += '\\\\';
    } else out += ch;
  }
  return out;
}

// 3. 扫描源码并按字面量整体替换
const dirs = ['src/ui', 'src/update', 'src/persistence', 'src/editing', 'src/imaging', 'src/platform',
  'src/core', 'src/editing_transform', 'src/effects', 'src/effects_tools', 'src/filters', 'src/graphics',
  'src/layers', 'src/render', 'src/retouch', 'src/text'];
const rawRe = /R"([^"\s]*)\(/g; // 原始字符串起点标记，逐段跳过太复杂 → 逐字面量处理时检查
const litRe = /"(?:[^"\\\n]|\\.)*"/g;

let totalRepl = 0, fileCount = 0;
const perFile = [];
for (const d of dirs) {
  const abs = path.join(root, d);
  if (!fs.existsSync(abs)) continue;
  const walk = dir => {
    for (const name of fs.readdirSync(dir)) {
      const p = path.join(dir, name);
      const st = fs.statSync(p);
      if (st.isDirectory()) { walk(p); continue; }
      if (!/\.(cpp|h)$/.test(p)) continue;
      const src = fs.readFileSync(p, 'utf8');
      let replaced = 0;
      // 找出原始字符串区间，跳过其中的字面量
      const rawRanges = [];
      const rawTok = /"([^"\\\n]*)\(/.source;
      let rm;
      const rawStartRe = /R"/g;
      while ((rm = rawStartRe.exec(src)) !== null) {
        // R"delim( ... )delim"
        const m = /^R"([^()\s]*)\(/.exec(src.slice(rm.index));
        if (!m) continue;
        const close = ')' + m[1] + '"';
        const end = src.indexOf(close, rm.index + m[0].length);
        if (end < 0) continue;
        rawRanges.push([rm.index, end + close.length]);
      }
      const inRaw = i => rawRanges.some(([a, b]) => i >= a && i < b);
      const out = src.replace(litRe, (whole, offset) => {
        if (inRaw(offset)) return whole;
        const inner = whole.slice(1, -1);
        if (!dict.has(inner)) return whole;
        const translated = cppEscape(dict.get(inner));
        replaced++;
        return '"' + translated + '"';
      });
      if (replaced > 0 && !dry) fs.writeFileSync(p, out, 'utf8');
      if (replaced > 0) { totalRepl += replaced; fileCount++; perFile.push(`${path.relative(root, p)}: ${replaced}`); }
    }
  };
  walk(abs);
}
console.log(dry ? '[dry] ' : '', '替换总数:', totalRepl, '文件数:', fileCount);
fs.writeFileSync(path.join(outDir, 'apply-report.txt'), perFile.join('\n'), 'utf8');
