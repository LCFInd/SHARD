#!/bin/sh
# FELUCCA + SHARD browser emulator: emu.c -> emu.wasm (clang, wasm32, freestanding)
set -e
cd "$(dirname "$0")"
R=../..
EXP=$(grep -o "EXPORT([a-z_]*)" emu.c | sed 's/EXPORT(\(.*\))/-Wl,--export=\1/' | tr '\n' ' ')
clang --target=wasm32 -O2 -nostdlib -ffreestanding -w -I$R/build/gen -I$R/firmware/hal -I$R/firmware/bsp/chip \
  -I$R/firmware/src -Wl,--no-entry -Wl,--export-memory -Wl,--allow-undefined -Wl,-z,stack-size=1048576 \
  $EXP "$@" -o emu.wasm emu.c
mkdir -p dist
cp page.html dist/index.html
cp core.js emu.wasm dist/
cat core.js worklet-tail.js > dist/emu-worklet.js
