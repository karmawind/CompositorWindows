# Build dict-09.json (ASCII-only script; Chinese values come from translations-09.json)
$ErrorActionPreference = 'Stop'
$chunkPath = 'G:\CompositorZH\l10n-tools\out\chunk-09.json'
$transPath = 'G:\CompositorZH\l10n-tools\out\translations-09.json'
$outPath   = 'G:\CompositorZH\l10n-tools\out\dict-09.json'

$arr = (Get-Content -Raw -Encoding UTF8 $chunkPath) | ConvertFrom-Json
$t   = (Get-Content -Raw -Encoding UTF8 $transPath) | ConvertFrom-Json
if ($arr.Count -ne 187) { throw "Expected 187 chunk entries, got $($arr.Count)" }
if ($t.Count  -ne 187) { throw "Expected 187 translations, got $($t.Count)" }

$dict = [ordered]@{}
for ($i = 0; $i -lt 187; $i++) {
    $key = [string]$arr[$i].s
    if ($dict.Contains($key)) { throw "Duplicate key at index ${i}: $key" }
    if ($null -eq $t[$i]) { $dict[$key] = $null }
    else { $dict[$key] = [string]$t[$i] }
}

$json = $dict | ConvertTo-Json
[System.IO.File]::WriteAllText($outPath, $json, [System.Text.UTF8Encoding]::new($false))
"WROTE $outPath"
