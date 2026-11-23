#!/usr/bin/env sh
# Builds demo/site/itch.js + itch.wasm from the feed handler with Emscripten.
#   docker run --rm -v "$PWD":/src -w /src emscripten/emsdk:3.1.74 sh demo/build.sh
set -e
em++ -std=c++20 -O3 -flto -fno-exceptions -Wall -Wextra \
  -Iinclude demo/wasm/itch_wasm.cpp -o demo/site/itch.js \
  -sMODULARIZE=1 -sEXPORT_NAME=createItch -sENVIRONMENT=web \
  -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=1073741824 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32,HEAPF64,UTF8ToString \
  -sFILESYSTEM=0 -sASSERTIONS=0
ls -l demo/site/itch.js demo/site/itch.wasm
