// 审计:所有 action(...)/bindCommand 标签经新 normalized() 后必须能在命令目录中找到
import fs from 'node:fs';
import path from 'node:path';

const root = 'G:/CompositorZH';
const regSrc = fs.readFileSync(path.join(root, 'src/ui/CommandRegistry.cpp'), 'utf8');

// 1. 提取目录条目 add("id","菜单","标签",...)
const catalog = new Map();
for (const m of regSrc.matchAll(/add\("([^"]+)","([^"]+)","([^"]*)"/g)) {
  catalog.set(m[2] + '\u0000' + m[3], m[1]);
}

// 2. 模拟 normalized()
function norm(v) {
  v = v.replace(/[（(]\s*&\s*[A-Za-z0-9]\s*[）)]\s*$/, '');
  v = v.replace(/&/g, '');
  v = v.replace(/\s*(…|\.\.\.)\s*$/, '');
  return v.trim();
}

// 3. 从 MainWindow.cpp 提取 action(menu,"Label",...) 调用与 addMenu 标题配对
const mw = fs.readFileSync(path.join(root, 'src/ui/MainWindow.cpp'), 'utf8');
const used = [];
{
  // 逐段扫描: addMenu("&X") 之后同一行/后续 action(x,"...")
  const lines = mw.split('\n').join('\n');
  // 简化:找 action(<var>,"标签" 形式,菜单变量已知映射
  const menuVars = {};
  for (const m of lines.matchAll(/auto\*(\w+)=menuBar\(\)->addMenu\("([^"]+)"\)/g)) menuVars[m[1]] = m[2];
  for (const m of lines.matchAll(/\baction\((\w+),"((?:[^"\\]|\\.)*)"/g)) {
    const menu = menuVars[m[1]];
    if (menu !== undefined) used.push({ menu, label: m[2], where: 'MainWindow action()' });
  }
}

// 4. 其他 commandSpec 直接调用点(字面量)
for (const f of ['FileActions.cpp', 'VisualStyle.cpp', 'LayerPanel.cpp']) {
  const src = fs.readFileSync(path.join(root, 'src/ui', f), 'utf8');
  for (const m of src.matchAll(/commandSpec\("([^"]+)"\s*,\s*"([^"]+)"\)/g)) {
    used.push({ menu: m[1], label: m[2], where: f + ' commandSpec()' });
  }
}

// 5. 工具条目 bindCommand(a,"Tools",entry.label) 用目录里 Tools 菜单条目核对
const toolEntries = [...catalog.keys()].filter(k => k.startsWith('Tools\u0000'));
console.log('目录条目总数:', catalog.size, '| Tools 条目:', toolEntries.length, '| 抽查绑定标签:', used.length);

let bad = 0;
for (const u of used) {
  const key = norm(u.menu) + '\u0000' + norm(u.label);
  if (!catalog.has(key) && !catalog.has(u.menu + '\u0000' + u.label)) {
    bad++;
    console.log('未匹配:', JSON.stringify(u.menu), '+', JSON.stringify(u.label), '@', u.where);
  }
}
console.log(bad === 0 ? '✓ 全部绑定都能匹配命令目录' : '✗ 存在 ' + bad + ' 个失配');
