#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# What actually crossed the memory controllers while a command ran.
#
#   ./traffic.sh <command> [arguments ...]
#
# Bandwidth is the wrong number for judging a design that exists to move each
# byte once. Two arrangements can saturate the same bus and differ by a factor
# of several in useful work, because one of them moved the same bytes in and
# out repeatedly -- copied into a working buffer, evicted by the next stage,
# fetched back for the reduction. The quantity that decides it is how many
# bytes crossed the controller for each byte the work actually needed, and that
# cannot be inferred from a rate.
#
# POWER9 counts it directly. Each memory controller synchronous unit reports
# the 64- and 128-byte reads and writes it dispatched, and -- separately -- the
# atomic operations it performed, which is the thing that makes an atomic
# reduction different in kind rather than in degree: it appears here as work
# done at the controller and not as a line fetched, modified and written back.
#
# The counters are chip-wide and count everything, so an idle measurement is
# taken first and subtracted. On a machine with other tenants that subtraction
# is a guess; on an isolated one it is close to exact, and the idle figure is
# printed so a reader can judge which they have.
#
# Run as root: the nest counters are system-wide.

set -eu

[ $# -ge 1 ] || { echo "usage: traffic.sh <command> [arguments ...]" >&2; exit 2; }

EVENTS=""
for unit in nest_mcs01_imc nest_mcs23_imc; do
	base=/sys/bus/event_source/devices/$unit
	[ -d "$base" ] || continue
	for e in $(ls "$base/events" | grep -v '\.scale$\|\.unit$'); do
		EVENTS="$EVENTS -e $unit/$e/"
	done
done
[ -n "$EVENTS" ] || { echo "no memory controller counters on this machine" >&2; exit 1; }

# The events carry a scale of 256 -- the hardware samples one operation in that
# many -- and perf applies it when it reads the counter. Applying it again here
# multiplied every figure by two hundred and fifty-six, which read as 587 GB/s
# on a run that moved two gigabytes.
SCALE=1

idle=$(mktemp)
busy=$(mktemp)
trap 'rm -f "$idle" "$busy"' EXIT

# What the machine does when asked to do nothing, for as long as the command
# will run. Measured first so it cannot include the command's own aftermath.
# shellcheck disable=SC2086
perf stat $EVENTS -a -x, -o "$idle" -- sleep 1 >/dev/null 2>&1 || true

start=$(date +%s%N)
# shellcheck disable=SC2086
perf stat $EVENTS -a -x, -o "$busy" -- "$@" >/dev/null 2>&1 || true
elapsed=$(( ($(date +%s%N) - start) ))

# Bytes, not dispatches. A 128-byte read and a 64-byte read are both one
# dispatch and move different amounts, so the event name carries the width and
# the count has to be weighted by it -- adding them unweighted understates a
# read-heavy run by up to a factor of two.
bytes() {
	awk -F, -v want="$2" -v scale="$SCALE" '
		$3 ~ want {
			gsub(/[^0-9]/, "", $1)
			width = ($3 ~ /128B/) ? 128 : 64
			total += $1 * scale * width
		}
		END { print total + 0 }
	' "$1"
}

# Operations, which have no width: an atomic is work done, not bytes moved.
operations() {
	awk -F, -v want="$2" -v scale="$SCALE" '
		$3 ~ want { gsub(/[^0-9]/, "", $1); total += $1 * scale }
		END { print total + 0 }
	' "$1"
}

read_idle=$(bytes "$idle" 'RD_DISP')
write_idle=$(bytes "$idle" 'WR_DISP')
amo_idle=$(operations "$idle" 'AMO_OP')
read_busy=$(bytes "$busy" 'RD_DISP')
write_busy=$(bytes "$busy" 'WR_DISP')
amo_busy=$(operations "$busy" 'AMO_OP')

seconds=$(awk -v n="$elapsed" 'BEGIN{printf "%.4f", n/1e9}')

# The idle sample ran for a second; scale it to the command's duration before
# subtracting, or a long command has a rounding error taken off it and a short
# one goes negative.
awk -v ri="$read_idle" -v wi="$write_idle" -v ai="$amo_idle" \
    -v rb="$read_busy" -v wb="$write_busy" -v ab="$amo_busy" \
    -v s="$seconds" '
BEGIN {
	r = rb - ri * s; if (r < 0) r = 0
	w = wb - wi * s; if (w < 0) w = 0
	a = ab - ai * s; if (a < 0) a = 0
	printf "  ran for %.3f s\n", s
	printf "  read      %12.1f MiB   %8.2f GB/s\n", r/1048576, r/s/1e9
	printf "  written   %12.1f MiB   %8.2f GB/s\n", w/1048576, w/s/1e9
	printf "  atomics   %12.0f operations at the controller\n", a
	printf "  idle background was %.1f MiB/s read, %.1f MiB/s written\n",
	       ri/1048576, wi/1048576
}'
