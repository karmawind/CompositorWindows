// 序列化兼容性修复:读侧双语、写侧英文、显示修正
import fs from 'node:fs';

const file = (p) => 'G:/CompositorZH/' + p;
let count = 0;
function patch(path, pairs, replaceAll = false) {
  let s = fs.readFileSync(file(path), 'utf8');
  for (const [oldStr, newStr] of pairs) {
    if (!s.includes(oldStr)) { console.log('MISS: ' + path + ' :: ' + oldStr.slice(0, 70)); continue; }
    s = replaceAll ? s.split(oldStr).join(newStr) : s.replace(oldStr, newStr);
    count++;
  }
  fs.writeFileSync(file(path), s, 'utf8');
}

// ── Adjustments.cpp: kinds/ranges 双语索引 ──
patch('src/effects/Adjustments.cpp', [
  [`constexpr std::array<const char*,12> kinds{"Hue/Saturation","色阶","曲线","曝光","渐变映射","颗粒","反相","黑白","色彩平衡","高斯模糊","动感模糊","添加杂色"};`,
   `constexpr std::array<const char*,12> kinds{"Hue/Saturation","色阶","曲线","曝光","渐变映射","颗粒","反相","黑白","色彩平衡","高斯模糊","动感模糊","添加杂色"};\nconstexpr std::array<const char*,12> kindsEn{"Hue/Saturation","Levels","Curves","Exposure","Gradient Map","Grain","Invert","Black & White","Color Balance","Gaussian Blur","Motion Blur","Add Noise"};`],
  [`constexpr std::array<const char*,7> ranges{"主","红色","黄色","绿色","青色","蓝色","洋红"};`,
   `constexpr std::array<const char*,7> ranges{"主","红色","黄色","绿色","青色","蓝色","洋红"};\nconstexpr std::array<const char*,7> rangesEn{"Master","Reds","Yellows","Greens","Cyans","Blues","Magentas"};`],
  [`template<size_t N>size_t named(QJsonValue v,const std::array<const char*,N>&names){if(!v.isString())bad();auto i=std::find_if(names.begin(),names.end(),[&](auto n){return v.toString()==n;});if(i==names.end())bad();return size_t(i-names.begin());}`,
   `template<size_t N>size_t named(QJsonValue v,const std::array<const char*,N>&names){if(!v.isString())bad();auto i=std::find_if(names.begin(),names.end(),[&](auto n){return v.toString()==n;});if(i==names.end())bad();return size_t(i-names.begin());}\ntemplate<size_t N>size_t named2(QJsonValue v,const std::array<const char*,N>&a,const std::array<const char*,N>&b){if(!v.isString())bad();for(size_t i=0;i<N;++i){if(v.toString()==a[i]||v.toString()==b[i])return i;}bad();return 0;}`],
  [`static constexpr std::array<const char*,4> channels{"RGB","红色","绿色","蓝色"};`,
   `static constexpr std::array<const char*,7> channels{"RGB","红色","绿色","蓝色","Red","Green","Blue"};`],
  [`s.kind=named(o["kind"],kinds);`, `s.kind=named2(o["kind"],kinds,kindsEn);`],
  [`s.selectedRange=named(h["range"],ranges);`, `s.selectedRange=named2(h["range"],ranges,rangesEn);`],
  [`s.hsv[named(values[i],ranges)]=hsvChange(values[i+1]);`, `s.hsv[named2(values[i],ranges,rangesEn)]=hsvChange(values[i+1]);`],
  [`s.bands[named(bands[i],ranges)]=`, `s.bands[named2(bands[i],ranges,rangesEn)]=`],
  [`defaultAdjustmentJson(std::string_view kind){named(text(kind),kinds);`, `defaultAdjustmentJson(std::string_view kind){named2(text(kind),kinds,kindsEn);`],
]);

// ── Shapes.cpp: kindCode 英文写侧 + 双语读侧 ──
patch('src/editing/Shapes.cpp', [
  [`const char* kindName(ShapeKind kind){return kind==ShapeKind::Rectangle?"矩形":kind==ShapeKind::Ellipse?"椭圆":"直线";}`,
   `const char* kindName(ShapeKind kind){return kind==ShapeKind::Rectangle?"矩形":kind==ShapeKind::Ellipse?"椭圆":"直线";}\nconst char* kindCode(ShapeKind kind){return kind==ShapeKind::Rectangle?"Rectangle":kind==ShapeKind::Ellipse?"Ellipse":"Line";}`],
  [`(kind.toString()!="矩形"&&kind.toString()!="椭圆"&&kind.toString()!="直线")`,
   `(kind.toString()!="矩形"&&kind.toString()!="椭圆"&&kind.toString()!="直线"&&kind.toString()!="Rectangle"&&kind.toString()!="Ellipse"&&kind.toString()!="Line")`],
  [`ShapeStyle style{kind.toString()=="矩形"?ShapeKind::Rectangle:kind.toString()=="椭圆"?ShapeKind::Ellipse:ShapeKind::Line,`,
   `ShapeStyle style{(kind.toString()=="矩形"||kind.toString()=="Rectangle")?ShapeKind::Rectangle:(kind.toString()=="椭圆"||kind.toString()=="Ellipse")?ShapeKind::Ellipse:ShapeKind::Line,`],
  [`{{"kind",QLatin1String(kindName(style.kind))},`, `{{"kind",QLatin1String(kindCode(style.kind))},`],
]);

