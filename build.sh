#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"

rm -rf "$ROOT/dist"

make -C "$ROOT" -f Makefile.mingw clean
make -C "$ROOT" -f Makefile.mingw

mkdir -p "$ROOT/dist"
cd "$ROOT/Release" && zip -r "$ROOT/dist/cs16-masterservers-pre-anniversary.zip" . -x '*.bak'
cd "$ROOT/Release-anniversary" && zip -r "$ROOT/dist/cs16-masterservers-anniversary.zip" . -x '*.bak'

echo ""
echo "Build complete. Output in dist/"
ls -lh "$ROOT/dist/"
