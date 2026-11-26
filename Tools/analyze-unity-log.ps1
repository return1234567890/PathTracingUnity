param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$Top = 20,
    [int]$MaxMB = 0
)

$fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
$sr = New-Object System.IO.StreamReader($fs)
$stats = New-Object 'System.Collections.Generic.Dictionary[string,int]'
$n = 0
$firstTs = $null
$lastTs = $null
$firstLines = New-Object 'System.Collections.Generic.List[string]'
$ring = New-Object 'System.Collections.Generic.Queue[string]'
$bytes = 0
$markers = New-Object 'System.Collections.Generic.Dictionary[string,int]'

while ($null -ne ($line = $sr.ReadLine())) {
    $n++
    $bytes += $line.Length + 2
    if ($firstLines.Count -lt 6) { $firstLines.Add($line) }
    if ($ring.Count -ge 8) { [void]$ring.Dequeue() }
    $ring.Enqueue($line)

    if ($line.Length -ge 14 -and $line[0] -eq '[') {
        $head = $line.Substring(0, 12)
        if ($head -match '^\[2026-\d\d-\d\d') {
            $ts = $line.Substring(1, 19)
            if ($null -eq $firstTs) { $firstTs = $ts }
            $lastTs = $ts
        }
    }

    $k = $line
    if ($k.Length -gt 44) { $k = $k.Substring(0, 44) }
    $v = 0
    if ($stats.TryGetValue($k, [ref]$v)) { $stats[$k] = $v + 1 } else { $stats[$k] = 1 }

    foreach ($m in @('BuildScene', 'PushToNative', 'EnterPlayMode', 'ExitingPlay', 'GameView', 'BLAS[', 'Unloading', 'NullReference', 'Exception')) {
        if ($line.Contains($m)) {
            $mv = 0
            if ($markers.TryGetValue($m, [ref]$mv)) { $markers[$m] = $mv + 1 } else { $markers[$m] = 1 }
        }
    }

    if ($MaxMB -gt 0 -and $bytes -ge ($MaxMB * 1MB)) { break }
}
$sr.Close()
$fs.Close()

"FILE      : $Path"
"BYTES     : $bytes"
"LINES     : $n"
"TS_FIRST  : $firstTs"
"TS_LAST   : $lastTs"
""
"MARKERS:"
foreach ($kv in $markers.GetEnumerator() | Sort-Object Value -Descending) { "  {0,10}  {1}" -f $kv.Value, $kv.Key }
""
"HEAD:"
foreach ($l in $firstLines) { "  " + $l }
""
"TAIL:"
foreach ($l in $ring) { "  " + $l }
""
"TOP PREFIXES:"
foreach ($kv in $stats.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top) {
    "  {0,10}  {1}" -f $kv.Value, $kv.Key
}
