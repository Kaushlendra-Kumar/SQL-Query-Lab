# One-command startup on Windows (PowerShell). Requires CMake, a C++20
# compiler (Visual Studio 2022 Build Tools) and Node.js 18+.
#   .\scripts\start.ps1
#   .\scripts\start.ps1 --dataset ecommerce --scale 10000
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root
$Server = Join-Path $Root 'build\server\Release\querylab_server.exe'
if (-not (Test-Path $Server)) {
  cmake -S . -B build
  cmake --build build --config Release
}
if (-not (Test-Path 'frontend\dist\index.html')) {
  Push-Location frontend
  if (-not (Test-Path node_modules)) { npm ci --no-audit --no-fund }
  npm run build
  Pop-Location
}
Write-Host 'Open http://127.0.0.1:8080 in your browser (Ctrl+C to stop).'
& $Server --static frontend\dist @args
