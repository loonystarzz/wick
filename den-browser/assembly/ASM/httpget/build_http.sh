#!/bin/sh
# build.sh - assembles and links httpget
set -e

cd "$(dirname "$0")"

nasm -f elf64 httpget2.asm -o httpget2.o
ld httpget2.o -o httpget2
strip httpget2
rm -f httpget2.o

chmod +x httpget2

echo "built ./httpget2 ($(du -h httpget2 | cut -f1))"
