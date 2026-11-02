$ErrorActionPreference = 'Stop'
$chunkPath = 'G:\CompositorZH\l10n-tools\out\chunk-08.json'
$pairsPath = 'G:\CompositorZH\l10n-tools\out\dict-08-pairs.json'
$outPath   = 'G:\CompositorZH\l10n-tools\out\dict-08.json'

$items = ConvertFrom-Json ([System.IO.File]::ReadAllText($chunkPath))
$pairs = ConvertFrom-Json ([System.IO.File]::ReadAllText($pairsPath))
if ($items -isnot [System.Array]) { $items = @($items) }
if ($pairs -isnot [System.Array]) { $pairs = @($pairs) }
if ($items.Count -eq 1 -and $items[0] -is [System.Array]) { $items = @($items[0]) }
if ($pairs.Count -eq 1 -and $pairs[0] -is [System.Array]) { $pairs = @($pairs[0]) }
if ($items.Count -ne $pairs.Count) { throw "count mismatch: chunk=$($items.Count) pairs=$($pairs.Count)" }

$seen = @{}
for ($i = 0; $i -lt $items.Count; $i++) {
    $s = $items[$i].s
    if ($seen.ContainsKey($s)) { throw "duplicate chunk key at index $i" }
    $seen[$s] = $true
    $en = @($pairs[$i])[0]
    if (-not ($en -ceq $s)) { throw "PAIR MISMATCH at index $i :: chunk=[$s] pair=[$en]" }
}

function Esc([string]$v) {
    $sb = New-Object System.Text.StringBuilder
    foreach ($ch in $v.ToCharArray()) {
        $c = [int]$ch
        if ($ch -eq '"') { [void]$sb.Append('\"') }
        elseif ($ch -eq '\') { [void]$sb.Append('\\') }
        elseif ($c -eq 10) { [void]$sb.Append('\n') }
        elseif ($c -eq 13) { [void]$sb.Append('\r') }
        elseif ($c -eq 9)  { [void]$sb.Append('\t') }
        elseif ($c -lt 32) { [void]$sb.Append('\u' + $c.ToString('x4')) }
        else { [void]$sb.Append($ch) }
    }
    return $sb.ToString()
}

$lines = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $items.Count; $i++) {
    $zh = @($pairs[$i])[1]
    if ($null -eq $zh) { $val = 'null' } else { $val = '"' + (Esc([string]$zh)) + '"' }
    $lines.Add('"' + (Esc([string]$items[$i].s)) + '": ' + $val)
}
$json = "{`n" + ($lines -join ",`n") + "`n}"
[System.IO.File]::WriteAllText($outPath, $json, (New-Object System.Text.UTF8Encoding($false)))

# ---- validation: re-parse output and compare against chunk ----
$dict = ConvertFrom-Json ([System.IO.File]::ReadAllText($outPath))
$props = @($dict.PSObject.Properties)
if ($props.Count -ne $items.Count) { throw "dict count $($props.Count) != $($items.Count)" }
$translated = 0; $skipped = 0
for ($i = 0; $i -lt $items.Count; $i++) {
    if (-not ($props[$i].Name -ceq $items[$i].s)) { throw "KEY MISMATCH at index $i" }
    if ($null -eq $props[$i].Value) { $skipped++ } else { $translated++ }
}
Write-Output "VALIDATED total=$($items.Count) translated=$translated skipped=$skipped"