// ── SelectionGesture.cpp: 显示一致性 ──
patch('src/editing/SelectionGesture.cpp', [
  [`?"Lasso":`, `?"套索":`],
]);

// ── ProjectStore.cpp: 校验/读写双语,写侧英文 ──
patch('src/persistence/ProjectStore.cpp', [
  [`enumValue(o["kind"],{"Hue/Saturation","色阶","曲线","曝光","渐变映射","颗粒","反相","黑白","色彩平衡","高斯模糊","动感模糊","添加杂色"});`,
   `enumValue(o["kind"],{"Hue/Saturation","Levels","Curves","Exposure","Gradient Map","Grain","Invert","Black & White","Color Balance","Gaussian Blur","Motion Blur","Add Noise","色阶","曲线","曝光","渐变映射","颗粒","反相","黑白","色彩平衡","高斯模糊","动感模糊","添加杂色"});`],
  [`enumValue(o["kind"],{"矩形","椭圆","直线"});`, `enumValue(o["kind"],{"Rectangle","Ellipse","Line","矩形","椭圆","直线"});`],
  [`style=="常规"||style=="Normal"`, `style=="Regular"||style=="常规"||style=="Normal"`],
  [`if(kind=="高斯模糊"||kind=="动感模糊"||kind=="添加杂色")version=std::max(version,9);`,
   `if(kind=="Gaussian Blur"||kind=="Motion Blur"||kind=="Add Noise"||kind=="高斯模糊"||kind=="动感模糊"||kind=="添加杂色")version=std::max(version,9);`],
  [`if((kind=="高斯模糊"||kind=="动感模糊"||kind=="添加杂色")&&version<9)`,
   `if((kind=="Gaussian Blur"||kind=="Motion Blur"||kind=="Add Noise"||kind=="高斯模糊"||kind=="动感模糊"||kind=="添加杂色")&&version<9)`],
  [`if(name=="左")text.alignment=TextAlignment::Left;else if(name=="居中")text.alignment=TextAlignment::Center;else if(name=="右")text.alignment=TextAlignment::Right;`,
   `if(name=="Left"||name=="左")text.alignment=TextAlignment::Left;else if(name=="Center"||name=="居中")text.alignment=TextAlignment::Center;else if(name=="Right"||name=="右")text.alignment=TextAlignment::Right;`],
  [`t["alignment"]="居中";else if(l.text->alignment==TextAlignment::Right)t["alignment"]="右";`,
   `t["alignment"]="Center";else if(l.text->alignment==TextAlignment::Right)t["alignment"]="Right";`],
], true);
// validateShape 的两处 "直线" 判断(单独处理避免误替换)
patch('src/persistence/ProjectStore.cpp', [
  [`if(string(o["kind"])=="直线"){bounded(o["lineWidth"]`, `if(string(o["kind"])=="Line"||string(o["kind"])=="直线"){bounded(o["lineWidth"]`],
  [`if(string(o["kind"])=="直线")fail("线条形状无法携带描边")`, `if(string(o["kind"])=="Line"||string(o["kind"])=="直线")fail("线条形状无法携带描边")`],
]);

// ── AdjustmentDialog.cpp: kind 归一化(英文→中文) ──
let d = fs.readFileSync(file('src/ui/AdjustmentDialog.cpp'), 'utf8');
const anchor = d.lastIndexOf('#include ');
const lineEnd = d.indexOf('\n', anchor) + 1;
const helper = `\nstatic QString normalizeKind(const QString& k){struct MapEntry{const char* en;const char* zh;};static constexpr MapEntry map[]={{"Levels","色阶"},{"Curves","曲线"},{"Exposure","曝光"},{"Gradient Map","渐变映射"},{"Grain","颗粒"},{"Invert","反相"},{"Black & White","黑白"},{"Color Balance","色彩平衡"},{"Gaussian Blur","高斯模糊"},{"Motion Blur","动感模糊"},{"Add Noise","添加杂色"}};for(const auto& e:map)if(k==QLatin1String(e.en))return QLatin1String(e.zh);return k;}\n`;
d = d.slice(0, lineEnd) + helper + d.slice(lineEnd);
const before = d;
d = d.replace('kind=settings["kind"].toString();kind_', 'kind=normalizeKind(settings["kind"].toString());kind_');
if (d === before) console.log('MISS: AdjustmentDialog normalize');
else count++;
fs.writeFileSync(file('src/ui/AdjustmentDialog.cpp'), d, 'utf8');

console.log('补丁应用数:', count);
