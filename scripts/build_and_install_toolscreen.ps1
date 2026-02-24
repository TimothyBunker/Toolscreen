param(
    [string]$TargetJarPath = "C:\Users\Tim\Desktop\msr\MultiMC\instances\MCSRRanked-Windows-1.16.1-All\Toolscreen-GuckerOffficial-Edition-1.0.6.jar",
    [string]$Generator = "Visual Studio 17 2022",
    [string]$Architecture = "x64",
    [string]$BuildConfig = "Release"
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = (Resolve-Path (Join-Path $scriptDir "..")).Path
$buildDir = Join-Path $repoRoot "build"

Write-Host "[Toolscreen] Repo: $repoRoot"
Write-Host "[Toolscreen] Build dir: $buildDir"
Write-Host "[Toolscreen] Target jar: $TargetJarPath"

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

if (-not (Test-Path $TargetJarPath)) {
    throw "Target jar not found at '$TargetJarPath'."
}

$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$backupPath = "$TargetJarPath.bak_integratednbb_$timestamp"
Copy-Item -LiteralPath $TargetJarPath -Destination $backupPath -Force
Write-Host "[Toolscreen] Backup created: $backupPath"

$tempZip = Join-Path $env:TEMP ("toolscreen_patch_" + [System.Guid]::NewGuid().ToString("N") + ".zip")
Copy-Item -LiteralPath $TargetJarPath -Destination $tempZip -Force

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

try {
    Move-Item -Path $tempZip -Destination $TargetJarPath -Force
}
catch {
    Copy-Item -Path $tempZip -Destination $TargetJarPath -Force
    Remove-Item -Path $tempZip -Force -ErrorAction SilentlyContinue
}

Write-Host "[Toolscreen] Installed new dlls/Toolscreen.dll into:"
Write-Host "  $TargetJarPath"
Write-Host "[Toolscreen] Done."
