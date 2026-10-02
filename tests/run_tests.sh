#!/bin/sh
# End to end regression tests: each run scans a tiny, fully known range of
# puzzle keys and checks that every expected key is reported.
# Usage: tests/run_tests.sh [path/to/keyhunt]
KH=${1:-./keyhunt}
case "$KH" in /*) ;; *) KH="$PWD/$KH" ;; esac
TESTS=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1

fail=0
check() {	# name expected_count actual_count
	if [ "$2" -eq "$3" ]; then
		echo "[ok]   $1 ($3)"
	else
		echo "[FAIL] $1: expected $2, got $3"
		fail=1
	fi
}
count() { grep -c "$1"; }

# Keys 1 to 0xFFFFF: 20 of the solved puzzles fall in this range
for mode in compress both; do
	n=$("$KH" -m address -f "$TESTS/1to32.txt" -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l $mode 2>&1 | count "Private Key")
	check "address -l $mode" 20 "$n"
	n=$("$KH" -m rmd160 -f "$TESTS/1to32.rmd" -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l $mode 2>&1 | count "Private Key")
	check "rmd160 -l $mode" 20 "$n"
done

# RIPEMD160 of the uncompressed public key of private key 1
echo 91b24bf9f5288532960ac687abb035127b1d28a5 > unc.rmd
n=$("$KH" -m rmd160 -f unc.rmd -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l uncompress 2>&1 | count "Private Key")
check "rmd160 -l uncompress" 1 "$n"

# Every lane of the 4 and 8 way hashers must be exercised: single threaded run, stride 1
n=$("$KH" -m address -f "$TESTS/1to32.txt" -r 1:FFFFF -n 0x100000 -t 1 -q -s 0 -l compress 2>&1 | count "Private Key")
check "address -t 1" 20 "$n"

# Endomorphism uses the 4 way SSE path
n=$("$KH" -m address -f "$TESTS/1to32.txt" -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l compress -e 2>&1 | count "Private Key")
[ "$n" -ge 20 ] && echo "[ok]   address -e ($n)" || { echo "[FAIL] address -e: got $n"; fail=1; }

# BSGS: the first public key is G itself (private key 1) and plain BSGS has never
# reported it, so 11 of the 12 keys are expected. This is the behaviour of the
# original code, kept here as a regression guard.
head -12 "$TESTS/1to63_65.txt" > pub12.txt
n=$("$KH" -m bsgs -f pub12.txt -r 1:FFFFFFFF -n 0x1000000 -t 4 -q -s 0 -B sequential 2>&1 | count "found privkey")
check "bsgs" 11 "$n"

exit $fail
