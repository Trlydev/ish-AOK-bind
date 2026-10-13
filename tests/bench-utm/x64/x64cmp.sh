#!/bin/bash
# x64cmp.sh RUN REF: compare two x64run.sh transcripts (logs/x64/RUN.log against logs/x64/REF.log)
W=$HOME/aok-vm-bench/logs/x64
ext() { tr -d "\r" < "$1" | sed -n "/^BENCH-START/,/^BENCH-END/p" | grep -av "^RESULT\|^aok:\|^BENCH-START"; }
ext "$W/$1.log" > /tmp/x64cmp.a; ext "$W/$2.log" > /tmp/x64cmp.b
n=$(wc -l < /tmp/x64cmp.a | tr -d " ")
if [ "$n" -gt 0 ] && cmp -s /tmp/x64cmp.a /tmp/x64cmp.b; then v=IDENTICAL; else v="DIFFERENT ($(wc -l < /tmp/x64cmp.b | tr -d " ") in ref)"; fi
echo "$1 vs $2: $n lines $v; $(grep -a "^RESULT" "$W/$1.log" | cut -d" " -f2,3 | tr "\n" ";") ref $(grep -a "^RESULT" "$W/$2.log" | cut -d" " -f2,3 | tr "\n" ";")"
