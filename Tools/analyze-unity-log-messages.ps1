param(
    [Parameter(Mandatory = $true)][string]$Path,
    [int]$Top = 25
)

$fs = [System.IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
$sr = New-Object System.IO.StreamReader($fs)
$stats = New-Object 'System.Collections.Generic.Dictionary[string,int]'
$n = 0
while ($null -ne ($line = $sr.ReadLine())) {
    $n++
    if ($line.Length -eq 0) { continue }
    if ($line.StartsWith('UnityEngine.')) { continue }
    if ($line.StartsWith('UnityEditor.')) { continue }
    if ($line.StartsWith('(Filename:')) { continue }
    if ($line.StartsWith('  ')) { continue }
    $k = $line
    if ($k.Length -gt 120) { $k = $k.Substring(0, 120) }
    $v = 0
    if ($stats.TryGetValue($k, [ref]$v)) { $stats[$k] = $v + 1 } else { $stats[$k] = 1 }
}
$sr.Close()
$fs.Close()

"FILE : $Path"
"LINES: $n"
""
"UNIQUE MESSAGE LINES (non-stack):"
foreach ($kv in $stats.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top) {
    "  {0,10}  {1}" -f $kv.Value, $kv.Key
}
