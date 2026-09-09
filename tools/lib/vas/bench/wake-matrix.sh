#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Every placement a wake can cross, each against its own control.
#
#   ./wake-matrix.sh <records.ndjson> [rounds] [trials]
#
# The placements are read from the machine rather than named here: threads of
# one core, cores sharing an L3, cores on one chip that do not, and cores on
# different chips. A machine laid out differently would make hand-written CPU
# numbers wrong in a way that still runs and still reports.
#
# Every cell is run twice, once sending the wake and once not. Without the
# control a cell reports how long it took a thread to resume, which is a
# number the wake need not have had anything to do with: a thread in wait
# resumes on its own about every microsecond here, which is the same order as
# the wake and would pass for it. The two arms are interleaved so that
# anything drifting over the collection lands on both.
#
# Threads sit only on isolated CPUs, and this refuses to run if there are not
# enough. A resume counted on a CPU the scheduler still uses is not evidence
# about the switchboard.
#
# Latency is measured by the example rather than here: mftb for the clock and
# an atomic Store Add for the arithmetic, so the resume path is two
# instructions. This script only decides what to run and in what order.

set -eu

RECORDS=${1:?usage: wake-matrix.sh <records.ndjson> [rounds] [trials]}
ROUNDS=${2:-12}
TRIALS=${3:-2000}

HERE=$(cd "$(dirname "$0")" && pwd)
WAKE_TOD=$HERE/../examples/wake_tod
STATUS=$(command -v fleet-status 2>/dev/null || echo /local/var-tmp/fleet-status)

say() {
	[ -x "$STATUS" ] && "$STATUS" "$*" 2>/dev/null || true
	printf '  %s\n' "$*"
}

[ -x "$WAKE_TOD" ] || { echo "build examples first: no $WAKE_TOD" >&2; exit 1; }

isolated=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)
[ -n "$isolated" ] || {
	echo "refusing to run: no isolated cpus; boot with isolcpus=" >&2
	exit 1
}

# Expand the list so membership can be tested by string match. The newline
# matters: read returns false at end of input without one, so a list printed
# with %s loses its last element and every derived role silently shifts.
expand() {
	printf '%s\n' "$1" | tr ',' '\n' | while IFS= read -r part; do
		case $part in
		*-*)
			lo=${part%-*}; hi=${part#*-}
			while [ "$lo" -le "$hi" ]; do echo "$lo"; lo=$((lo + 1)); done
			;;
		*) echo "$part" ;;
		esac
	done
}

isolated_list=$(expand "$isolated")
is_isolated() { echo "$isolated_list" | grep -qx "$1"; }

anchor=$(echo "$isolated_list" | head -1)

siblings=$(cat "/sys/devices/system/cpu/cpu$anchor/topology/thread_siblings_list")
l3=$(cat "/sys/devices/system/cpu/cpu$anchor/cache/index3/shared_cpu_list")
# The chip the anchor is on, so "other chip" is read rather than assumed.
home_node=
for n in /sys/devices/system/node/node*; do
	expand "$(cat "$n/cpulist")" | grep -qx "$anchor" && home_node=$n
done
[ -n "$home_node" ] || { echo "cannot find the anchor's node" >&2; exit 1; }
home_cpus=$(expand "$(cat "$home_node/cpulist")")

# A thread of the same core.
smt=$(expand "$siblings" | grep -vx "$anchor" | head -1)
# A core sharing the L3, on another core.
l3_list=$(expand "$l3")
core=$(echo "$l3_list" | grep -vx "$anchor" | grep -vx "${smt:-x}" | while IFS= read -r c; do
	is_isolated "$c" && echo "$c"
done | head -1)
# An isolated CPU on this chip but outside that L3.
chiplet=$(echo "$isolated_list" | while IFS= read -r c; do
	echo "$l3_list" | grep -qx "$c" && continue
	echo "$home_cpus" | grep -qx "$c" && echo "$c"
done | head -1)
# An isolated CPU on any other chip.
chip=$(echo "$isolated_list" | while IFS= read -r c; do
	echo "$home_cpus" | grep -qx "$c" || echo "$c"
done | head -1)

matrix=""
for pair in "smt:$smt" "l3:$core" "chip-local:$chiplet" "chip-remote:$chip"; do
	name=${pair%%:*}; peer=${pair#*:}
	[ -n "$peer" ] || { say "skipping $name: no isolated cpu fills that role"; continue; }
	matrix="$matrix $name:$anchor:$peer"
done

[ -n "$matrix" ] || { echo "no placements available" >&2; exit 1; }

: > "$RECORDS"

say "vas wake matrix: $ROUNDS rounds of $TRIALS trials, anchor cpu $anchor"
for entry in $matrix; do
	say "  $(echo "$entry" | cut -d: -f1): cpu $(echo "$entry" | cut -d: -f2) to cpu $(echo "$entry" | cut -d: -f3)"
done

r=1
while [ "$r" -le "$ROUNDS" ]; do
	for entry in $matrix; do
		name=$(echo "$entry" | cut -d: -f1)
		a=$(echo "$entry" | cut -d: -f2)
		b=$(echo "$entry" | cut -d: -f3)
		for arm in wake nowake; do
			[ "$arm" = nowake ] && extra=--no-wake || extra=
			# shellcheck disable=SC2086
			timeout 120 "$WAKE_TOD" --trials "$TRIALS" --settle 200 \
				--send-cpu "$a" --wait-cpu "$b" $extra \
				--label "$name-$arm" >> "$RECORDS" 2>/dev/null ||
				say "warn: $name-$arm round $r did not complete"
		done
	done
	r=$((r + 1))
done

say "collected $(wc -l < "$RECORDS") records into $RECORDS"
