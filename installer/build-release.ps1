# Build every component and package the NSIS installer.
#   powershell -ExecutionPolicy Bypass -File installer\build-release.ps1 [-Version 0.1.1]
param([string]$Version = '0.1.1')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Invoke-Build([string]$cmd) {
    $out = cmd /c "`"$cmd`"" 2>&1
    if ($LASTEXITCODE -ne 0 -or ($out | Select-String -Pattern ' error ' -SimpleMatch)) {
        $out | Select-String 'error' | ForEach-Object { Write-Host $_ }
        throw "build failed: $cmd"
    }
}

Write-Host '== host (x86)';  Invoke-Build "$root\host\build.cmd"
Write-Host '== tools (x86)'; Invoke-Build "$root\tools\build.cmd"
Write-Host '== tip (x64+x86)'; Invoke-Build "$root\tip\build.cmd"

$bin = Join-Path $PSScriptRoot 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
Copy-Item "$root\host\hanin_host.exe" $bin -Force
Copy-Item "$root\tools\hanin_tools.exe" $bin -Force
Copy-Item "$root\tip\HaninTip64.dll" $bin -Force
Copy-Item "$root\tip\HaninTip32.dll" $bin -Force

New-Item -ItemType Directory -Force (Join-Path $root 'release') | Out-Null
$makensis = @("${env:ProgramFiles(x86)}\NSIS\makensis.exe", "$env:ProgramFiles\NSIS\makensis.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $makensis) { throw 'makensis.exe (NSIS 3) not found' }
Write-Host "== NSIS ($makensis)"
& $makensis /V2 "/DVERSION=$Version" (Join-Path $PSScriptRoot 'HaninRevival.nsi')
if ($LASTEXITCODE -ne 0) { throw 'makensis failed' }
$setup = Join-Path $root "release\HaninRevival-Setup-$Version.exe"
Get-Item $setup | Select-Object FullName, Length
(Get-FileHash $setup -Algorithm SHA256).Hash
