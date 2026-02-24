param(
    [string]$BaseJarPath = "C:\Users\Tim\Desktop\msr\MultiMC\instances\MCSRRanked-Windows-1.16.1-All\Toolscreen-GuckerOffficial-Edition-1.0.6.jar",
    [string]$ReleaseName = "Toolscreen-GuckerOffficial-Edition",
    [string]$ReleaseVersion = "1.0.6",
    [string]$ReleasePageUrl = "",
    [string]$OutputDir = "",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Architecture = "x64",
    [string]$BuildConfig = "Release"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
$buildDir = Join-Path $repoRoot "build"
$defaultReleasePageUrl = "https://github.com/TimothyBunker/Toolscreen/releases"

function Get-ReleasePageUrlFromOrigin {
    param(
        [string]$RepoRoot
    )

    try {
        $originUrl = (git -C $RepoRoot remote get-url origin 2>$null)
        if ([string]::IsNullOrWhiteSpace($originUrl)) {
            return $null
        }

        $originUrl = $originUrl.Trim()
        $owner = $null
        $repo = $null

        if ($originUrl -match "^git@github\.com:(?<owner>[^/]+)/(?<repo>[^/.]+)(?:\.git)?$") {
            $owner = $Matches["owner"]
            $repo = $Matches["repo"]
        }
        elseif ($originUrl -match "^https://github\.com/(?<owner>[^/]+)/(?<repo>[^/.]+)(?:\.git)?/?$") {
            $owner = $Matches["owner"]
            $repo = $Matches["repo"]
        }

        if (-not [string]::IsNullOrWhiteSpace($owner) -and -not [string]::IsNullOrWhiteSpace($repo)) {
            return "https://github.com/$owner/$repo/releases"
        }
    }
    catch {
        return $null
    }

    return $null
}

if ([string]::IsNullOrWhiteSpace($ReleasePageUrl)) {
    $autoReleasePage = Get-ReleasePageUrlFromOrigin -RepoRoot $repoRoot
    if ([string]::IsNullOrWhiteSpace($autoReleasePage)) {
        $ReleasePageUrl = $defaultReleasePageUrl
    }
    else {
        $ReleasePageUrl = $autoReleasePage
    }
}

$safeName = ($ReleaseName -replace "[^a-zA-Z0-9._-]", "-")
$safeVersion = ($ReleaseVersion -replace "[^a-zA-Z0-9._-]", "-")

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutputDir = Join-Path $repoRoot ("release\" + $safeName + "-" + $safeVersion + "-" + $timestamp)
}

Write-Host "[Package] Repo: $repoRoot"
Write-Host "[Package] Build dir: $buildDir"
Write-Host "[Package] Base jar: $BaseJarPath"
Write-Host "[Package] Output dir: $OutputDir"
Write-Host "[Package] Release page: $ReleasePageUrl"

if (-not (Test-Path (Join-Path $buildDir "CMakeCache.txt"))) {
    cmake -S $repoRoot -B $buildDir -G $Generator -A $Architecture
} else {
    cmake -S $repoRoot -B $buildDir
}
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}

cmake --build $buildDir --config $BuildConfig
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE."
}

$dllPath = Join-Path $buildDir "$BuildConfig\Toolscreen.dll"
if (-not (Test-Path $dllPath)) {
    throw "Built DLL not found at '$dllPath'."
}

if (-not (Test-Path $BaseJarPath)) {
    throw "Base jar not found at '$BaseJarPath'."
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$releaseJarName = "$safeName-$safeVersion.jar"
$releaseJarPath = Join-Path $OutputDir $releaseJarName

$tempZip = Join-Path $env:TEMP ("toolscreen_release_" + [System.Guid]::NewGuid().ToString("N") + ".zip")
Copy-Item -LiteralPath $BaseJarPath -Destination $tempZip -Force

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$zip = [System.IO.Compression.ZipFile]::Open($tempZip, [System.IO.Compression.ZipArchiveMode]::Update)
try {
    $entryPath = "dlls/Toolscreen.dll"
    $existing = $zip.GetEntry($entryPath)
    if ($null -ne $existing) {
        $existing.Delete()
    }

    [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
        $zip,
        $dllPath,
        $entryPath,
        [System.IO.Compression.CompressionLevel]::Optimal
    ) | Out-Null
}
finally {
    $zip.Dispose()
}

Copy-Item -Path $tempZip -Destination $releaseJarPath -Force
Remove-Item -Path $tempZip -Force -ErrorAction SilentlyContinue

$credits = @"
# Credits

This release is built on top of:

- Toolscreen by jojoe77777:
  - https://github.com/jojoe77777/Toolscreen
- NinjaBrain Bot by Ninjabrain1:
  - https://github.com/Ninjabrain1/Ninjabrain-Bot

Fork/release packaging and integration work:
- Stronghold overlay and MCSR workflow customizations in this branch.
"@

$notes = @"
# $ReleaseName $ReleaseVersion

## Summary

Custom Toolscreen build focused on MCSR stronghold workflow, including native overlay, compact HUD modes, and multi-monitor companion view.
This release is standalone-only for stronghold tracking (local F3+C clipboard pipeline, no backend process required).

## Attribution

Please keep upstream attribution when sharing this build:
- Toolscreen: https://github.com/jojoe77777/Toolscreen
- NinjaBrain Bot: https://github.com/Ninjabrain1/Ninjabrain-Bot

## Download Here

- $ReleasePageUrl

## Included Artifacts

- $releaseJarName
- CREDITS.md
- RELEASE_NOTES.md
- SHA256SUMS.txt
"@

$creditsPath = Join-Path $OutputDir "CREDITS.md"
$notesPath = Join-Path $OutputDir "RELEASE_NOTES.md"

Set-Content -LiteralPath $creditsPath -Value $credits -Encoding UTF8
Set-Content -LiteralPath $notesPath -Value $notes -Encoding UTF8

$hash = (Get-FileHash -LiteralPath $releaseJarPath -Algorithm SHA256).Hash.ToLowerInvariant()
$hashLine = "$hash  $releaseJarName"
$hashPath = Join-Path $OutputDir "SHA256SUMS.txt"
Set-Content -LiteralPath $hashPath -Value $hashLine -Encoding ASCII

$zipName = "$safeName-$safeVersion.zip"
$zipPath = Join-Path $OutputDir $zipName
Compress-Archive -Path $releaseJarPath, $creditsPath, $notesPath, $hashPath -DestinationPath $zipPath -Force

Write-Host "[Package] Release jar:"
Write-Host "  $releaseJarPath"
Write-Host "[Package] Release zip:"
Write-Host "  $zipPath"
Write-Host "[Package] SHA256:"
Write-Host "  $hashLine"
Write-Host "[Package] Done."
