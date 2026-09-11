#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Collect wake-latency records across placements, for analyse.py to compare.
#
#   ./wake-latency.sh <records.ndjson> [runs] [iterations] [settle]
#
# Every run uses the same settle, because it adds a constant to each round
# trip: a comparison across different settles compares the settles. analyse.py
# refuses records that disagree about it, so this is enforced rather than
# merely intended.
#
# The placements come from the machine's own topology rather than from
# constants here. Two CPUs from one node give the same-chip case and one from
# each give the cross-chip case, and the CPUs are taken from the nohz_full set
# where it exists, because a housekeeping CPU takes the timer interrupt that a
# lost wake would otherwise hide behind.
#
# Progress goes to the fleet console relay, which is what survives a host that
# stops answering; the records go to the file and nothing else.

set -eu

RECORDS=${1:?usage: wake-latency.sh <records.ndjson> [runs] [iterations] [settle]}
RUNS=${2:-12}
ITERATIONS=${3:-10000}
SETTLE=${4:-1000}

HERE=$(cd "$(dirname "$0")" && pwd)
PINGPONG=$HERE/../examples/pingpong
STATUS=$(command -v fleet-status 2>/dev/null || echo /local/var-tmp/fleet-status)

say() {
	[ -x "$STATUS" ] && "$STATUS" "$*" 2>/dev/null || true
	printf '  %s\n' "$*"
}

[ -x "$PINGPONG" ] || { echo "build examples first: no $PINGPONG" >&2; exit 1; }

# Two CPUs on one node, and one on each of two nodes. Prefer CPUs that are in
# the nohz_full set: a wake lost on a housekeeping CPU is rescued by the timer
# tick quickly enough to look like a slow wake rather than a lost one.
nohz=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo)
pick() {
	# $1 = node number, $2 = how many to take
	list=$(cat "/sys/devices/system/node/node$1/cpulist" 2>/dev/null || echo)
	python3 - "$list" "$nohz" "$2" <<'PY'
import sys
def expand(spec):
    out = []
    for part in spec.split(','):
        part = part.strip()
        if not part:
            continue
        if '-' in part:
            a, b = part.split('-')
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out
cpus, nohz, want = expand(sys.argv[1]), set(expand(sys.argv[2])), int(sys.argv[3])
pool = [c for c in cpus if c in nohz] or cpus
# Spread the picks so two on one node are not the same core's threads.
step = max(1, len(pool) // (want + 1))
print(' '.join(str(pool[(i + 1) * step]) for i in range(want)))
PY
}

nodes=$(ls -d /sys/devices/system/node/node* 2>/dev/null | sed 's/.*node//' | sort -n)
first=$(echo "$nodes" | head -1)
second=$(echo "$nodes" | sed -n 2p)

same=$(pick "$first" 2)
same_a=$(echo "$same" | cut -d' ' -f1)
same_b=$(echo "$same" | cut -d' ' -f2)

: > "$RECORDS"

say "vas wake latency: $RUNS runs of $ITERATIONS, settle $SETTLE"
say "same-chip: node $first cpus $same_a and $same_b"

# Interleaved, not blocked. Running every same-chip measurement and then every
# cross-chip one confounds the comparison with anything that drifts over the
# course of the collection -- the machine warming, a background job starting,
# frequency wandering -- because that drift lands entirely on the second
# condition. Alternating spreads any such trend across both, so it cancels in
# the difference instead of masquerading as it.
if [ -n "$second" ]; then
	cross_b=$(pick "$second" 1)
	say "cross-chip: node $first cpu $same_a and node $second cpu $cross_b"
else
	cross_b=
	say "one node only: no cross-chip condition to collect"
fi

i=1
while [ "$i" -le "$RUNS" ]; do
	"$PINGPONG" --iterations "$ITERATIONS" --settle "$SETTLE" \
		--a-cpu "$same_a" --b-cpu "$same_b" --label same-chip \
		>> "$RECORDS" 2>/dev/null || say "warn: same-chip run $i failed"
	if [ -n "$cross_b" ]; then
		"$PINGPONG" --iterations "$ITERATIONS" --settle "$SETTLE" \
			--a-cpu "$same_a" --b-cpu "$cross_b" --label cross-chip \
			>> "$RECORDS" 2>/dev/null || say "warn: cross-chip run $i failed"
	fi
	i=$((i + 1))
done

say "collected $(wc -l < "$RECORDS") records into $RECORDS"
