// 依赖预下载：绕过本机 schannel 故障，用 Node(OpenSSL) 下载并校验 SHA-256
import https from 'node:https';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const root = 'G:/开源PS';
const jobs = [
  { url: 'https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_683/qt6_683/qt.qt6.683.win64_msvc2022_64/6.8.3-0-202503201308qtbase-Windows-Windows_11_23H2-MSVC2022-Windows-Windows_11_23H2-X86_64.7z',
    file: 'dependencies/downloads/qtbase.7z',
    sha256: '41688269FAC0565DB956C66D9EECAE777D16197E0C02CD81B88640C1F5D73D3F' },
  { url: 'https://github.com/AcademySoftwareFoundation/openexr/archive/6661cb40c6fa8d332f142e660e25ac96675c15bd.tar.gz',
    file: 'dependencies/downloads/hdr/openexr.tar.gz',
    sha256: '6438c34123ab378f9b1aa98909ed5e65f4898fac7689ea736800ddb641b7cdfa' },
  { url: 'https://github.com/AcademySoftwareFoundation/Imath/archive/c0396a055a01bc537d32f435aee11a9b7ed6f0b5.tar.gz',
    file: 'dependencies/downloads/hdr/imath.tar.gz',
    sha256: 'adb799567cd7b6a505deedb8f8f653b07c12ed2c38c6bdadded4da0b32cc2abf' },
  { url: 'https://github.com/ebiggers/libdeflate/archive/495fee110ebb48a5eb63b75fd67e42b2955871e2.tar.gz',
    file: 'dependencies/downloads/hdr/libdeflate.tar.gz',
    sha256: '4182481e6b1a1ebe00ae076d4eddb6978d9153749a6f4f3c2914770b5ffc8956' },
  { url: 'https://github.com/microsoft/onnxruntime/releases/download/v1.30.0/onnxruntime-win-x64-1.30.0.zip',
    file: 'dependencies/imaging/onnxruntime-win-x64-1.30.0.zip',
    sha256: 'c6ba983baf5681af108599675d2a89c2d145512d02de28aed0bff177cd0ba949' },
  { url: 'https://raw.githubusercontent.com/saucecontrol/Compact-ICC-Profiles/bdd84663061bc4ae95ca70decff54f581e27f702/profiles/AdobeCompat-v2.icc',
    file: 'evidence/imaging/AdobeCompat-v2.icc',
    sha256: '60fb2adecacf82132db0b1c09b303316f3bbd9e2823e7ba096d01627d12d57c9' },
];

function download(url, dest, redirects = 0) {
  return new Promise((resolve, reject) => {
    if (redirects > 10) return reject(new Error('too many redirects: ' + url));
    const req = https.get(url, { headers: { 'User-Agent': 'Mozilla/5.0 (Windows NT 10.0; Win64; x64)' } }, res => {
      const status = res.statusCode || 0;
      if ([301, 302, 303, 307, 308].includes(status)) {
        res.resume();
        const loc = new URL(res.headers.location, url).toString();
        return resolve(download(loc, dest, redirects + 1));
      }
      if (status !== 200) { res.resume(); return reject(new Error('HTTP ' + status + ' for ' + url)); }
      const total = parseInt(res.headers['content-length'] || '0', 10);
      fs.mkdirSync(path.dirname(dest), { recursive: true });
      const tmp = dest + '.part';
      const out = fs.createWriteStream(tmp);
      let seen = 0, lastLog = 0;
      const t0 = Date.now();
      res.on('data', chunk => {
        seen += chunk.length;
        const now = Date.now();
        if (now - lastLog > 3000) {
          lastLog = now;
          const mb = (seen / 1048576).toFixed(1);
          const pct = total ? `(${(seen / total * 100).toFixed(0)}%)` : '';
          const speed = ((seen / 1048576) / ((now - t0) / 1000)).toFixed(2);
          console.log(`  ${path.basename(dest)}: ${mb} MB ${pct} @ ${speed} MB/s`);
        }
      });
      res.pipe(out);
      out.on('finish', () => out.close(() => fs.renameSync(tmp, dest) & resolve(seen)));
      out.on('error', reject);
      res.on('error', reject);
    });
    req.on('error', reject);
    req.setTimeout(60000, () => { req.destroy(new Error('timeout')); });
  });
}

for (const job of jobs) {
  const dest = path.join(root, job.file);
  if (fs.existsSync(dest)) {
    const h = crypto.createHash('sha256').update(fs.readFileSync(dest)).digest('hex');
    if (h.toLowerCase() === job.sha256.toLowerCase()) { console.log('OK(已存在) ' + job.file); continue; }
    fs.rmSync(dest, { force: true });
  }
  console.log('下载 ' + job.url);
  const retries = 4;
  for (let i = 1; i <= retries; i++) {
    try { const n = await download(job.url, dest); console.log(`OK ${job.file} ${(n / 1048576).toFixed(1)} MB`); break; }
    catch (e) {
      console.error(`  失败(${i}/${retries}): ${e.message}`);
      if (i === retries) { console.error('GIVEUP ' + job.file); process.exitCode = 1; }
      else await new Promise(r => setTimeout(r, 3000 * i));
    }
  }
  if (fs.existsSync(dest)) {
    const h = crypto.createHash('sha256').update(fs.readFileSync(dest)).digest('hex');
    if (h.toLowerCase() !== job.sha256.toLowerCase()) { console.error('SHA256不匹配 ' + job.file + ' 得到 ' + h); process.exitCode = 1; }
    else console.log('SHA256 OK ' + job.file);
  }
}
console.log('全部完成 exit=' + process.exitCode);
