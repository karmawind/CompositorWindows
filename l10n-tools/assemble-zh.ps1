# 组装中文版:复制官方便携运行时 → 替换为重编译的中文 exe → 校验
$ErrorActionPreference = 'Stop'
$src  = 'G:\开源PS\CompositorWindows-1.1.0\CompositorWindows'
$dst  = 'G:\开源PS\CompositorWindows-中文版\CompositorWindows'
$exe  = 'G:\CompositorZH\build\release\Release\Compositor.exe'

if (Test-Path 'G:\开源PS\CompositorWindows-中文版') { Remove-Item 'G:\开源PS\CompositorWindows-中文版' -Recurse -Force }
New-Item -ItemType Directory -Path (Split-Path $dst -Parent) | Out-Null
robocopy $src $dst /E /NFL /NDL /NJH /NP /XF Compositor.exe | Out-Null
if ($LASTEXITCODE -ge 8) { throw 'robocopy 失败' }
Copy-Item $exe "$dst\Compositor.exe" -Force

$exeInfo = Get-Item "$dst\Compositor.exe"
Write-Output ("exe = {0:N0} KB  {1}" -f ($exeInfo.Length/1KB), $exeInfo.LastWriteTime)
$model = "$dst\models\birefnet-lite.onnx"
$shader = "$dst\shaders\BrushCoverage.hlsl"
Write-Output ("model 存在: " + (Test-Path $model) + "  shader 存在: " + (Test-Path $shader))
Write-Output ("文件总数: " + (Get-ChildItem $dst -Recurse -File).Count)
Write-Output "组装完成: $dst"
