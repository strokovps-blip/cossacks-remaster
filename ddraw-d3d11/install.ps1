param([switch]$Remove)
$g = "Z:\Cossacks\Cossacks - The Art Of War"
if ($Remove) { Remove-Item "$g\ddraw.dll" -ErrorAction SilentlyContinue; "removed" }
else { Copy-Item "$PSScriptRoot\ddraw.dll" "$g\ddraw.dll" -Force; "installed" }
