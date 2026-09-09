#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Wake latency at several held clocks, so the fixed part can be told from the
# part that scales with the core.
#
#   ./clock-sweep.sh <records.ndjson> [runs-per-point]
#
# A latency measured at one frequency is a single number that two very
# different machines would produce: one whose cost is a fabric traversal, and
# one whose cost is instructions in the sending core. They are told apart by
# varying the clock, since only the second moves. That is the whole reason this
# exists, and it needs the clock to be held at each point rather than merely
# observed -- a sweep across a clock that wanders is a sweep of nothing.
#
# Every run is written out, not an average of them. The model downstream has to
# see the spread to say anything honest about the uncertainty, and averaging
# here would hide exactly the run-to-run variation that decides whether a fit
# means anything.
#
# Each clock point gets its own environment record, because the point of the
# exercise is that the machine differed between them.
#
# Run as root: holding the clock needs it.

set -eu

RECORDS=${1:?usage: clock-sweep.sh <records.ndjson> [runs-per-point]}
RUNS=${2:-5}

HERE=$(cd "$(dirname "$0")" && pwd)
WAKE_TOD=$HERE/../examples/wake_tod
PROVENANCE=$HERE/../examples/provenance
PIN=$HERE/pin-clock.sh

for tool in "$WAKE_TOD" "$PROVENANCE" "$PIN"; do
	[ -x "$tool" ] || { echo "missing $tool" >&2; exit 1; }
done

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

online_list=$(expand "$(cat /sys/devices/system/cpu/online)")
isolated_list=$(for c in $(expand "$(cat /sys/devices/system/cpu/isolated)"); do
	echo "$online_list" | grep -qx "$c" && echo "$c"
done)
is_isolated() { echo "$isolated_list" | grep -qx "$1"; }

# The same four relationships the matrix uses, from an anchor that can fill all
# of them; see wake-matrix.sh for why the first isolated CPU will not do.
anchor=""
for cand in $isolated_list; do
	sib=$(expand "$(cat /sys/devices/system/cpu/cpu$cand/topology/thread_siblings_list)")
	l3set=$(expand "$(cat /sys/devices/system/cpu/cpu$cand/cache/index3/shared_cpu_list)")
	home=""
	for n in /sys/devices/system/node/node*; do
		expand "$(cat "$n/cpulist")" | grep -qx "$cand" && home=$n
	done
	[ -n "$home" ] || continue
	homeset=$(expand "$(cat "$home/cpulist")")

	a_smt=$(echo "$sib" | grep -vx "$cand" | while IFS= read -r c; do
		is_isolated "$c" && echo "$c"; done | head -1)
	a_l3=$(echo "$l3set" | grep -vx "$cand" | grep -vx "${a_smt:-x}" |
		while IFS= read -r c; do is_isolated "$c" && echo "$c"; done | head -1)
	a_local=$(echo "$isolated_list" | while IFS= read -r c; do
		echo "$l3set" | grep -qx "$c" && continue
		echo "$homeset" | grep -qx "$c" && echo "$c"
	done | head -1)
	a_remote=$(echo "$isolated_list" | while IFS= read -r c; do
		echo "$homeset" | grep -qx "$c" || echo "$c"
	done | head -1)

	if [ -n "$a_smt" ] && [ -n "$a_l3" ] && [ -n "$a_local" ] && [ -n "$a_remote" ]; then
		anchor=$cand
		smt=$a_smt; l3=$a_l3; local_peer=$a_local; remote=$a_remote
		break
	fi
done

[ -n "$anchor" ] || { echo "no cpu can fill all four placements" >&2; exit 1; }

: > "$RECORDS"

echo "  anchor cpu $anchor: smt $smt, l3 $l3, chip-local $local_peer, chip-remote $remote"

# Every frequency the pstate table offers, thinned to a handful spread across
# its range: more points constrain the fit, but each costs a full collection.
available=$(tr ' ' '\n' < /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies |
	    grep -v '^$' | sort -n)
count=$(echo "$available" | wc -l)
step=$((count / 4))
[ "$step" -lt 1 ] && step=1
points=$(echo "$available" | awk -v s="$step" 'NR % s == 1')

for khz in $points; do
	if ! "$PIN" "$khz" >/dev/null 2>&1; then
		echo "  $khz kHz: could not hold, skipping" >&2
		continue
	fi

	# The machine at this point, since it is deliberately not the machine
	# at the last one.
	"$PROVENANCE" >> "$RECORDS" 2>/dev/null || {
		echo "  $khz kHz: machine not fit, skipping" >&2
		continue
	}

	r=1
	while [ "$r" -le "$RUNS" ]; do
		for pair in "smt:$smt" "l3:$l3" "chip-local:$local_peer" "chip-remote:$remote"; do
			name=${pair%%:*}
			peer=${pair#*:}
			timeout 120 "$WAKE_TOD" --trials 4000 --settle 200 \
				--send-cpu "$anchor" --wait-cpu "$peer" \
				--label "$name" >> "$RECORDS" 2>/dev/null ||
				echo "  $khz kHz $name run $r did not complete" >&2
		done
		r=$((r + 1))
	done
	echo "  $khz kHz done"
done

echo "  collected $(grep -c '"label"' "$RECORDS") runs into $RECORDS"
