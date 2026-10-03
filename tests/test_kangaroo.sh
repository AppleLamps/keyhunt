#!/bin/sh
# Bounded known key recovery tests for novel/kangaroo: every mode must recover
# the known key of puzzle 40 (range 2^39..2^40-1), plus the edge cases.
# Usage: tests/test_kangaroo.sh [path/to/kangaroo]
KG=${1:-./kangaroo}
case "$KG" in /*) ;; *) KG="$PWD/$KG" ;; esac
fail=0
check() {	# name expected_key args...
	name=$1; want=$2; shift 2
	out=$("$KG" "$@" -q 2>&1); st=$?
	got=$(echo "$out" | grep -o "privkey [0-9a-f]*" | awk '{print $2}' | sed 's/^0*//')
	if [ "$st" -eq 0 ] && [ "$got" = "$want" ]; then
		echo "[ok]   $name ($(echo "$out" | grep -o '[0-9.]*\*sqrt(W)' | head -1))"
	else
		echo "[FAIL] $name: exit $st, got '$got', want '$want'"; echo "$out" | tail -3; fail=1
	fi
}
P40=03a2efa402fd5268400c77c20e574ba86409ededee7c4020e4b9f0edbee53de0d4
R40=8000000000:ffffffffff
check "kangaroo"               e9ae4933d6 -p $P40 -r $R40 -t 2 -k 256 -s 1
check "kangaroo, negation on"  e9ae4933d6 -p $P40 -r $R40 -t 2 -k 256 -s 1 -e
check "gaudry-schost"          e9ae4933d6 -p $P40 -r $R40 -t 2 -k 256 -s 1 -g
check "gaudry-schost, neg off" e9ae4933d6 -p $P40 -r $R40 -t 2 -k 256 -s 1 -g -n
# key at the interval midpoint (Q - c*G would be the identity)
check "midpoint key 0x33 in 1:65"  33  -p 02463b3d9f662621fb1b4be8fbbe2520125a216cdfc9dae3debcba4850c690d45b -r 1:65 -g
# W = 64 is just above the direct check threshold: a 64 key search through the walks
check "key 0x34 in 1:65 (walks)"        34  -p 032b22efda32491a9e0294339ca3da761f7d36cfc8814c1b29ca731921025ff695 -r 1:65 -g
# small range through the walks (W = 4095), single thread, few kangaroos
check "small range kangaroo"       234 -p 03036213d1501ed4dc0c7bdcdc6a9f1d60c6cb558dafae1eace5ed2aba3b2eef5f -r 1:1000 -t 1 -k 8 -s 3
check "small range gaudry-schost"  234 -p 03036213d1501ed4dc0c7bdcdc6a9f1d60c6cb558dafae1eace5ed2aba3b2eef5f -r 1:1000 -t 1 -k 8 -s 3 -g
# the same seed must reproduce the same run
a=$("$KG" -p $P40 -r $R40 -t 1 -k 256 -s 5 -g -q | grep -o '^\[+\] [0-9]* ops' | head -1)
b=$("$KG" -p $P40 -r $R40 -t 1 -k 256 -s 5 -g -q | grep -o '^\[+\] [0-9]* ops' | head -1)
if [ -n "$a" ] && [ "$a" = "$b" ]; then echo "[ok]   seed reproducibility ($a)"; else echo "[FAIL] seed reproducibility: '$a' vs '$b'"; fail=1; fi

# Work file: a run stopped by -x resumes from its distinguished points, files of
# separate runs merge with cat, and a file of another search is refused
WD=$(mktemp -d); trap 'rm -rf "$WD"' EXIT
"$KG" -p $P40 -r $R40 -t 2 -k 256 -s 1 -g -w "$WD/a.dp" -x 200000 -q > /dev/null; st=$?
n1=$(stat -c %s "$WD/a.dp")
if [ "$st" -eq 2 ] && [ "$n1" -gt 128 ]; then echo "[ok]   -x stop, work file written ($n1 bytes)"; else echo "[FAIL] -x stop: exit $st, $n1 bytes"; fail=1; fi
out=$("$KG" -p $P40 -r $R40 -t 2 -k 256 -s 1 -g -w "$WD/a.dp" -x 200000 2>&1)
if echo "$out" | grep -q "$(( (n1 - 128) / 64 )) distinguished points loaded"; then echo "[ok]   resume loads every stored point"; else echo "[FAIL] resume"; echo "$out" | head -5; fail=1; fi
got=""
for i in 1 2 3 4 5 6 7 8; do
	got=$("$KG" -p $P40 -r $R40 -t 2 -k 256 -s 1 -g -w "$WD/a.dp" -x 200000 -q | grep -o "privkey [0-9a-f]*" | awk '{print $2}' | sed 's/^0*//')
	[ -n "$got" ] && break
done
if [ "$got" = e9ae4933d6 ]; then echo "[ok]   resumed runs find the key (run $i)"; else echo "[FAIL] resumed runs: '$got'"; fail=1; fi
"$KG" -p $P40 -r $R40 -t 2 -k 256 -s 7 -g -w "$WD/b.dp" -x 400000 -q > /dev/null
"$KG" -p $P40 -r $R40 -t 2 -k 256 -s 9 -g -w "$WD/c.dp" -x 400000 -q > /dev/null
cat "$WD/b.dp" "$WD/c.dp" > "$WD/bc.dp"
check "merged work files"      e9ae4933d6 -p $P40 -r $R40 -t 2 -k 256 -s 11 -g -w "$WD/bc.dp"
if "$KG" -p $P40 -r 8000000000:fffffffffe -g -w "$WD/a.dp" -q > /dev/null 2>&1; then echo "[FAIL] work file of another range accepted"; fail=1; else echo "[ok]   work file of another range refused"; fi
if "$KG" -p $P40 -r $R40 -w "$WD/a.dp" -q > /dev/null 2>&1; then echo "[FAIL] work file of another mode accepted"; fail=1; else echo "[ok]   work file of another mode refused"; fi
exit $fail
