#!/usr/bin/env bash
# Low-RAM-friendly compile for ChromeOS / small VMs.
# Uses a single job (-j 1) so the ESP32 toolchain does not OOM.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "arduino-cli not found on PATH" >&2
  exit 1
fi

echo "Available RAM before compile:"
command -v free >/dev/null && free -h || true

# FQBN uses PartitionScheme=no_ota (firmware ~1.47MB needs a 2MB app partition).
# -j 1: never parallelize on this host (IDE wrappers also flock the compiler).
FQBN='esp32:esp32:esp32:UploadSpeed=921600,CPUFreq=240,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=no_ota,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none,JTAGAdapter=default,ZigbeeMode=default'
# Same build dir as flash.ps1 -SkipCompile.
exec arduino-cli compile -j 1 --fqbn "$FQBN" --warnings none --build-path "$ROOT/build" "$ROOT" "$@"
