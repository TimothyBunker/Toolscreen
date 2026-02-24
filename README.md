# Toolscreen-GuckerOffficial-Edition

Custom Toolscreen fork for Minecraft 1.16.1 stronghold routing workflows.

## Download Here

- Release page: https://github.com/TimothyBunker/Toolscreen/releases
- Main artifact: `Toolscreen-GuckerOffficial-Edition-<version>.jar`

## Install (MultiMC/Prism Launcher)

1. Put the release jar in your instance folder (same level as `mmc-pack.json`).
2. Double-click the jar once to patch/update the instance copy.
3. Launch Minecraft from that instance.

## Core Hotkeys

- `H`: show/hide stronghold HUD
- `Shift+H`: lock/unlock target
- `Ctrl+Shift+H`: reset target/throws state
- `Num8`: angle + step
- `Num2`: angle - step
- `Num4`: undo
- `Num6`: redo
- `Num5`: calc reset
- `Ctrl+Shift+M`: toggle all macros on/off

## Build (Windows x64)

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Build Variants

MCSR-safe build:

```powershell
cmake -S . -B build -DTOOLSCREEN_FORCE_MCSR_SAFE=ON
cmake --build build --config Release
```

Full-feature build:

```powershell
cmake -S . -B build -DTOOLSCREEN_FORCE_MCSR_SAFE=OFF
cmake --build build --config Release
```

## Helper Scripts

Build + install:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build_and_install_toolscreen.ps1
```

Package release jar/zip:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package_release.ps1
```

`package_release.ps1` auto-uses your `origin` GitHub repo for `Download Here` links in generated release notes.

## Attribution

- Toolscreen: https://github.com/jojoe77777/Toolscreen
- NinjaBrain Bot: https://github.com/Ninjabrain1/Ninjabrain-Bot
