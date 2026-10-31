<#
.SYNOPSIS
  Builds a reNut release: raises version.txt once, then builds Windows and
  Linux from that same version (the *-dist presets, RENUT_RELEASE).

.DESCRIPTION
  A release build names itself "reNut <version>" to the RexLiveless server
  (and renut@<version> to Sentry); ordinary builds stay "reNut dev". Raise the
  server's minimum for reNut to the new version in its admin console once the
  release is out, and older releases can no longer post scores or replays.

  Run from the repository root's shell that normally builds reNut (clang and
  Ninja on the PATH). Linux builds in WSL (Ubuntu-24.04).

.EXAMPLE
  .\tools\release.ps1                 # raises the last number: 0.1.0.1 -> 0.1.0.2 (0.1.0 -> 0.1.1)
  .\tools\release.ps1 -Bump minor     # 0.1.0.2 -> 0.2.0.0
  .\tools\release.ps1 -NoBump         # rebuild the current version
  .\tools\release.ps1 -SkipLinux      # Windows only
#>
param(
    # Which number to raise (the ones after it go to 0); by default the last:
    # build for major.minor.patch.build, patch for major.minor.patch.
    [ValidateSet("", "build", "patch", "minor", "major")] [string] $Bump = "",
    [switch] $NoBump,
    [switch] $SkipLinux,
    # Uploads the debug files to Sentry after building (needs $env:SENTRY_AUTH_TOKEN).
    [switch] $UploadSymbols
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$versionFile = Join-Path $root "version.txt"

$current = (Get-Content $versionFile -TotalCount 1).Trim()
if ($current -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
    throw "version.txt should hold major.minor.patch or major.minor.patch.build, not '$current'"
}
[int[]] $parts = $current.Split('.') | ForEach-Object { [int]$_ }

if ($NoBump) {
    $version = $current
} else {
    $index = @{ major = 0; minor = 1; patch = 2; build = 3 }
    $at = if ($Bump) { $index[$Bump] } else { $parts.Count - 1 }
    if ($at -ge $parts.Count) { throw "version.txt '$current' has no $Bump number" }
    $parts[$at]++
    for ($i = $at + 1; $i -lt $parts.Count; $i++) { $parts[$i] = 0 }
    $version = $parts -join '.'
    Set-Content -Path $versionFile -Value $version -Encoding ascii
}
Write-Host "reNut $version" -ForegroundColor Cyan

function Invoke-Step([string] $what, [scriptblock] $step) {
    Write-Host "== $what" -ForegroundColor Cyan
    & $step
    if ($LASTEXITCODE -ne 0) { throw "$what failed (exit $LASTEXITCODE); version.txt is now $version" }
}

Push-Location $root
try {
    Invoke-Step "Windows: configure" { cmake --preset win-amd64-dist }
    Invoke-Step "Windows: build" { cmake --build --preset win-amd64-dist }
    if ($UploadSymbols) {
        if (-not $env:SENTRY_AUTH_TOKEN) { throw "-UploadSymbols needs `$env:SENTRY_AUTH_TOKEN set" }
        Invoke-Step "Windows: upload symbols" { cmake --build --preset win-amd64-dist --target renut_upload_symbols }
    }

    if (-not $SkipLinux) {
        $rexglue = "/mnt/g/github repos/rexglue-sdk/out/install/linux-amd64/lib/cmake/rexglue"
        $configure = "cd /mnt/f/renut2 && cmake --preset linux-amd64-dist -DCMAKE_C_COMPILER=/usr/bin/clang-20 " +
                     "-DCMAKE_CXX_COMPILER=/usr/bin/clang++-20 '-Drexglue_DIR=$rexglue'"
        Invoke-Step "Linux: configure" { wsl -d Ubuntu-24.04 -u root -- bash -c $configure }
        Invoke-Step "Linux: build" { wsl -d Ubuntu-24.04 -u root -- cmake --build /mnt/f/renut2/out/build/linux-amd64-dist }
        if ($UploadSymbols) {
            # From Windows, with the sentry-cli the Windows build found: WSL has no
            # sentry-cli of its own, and sentry-cli uploads Linux files from any OS.
            # ORG and PROJECT are sentrywork_attach's in CMakeLists.txt.
            $cache = Get-Content (Join-Path $root "out\build\win-amd64-dist\CMakeCache.txt")
            $cli = ($cache | Select-String '^SENTRYWORK_SENTRY_CLI:FILEPATH=(.+)$').Matches[0].Groups[1].Value
            $files = @(
                (Join-Path $root "out\build\linux-amd64-dist\renut"),
                "G:\github repos\rexglue-sdk\out\install\linux-amd64\lib\librexruntime.so"
            ) -join '|'
            Invoke-Step "Linux: upload symbols" {
                cmake "-DSENTRY_CLI=$cli" -DORG=madlad-inc -DPROJECT=renut "-DFILES=$files" `
                      -P (Join-Path $root "thirdparty\sentrywork\cmake\upload_symbols.cmake")
            }
        }
    }
} finally {
    Pop-Location
}

Write-Host "Built reNut ${version}: out\build\win-amd64-dist" -ForegroundColor Green
if (-not $SkipLinux) { Write-Host "                 and out/build/linux-amd64-dist" -ForegroundColor Green }
Write-Host "Once it's out, set reNut's minimum version to $version in the server's admin console." -ForegroundColor Yellow
