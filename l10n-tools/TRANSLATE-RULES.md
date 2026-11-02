# Compositor 汉化翻译规则（务必遵守）

你拿到的是一款 **Windows 桌面图像编辑器**（类 Photoshop：图层/蒙版/画笔/滤镜/选区/调整）界面字符串。
把 chunk 文件中的每个英文键翻译为**简体中文（中国大陆，软件界面风格）**，术语向 **Photoshop 中文版**看齐。

## 输出格式
- 输出 JSON 对象写入指定 dict 文件：`{"<原文精确字符>": "<译文>", ...}`
- **键必须与输入完全一致**（逐字节），包括 `&`、`…`、`%1`、`\n`、`\"`、括号空格等。绝不能改键。
- 不该翻译的（见下方"跳过规则"）值为 `null`。
- 每个键都必须出现，不允许遗漏。

## 跳过规则（值 = null）
1. 标识符/对象名/设置键：如 `layerBlend`、`viewRulers`、`tools`、`textFont`、`automaticReleaseCheck`（camelCase 或全小写无空格单词，且不是常见英文单词的界面标签）。
2. 文件扩展名/格式名：`png`、`jpg`、`jpeg`、`comp`、`exr`、`icc`、`hdr`、`psd`。
3. 包含路径或 URL：含 `/`、`\\`、`://`、`mailto`、`.com`。
4. 颜色十六进制、数字、单字母（`X`、`Y`、`W`、`H`）→ `X`/`Y` 保留 null；但 `Width`/`Height` 要译。
5. JSON/manifest 字段名：含 `.` 的驼峰（`layer.visible`）。
6. 纯快捷键表示法（`Ctrl+Shift+P`、`Alt+F4`）。
7. 测试用断言消息（通常是长句、含 `expected`、`should`、`must be`、`!=`、`==`）——若上下文字段显示在 `tests/` 目录或明显是开发者日志（含 `zoom_units=`、`ms=` 等技术指标）→ null。

## 保留规则
- 占位符原样保留：`%1` `%2` `%n`，转义序列 `\n` `\"` `\\` 原样保留。
- HTML 标签原样保留（`<b>` `</b>` `<br>` 等），只译标签之间的文字。
- 专有名词不译：Compositor、Photoshop、GitHub、Qt、ONNX、BiRefNet、OpenEXR、LibRaw、Inter。
- 格式/技术名词不译：PNG、JPEG、JP2、TIFF、HEIC、HEIF、AVIF、PSD、PSB、SVG、RAW、EXR、HDR、RGB、RGBA、sRGB、CMYK、DPI、PPI、LUT、SM、GPU、CPU、MSAA。
- 单位不译：px、MB、GB、KB、ms、DPI。
- 快捷键字母括号保留：`Brush (B)` → `画笔 (B)`；`Zoom (Z)` → `缩放 (Z)`。
- 菜单助记符 `&X`：译成 `中文(&X)`。如 `&File` → `文件(&F)`；`&Import Image…` → `导入图像(&I)…`。`&` 后的字母保持原字母。
- 省略号：英文 `…` 用中文 `…`；结尾标点用中文全角（`。`不做句尾也行，界面标签通常不加句号；完整句子用中文标点）。

## 术语表（必须统一）
| EN | ZH |
|---|---|
| Layer(s) | 图层 |
| Mask | 蒙版 |
| Canvas | 画布 |
| Selection | 选区 |
| Brush | 画笔 |
| Eraser | 橡皮擦 |
| Pencil | 铅笔 |
| Fill | 填充 |
| Gradient | 渐变 |
| Shape | 形状 |
| Type / Text (工具) | 文本 |
| Filter(s) | 滤镜 |
| Adjustment(s) | 调整 |
| Blend mode | 混合模式 |
| Opacity | 不透明度 |
| Transform | 变换 |
| Flip | 翻转 |
| Rotate | 旋转 |
| Crop | 裁剪 |
| Scale | 缩放 |
| Move (工具) | 移动 |
| Marquee | 选框 |
| Lasso | 套索 |
| Wand | 魔棒 |
| Eyedropper | 吸管 |
| Heal / Spot Healing | 修复 |
| Clone (Stamp) | 仿制图章 |
| Retouch | 润饰 |
| Zoom | 缩放 |
| Hand (工具) | 抓手 |
| Rulers | 标尺 |
| Guides | 参考线 |
| Snap | 对齐 |
| Grid | 网格 |
| History | 历史记录 |
| Undo / Redo | 撤销 / 重做 |
| Export / Import | 导出 / 导入 |
| Save / Open / Close | 保存 / 打开 / 关闭 |
| Project | 项目 |
| Foreground / Background (颜色) | 前景色 / 背景色 |
| Hue / Saturation / Brightness | 色相 / 饱和度 / 亮度 |
| Contrast | 对比度 |
| Exposure | 曝光 |
| Temperature / Tint | 色温 / 色调 |
| Noise Reduction | 降噪 |
| Sharpen | 锐化 |
| Blur | 模糊 |
| Gaussian | 高斯 |
| Levels | 色阶 |
| Curves | 曲线 |
| Black & White | 黑白 |
| Color Balance | 色彩平衡 |
| Invert | 反相 |
| Threshold | 阈值 |
| Posterize | 色调分离 |
| Feather | 羽化 |
| Stroke | 描边 |
| Shadow / Highlight / Midtone | 阴影 / 高光 / 中间调 |
| Clipping mask | 剪贴蒙版 |
| Group (图层组) | 编组 |
| Merge | 合并 |
| Flatten | 拼合 |
| Rasterize | 栅格化 |
| Subject (主体抠图) | 主体 |
| Background removal | 去除背景 |
| Export Image | 导出图像 |
| Import Image | 导入图像 |
| New Canvas | 新建画布 |
| Welcome | 欢迎 |
| Palette | 色板 |
| Swatch | 色块 |
| Dock / Panel | 面板 |
| Status bar | 状态栏 |
| Tooltip 语气 | 简洁命令式 |
| Focus Stack | 焦点堆栈 |
| Batch | 批量 |
| Preset | 预设 |
| Shortcut | 快捷键 |
| Command Palette | 命令面板 |
| Navigator | 导航器 |
| Full Screen | 全屏 |
| Anti-alias | 消除锯齿 |
| Tolerance | 容差 |
| Contiguous | 连续 |
| Hardness | 硬度 |
| Flow | 流量 |
| Spacing | 间距 |
| Size (笔刷) | 大小 |
| Angle | 角度 |
| Width / Height | 宽度 / 高度 |
| Radius | 半径 |
| Strength | 强度 |
| Opacity slider | 不透明度滑块 |

## 语气与风格
- 界面标签：短、名词化，不加句号。`Duplicate Layer` → `复制图层`。
- 按钮：动词开头。`Cancel` → `取消`；`Continue` → `继续`；`Skip This Version` → `跳过此版本`；`Later` → `稍后提醒`。
- 完整句子（状态栏/提示/警告）：自然流畅中文，可用中文标点。
- `Untitled` → `未命名`；`Save changes before closing?` → `关闭前保存更改吗？`
- 尽量简短，菜单项一般 ≤6 字。
