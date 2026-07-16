#!/bin/sh
# build.sh - assembles and links denasm
set -e

cd "$(dirname "$0")"

nasm -f elf64 den.asm -o den.o
ld den.o -o denasm
strip denasm
rm -f den.o

chmod +x denasm

echo "built ./denasm ($(du -h denasm | cut -f1))"
