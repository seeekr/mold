#!/bin/bash
set -e
echo -n "Testing $(basename -s .sh $0) ..."
t=$(pwd)/tmp/$(basename -s .sh $0)
mkdir -p $t

cat <<EOF | cc -o $t/a.o -c -x assembler -
  .text
  .globl _start, foo
_start:
  nop
EOF

../mold -o $t/exe $t/a.o

echo ' OK'
