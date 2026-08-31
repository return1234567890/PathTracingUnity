param(
    [Parameter(Mandatory=$true)][string]$spvPath,
    [Parameter(Mandatory=$true)][string]$headerPath,
    [string]$varName = "g_DebugDepthSpv"
)
$bytes = [System.IO.File]::ReadAllBytes($spvPath)
$sb = [System.Text.StringBuilder]::new()
$sb.AppendLine("static const uint32_t ${varName}[] = {") | Out-Null
$uintCount = $bytes.Length / 4
for ($i = 0; $i -lt $uintCount; $i++) {
    $val = [BitConverter]::ToUInt32($bytes, $i * 4)
    $sb.Append(('    0x{0:x8}' -f $val)) | Out-Null
    if ($i + 1 -lt $uintCount) { $sb.Append(',') | Out-Null }
    if (($i + 1) % 8 -eq 0 -or $i + 1 -eq $uintCount) {
        $sb.AppendLine() | Out-Null
    }
}
$sb.AppendLine('};') | Out-Null
$sb.Append("static const uint32_t ${varName}_size = $($bytes.Length);") | Out-Null
[System.IO.File]::WriteAllText($headerPath, $sb.ToString())
Write-Output "Header updated: $($bytes.Length) bytes, $($uintCount) uint32s, var=${varName}"
