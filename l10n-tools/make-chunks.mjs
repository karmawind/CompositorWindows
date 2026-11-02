// 从 strings.json 生成分块的待翻译列表 chunk-NN.json
import fs from 'node:fs';
import path from 'node:path';

const root = 'G:/CompositorZH';
const outDir = path.join(root, 'l10n-tools/out');
const entries = JSON.parse(fs.readFileSync(path.join(outDir, 'strings.json'), 'utf8'));

const skipRe = [
  /^[\w.:+\-\/\\]+$/,               // 无空白的标识符/路径/键 (camelCase, snake_case, 文件名)
  /^[A-Z0-9]{1,6}$/,                // 短全大写: RGB, PNG, DPI...
  /^:+\//,                          // :/assets/...  Qt资源路径
  /:\/\//,                          // URL
  /^[a-z0-9]+(\.[a-z0-9]+)+$/i,     // layer.visible 型
];

function obviousIdentifier(s, ui) {
  if (!ui) return false; // 非 UI 的由句子启发式决定
  if (s.includes(' ')) return false;
  return skipRe.some(re => re.test(s)) && !/^[A-Z][a-z]{2,}/.test(s) === false ? false : skipRe.some(re => re.test(s)) && !/^[A-Z][a-z]+( [A-Za-z…&()%°]+)*$/.test(s);
}
// 简化: 无空格且匹配 skipRe 的直接视为不可译
function isSkip(s) { return skipRe.some(re => re.test(s)); }

const picked = [];
for (const e of entries) {
  const s = e.s;
  if (e.ui) {
    if (s.includes(' ') || /[…&?]/.test(s) || /^[A-Z][a-z]{2,}$/.test(s)) {
      if (!isSkip(s)) { picked.push(e); continue; }
    } else {
      // 无空格短词，若像英文单词(纯字母)也翻译，如 Layers/Tools/Undo
      if (/^[A-Za-z][A-Za-z'&…°]+$/.test(s) && !isSkip(s)) { picked.push(e); continue; }
    }
  } else {
    // 非 UI：只收含空格的完整句子(可能出现在错误弹窗)
    if (s.includes(' ') && /^[A-Z]/.test(s) && /[a-z]{3}/.test(s) && !/[=<>]/.test(s) && !isSkip(s)) picked.push(e);
  }
}

// 去重已保证；按 UI 优先排序
picked.sort((a, b) => (b.ui - a.ui) || b.n - a.n);
const CHUNK = 200;
const chunks = [];
for (let i = 0; i < picked.length; i += CHUNK) {
  chunks.push(picked.slice(i, i + CHUNK));
}
chunks.forEach((c, i) => {
  fs.writeFileSync(path.join(outDir, `chunk-${String(i + 1).padStart(2, '0')}.json`),
    JSON.stringify(c.map(e => ({ s: e.s, ctx: e.f.slice(0, 3).join(', ') })), null, 0), 'utf8');
});
fs.writeFileSync(path.join(outDir, 'picked-count.txt'), String(picked.length), 'utf8');
console.log('picked:', picked.length, 'chunks:', chunks.length);
console.log('ui picked:', picked.filter(e => e.ui).length, 'non-ui picked:', picked.filter(e => !e.ui).length);
