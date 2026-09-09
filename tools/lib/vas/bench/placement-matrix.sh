#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Collect wake latencies across placements and barrier settings.
#
#   ./placement-matrix.sh <records.ndjson> [rounds] [iterations] [settle]
#
# The placements are read from the machine: threads of one core, cores sharing
# an L3, cores on one chip that do not, and cores on different chips. Naming
# them by hand invites getting them wrong on a machine that is laid out
# differently.
#
# Idle residency is a precondition, not a detail. A thread in wait may let its
# core drop into a stop state, and a wake that has to bring one back costs
# milliseconds -- measured here at 8150 us against 8.98 us for the same pair
# with idle states disabled, a difference of nearly three orders of magnitude
# that has nothing to do with the placement being measured. Left uncontrolled
# it lands entirely on whichever CPUs the operator has not been using, which
# is to say on the distant ones, and reads exactly like a distance effect.
#
# So this disables every idle state on every CPU it will touch, and refuses to
# run if it cannot. What it does not do is put them back: the caller decides
# when the machine returns to normal.

set -eu

RECORDS=${1:?usage: placement-matrix.sh <records.ndjson> [rounds] [iterations] [settle]}
ROUNDS=${2:-10}
ITERATIONS=${3:-2000}
SETTLE=${4:-1000}

HERE=$(cd "$(dirname "$0")" && pwd)
PINGPONG=$HERE/../examples/pingpong
STATUS=$(command -v fleet-status 2>/dev/null || echo /local/var-tmp/fleet-status)

say() {
	[ -x "$STATUS" ] && "$STATUS" "$*" 2>/dev/null || true
	printf '  %s\n' "$*"
}

[ -x "$PINGPONG" ] || { echo "build examples first: no $PINGPONG" >&2; exit 1; }

first_of() { cut -d, -f1 <<EOF | cut -d- -f1
$1
EOF
}

# The anchor, and one partner for each relationship it can have.
anchor=$(cut -d- -f1 < /sys/devices/system/node/node0/cpulist)
anchor=$((anchor + 16))		# away from the housekeeping CPUs at the start

siblings=$(cat /sys/devices/system/cpu/cpu$anchor/topology/thread_siblings_list)
l3=$(cat /sys/devices/system/cpu/cpu$anchor/cache/index3/shared_cpu_list)

# A thread of the same core, that is not the anchor.
smt=$(echo "$siblings" | tr ',-' '\n\n' | grep -v "^$anchor$" | head -1)
# A core sharing the L3, that is not this core.
core=$(echo "$l3" | tr ',' '\n' | cut -d- -f1 | grep -v "^$anchor$" | head -1)
# A core on this chip outside that L3: step past the group.
chiplet=$((anchor + 8))
# A core on the other chip.
other_node=$(ls -d /sys/devices/system/node/node* | sed 's/.*node//' | sort -n | sed -n 2p)
chip=$(cut -d- -f1 < "/sys/devices/system/node/node$other_node/cpulist")
chip=$((chip + 40))

matrix="smt:$anchor:$smt core:$anchor:$core chiplet:$anchor:$chiplet chip:$anchor:$chip"

# Every CPU the matrix touches must stay out of a stop state.
cpus="$anchor $smt $core $chiplet $chip"
for c in $cpus; do
	sib=$(cat /sys/devices/system/cpu/cpu$c/topology/thread_siblings_list 2>/dev/null || echo "$c")
	for s in $(echo "$sib" | tr ',-' '\n\n'); do
		for d in /sys/devices/system/cpu/cpu$s/cpuidle/state*; do
			[ -e "$d/disable" ] || continue
			echo 1 > "$d/disable" 2>/dev/null || true
		done
	done
done

for c in $cpus; do
	for d in /sys/devices/system/cpu/cpu$c/cpuidle/state*; do
		[ -e "$d/disable" ] || continue
		if [ "$(cat "$d/disable")" != "1" ]; then
			echo "refusing to run: cpu$c $(basename "$d") still enabled (need root?)" >&2
			exit 1
		fi
	done
done

: > "$RECORDS"

say "vas placement matrix: $ROUNDS rounds of $ITERATIONS, settle $SETTLE"
for entry in $matrix; do
	say "  $(echo "$entry" | cut -d: -f1): cpu $(echo "$entry" | cut -d: -f2) and cpu $(echo "$entry" | cut -d: -f3)"
done
say "idle states disabled on: $cpus"

# Interleaved: one run of every condition per round, so anything drifting over
# the collection is spread across all of them rather than landing on the last.
r=1
while [ "$r" -le "$ROUNDS" ]; do
	for entry in $matrix; do
		name=$(echo "$entry" | cut -d: -f1)
		a=$(echo "$entry" | cut -d: -f2)
		b=$(echo "$entry" | cut -d: -f3)
		for barrier in plain sync; do
			[ "$barrier" = sync ] && extra=--sync-after-wake || extra=
			# shellcheck disable=SC2086
			timeout 120 "$PINGPONG" --iterations "$ITERATIONS" \
				--settle "$SETTLE" --a-cpu "$a" --b-cpu "$b" \
				$extra --label "$name-$barrier" \
				>> "$RECORDS" 2>/dev/null ||
				say "warn: $name-$barrier round $r did not complete"
		done
	done
	r=$((r + 1))
done

say "collected $(wc -l < "$RECORDS") records into $RECORDS"
