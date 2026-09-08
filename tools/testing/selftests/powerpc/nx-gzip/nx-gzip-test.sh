#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later

if [[ ! -w /dev/crypto/nx-gzip ]]; then
	echo "Can't access /dev/crypto/nx-gzip, skipping"
	echo "skip: $0"
	exit 4
fi

set -e

failed=0

function cleanup
{
	# Keep the evidence when something went wrong. A maintainer looking at
	# a report needs the input that provoked it and both intermediate
	# files, and they are at most a few hundred megabytes.
	if [[ $failed -eq 0 ]]; then
		rm -f nx-tempfile*
	else
		echo "note: leaving nx-tempfile* in $(pwd) for inspection"
	fi
}

trap cleanup EXIT

# What someone reading a failure report needs, and cannot get afterwards.
function diagnose
{
	local stage=$1 size=$2 n=$3 fname=$4

	failed=1
	echo "---- nx-gzip failure diagnostics ----"
	echo "stage:        $stage"
	echo "size:         $size    iteration: $n"
	echo "kernel:       $(uname -r)"
	# The MMU mode and page size are the first thing to establish. This
	# path behaves differently under HPT, where a translation the
	# accelerator needs is a cache entry that can be evicted, than under
	# radix, where it walks the same tree the core does.
	echo "mmu / page:   $(awk '/^MMU/ {print $3}' /proc/cpuinfo | head -1)" 	     "/ $(getconf PAGESIZE)"
	echo "platform:     $(cat /proc/device-tree/compatible 2>/dev/null | tr '\0' ' ')"
	echo "device:       $(ls -l /dev/crypto/nx-gzip 2>&1)"
	for f in "$fname" "${fname}.nx.gz" "${fname}.nx.gz.nx.gunzip"; do
		if [[ -e $f ]]; then
			echo "file:         $f  $(stat -c %s "$f") bytes"
		else
			echo "file:         $f  ABSENT"
		fi
	done
	if [[ -e $fname && -e ${fname}.nx.gz.nx.gunzip ]]; then
		echo "first differing byte:"
		cmp "$fname" "${fname}.nx.gz.nx.gunzip" 2>&1 | head -2 || true
	fi
	echo "------------------------------------"
}

function test_sizes
{
	local n=$1
	local fname="nx-tempfile.$n"

	for size in 4K 64K 1M 64M
	do
		echo "Testing $size ($n) ..."
		dd if=/dev/urandom of=$fname bs=$size count=1

		./gzfht_test $fname || { diagnose compress "$size" "$n" "$fname"; return 1; }
		./gunz_test ${fname}.nx.gz || { diagnose decompress "$size" "$n" "$fname"; return 1; }

		# Assert what the test is actually for. Comparing the engine's
		# own checksums against each other only says the hardware
		# agreed with itself; the README documents comparing the files
		# and the script never did, so a round trip that silently
		# corrupted data passed.
		cmp "$fname" "${fname}.nx.gz.nx.gunzip" \
			|| { diagnose compare "$size" "$n" "$fname"; return 1; }
	done
}

echo "Doing basic test of different sizes ..."
test_sizes 0

echo "Running tests in parallel ..."
for i in {1..16}
do
	test_sizes $i &
done

wait

echo "OK"

exit 0
