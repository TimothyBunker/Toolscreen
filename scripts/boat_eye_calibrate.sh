#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PS_SCRIPT="$ROOT_DIR/scripts/boat_eye_calibrate.ps1"

if ! command -v powershell.exe >/dev/null 2>&1; then
  echo "powershell.exe not found. Run this from WSL on Windows."
  exit 1
fi

if ! command -v wslpath >/dev/null 2>&1; then
  echo "wslpath not found; cannot convert script path for powershell.exe."
  exit 1
fi

PS_SCRIPT_WIN="$(wslpath -w "$PS_SCRIPT")"

# Forward all args directly to the PowerShell script.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PS_SCRIPT_WIN" "$@"
