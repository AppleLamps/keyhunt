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
# run <pattern> <keyhunt args...>: runs keyhunt, fails the test run on a non zero
# exit status (crash, sanitizer report), and leaves the number of matching
# lines in $N. (Piping straight into grep would hide the exit status.)
run() {
	pat=$1; shift
	"$KH" "$@" > run.out 2>&1
	st=$?
	if [ "$st" -ne 0 ]; then
		echo "[FAIL] keyhunt $* exited with status $st"
		tail -5 run.out
		fail=1
	fi
	N=$(grep -c "$pat" run.out)
}

# Keys 1 to 0xFFFFF: 20 of the solved puzzles fall in this range
for mode in compress both; do
	run "Private Key" -m address -f "$TESTS/1to32.txt" -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l $mode
	check "address -l $mode" 20 "$N"
	run "Private Key" -m rmd160 -f "$TESTS/1to32.rmd" -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l $mode
	check "rmd160 -l $mode" 20 "$N"
done

# RIPEMD160 of the uncompressed public key of private key 1
echo 91b24bf9f5288532960ac687abb035127b1d28a5 > unc.rmd
run "Private Key" -m rmd160 -f unc.rmd -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l uncompress
check "rmd160 -l uncompress" 1 "$N"

# Every lane of the 4 and 8 way hashers must be exercised: single threaded run, stride 1
run "Private Key" -m address -f "$TESTS/1to32.txt" -r 1:FFFFF -n 0x100000 -t 1 -q -s 0 -l compress
check "address -t 1" 20 "$N"

# Endomorphism: the six x-only hashes per point go through the 8/16 way kernels
# when available. Every SIMD setting must give the same hits.
for simd in "" avx2 none; do
	export KEYHUNT_SIMD=$simd
	run "Private Key" -m address -f "$TESTS/1to32.txt" -r 1:FFFFF -n 0x100000 -t 1 -q -s 0 -l compress -e
	check "address -e ${simd:-auto}" 20 "$N"
	run "Private Key" -m address -f "$TESTS/1to32.txt" -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l both -e
	check "address -e -l both ${simd:-auto}" 20 "$N"
	run "Private Key: 1$" -m rmd160 -f unc.rmd -r 1:FFFFF -n 0x100000 -t 1 -q -s 0 -l uncompress -e
	check "rmd160 -e -l uncompress ${simd:-auto}" 1 "$N"
	# Vanity: the counts are the ones of the 4 way SSE path
	run "Vanity Private Key" -m vanity -v 1Good -v 1Bad -r 1:FFFFF -n 0x100000 -t 1 -q -s 0 -l compress
	check "vanity ${simd:-auto}" 22 "$N"
	run "Vanity Private Key" -m vanity -v 1Good -v 1Bad -r 1:FFFFF -n 0x100000 -t 1 -q -s 0 -l both
	check "vanity -l both ${simd:-auto}" 33 "$N"
	run "Vanity Private Key" -m vanity -v 1Good -v 1Bad -r 1:FFFFF -n 0x100000 -t 1 -q -s 0 -l compress -e
	check "vanity -e ${simd:-auto}" 82 "$N"
	run "Vanity Private Key" -m vanity -v 1Good -v 1Bad -r 1:FFFFF -n 0x100000 -t 4 -q -s 0 -l both -e
	check "vanity -e -l both ${simd:-auto}" 159 "$N"
done
unset KEYHUNT_SIMD

# BSGS: the first public key is G itself (private key 1) and plain BSGS has never
# reported it, so 11 of the 12 keys are expected. This is the behaviour of the
# original code, kept here as a regression guard.
head -12 "$TESTS/1to63_65.txt" > pub12.txt
run "found privkey" -m bsgs -f pub12.txt -r 1:FFFFFFFF -n 0x1000000 -t 4 -q -s 0 -B sequential
check "bsgs" 11 "$N"

exit $fail
