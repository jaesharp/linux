#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Hold the core clock still, so that a time means one thing.
#
#   ./pin-clock.sh [kHz]      hold the clock, by default at the highest nominal
#   ./pin-clock.sh release    give it back to the firmware
#   ./pin-clock.sh show       report where every CPU is, without changing it
#
# The governor being at performance is not enough here. Above the highest
# nominal frequency the firmware runs a boost of its own, decided from power
# and thermal headroom it does not tell the operating system about, so the
# clock moves between the nominal top and the boost ceiling -- a factor of 1.78
# on this machine -- without anything in sysfs changing. Two runs of the same
# loop then differ by that factor and neither is wrong.
#
# Three things are needed to stop it. The boost is switched off, which caps the
# clock at the top of the pstate table. The minimum is then raised to meet the
# maximum, so the governor has one choice. And throttling is checked afterwards,
# because a chip that is over its power or thermal budget will drop below the
# floor whatever the governor was told -- and a pin that silently did not hold
# is worse than no pin, since the numbers look authoritative.
#
# Run as root. Reversible: release restores the range the firmware had.

set -eu

CPUFREQ=/sys/devices/system/cpu/cpufreq
ACTION=${1:-hold}
WANTED=${2:-}
case $ACTION in
[0-9]*) WANTED=$ACTION; ACTION=hold ;;
esac

policies() { ls -d "$CPUFREQ"/policy* 2>/dev/null; }

[ -n "$(policies)" ] || { echo "no cpufreq policies; nothing to pin" >&2; exit 1; }

one=$(policies | head -1)
nominal=$(tr ' ' '\n' < "$one/scaling_available_frequencies" | grep -v '^$' |
	  sort -rn | head -1)
floor=$(tr ' ' '\n' < "$one/scaling_available_frequencies" | grep -v '^$' |
	sort -n | head -1)
ceiling=$(cat "$one/cpuinfo_max_freq")

report() {
	seen=""
	for p in $(policies); do
		cur=$(cat "$p/scaling_cur_freq")
		case " $seen " in *" $cur "*) ;; *) seen="$seen $cur" ;; esac
	done
	printf '  governor %s, boost %s\n' \
		"$(cat "$one/scaling_governor")" \
		"$(cat "$CPUFREQ/boost" 2>/dev/null || echo 'not offered')"
	printf '  range %s to %s kHz, distinct current values:%s\n' \
		"$(cat "$one/scaling_min_freq")" "$(cat "$one/scaling_max_freq")" \
		"$seen"
}

throttled() {
	# Any non-zero reason a chip gave for dropping below what it was asked.
	total=0
	for f in /sys/devices/system/cpu/cpu*/cpufreq/throttle_stats/*; do
		[ -f "$f" ] || continue
		case $(basename "$f") in
		turbo_stat|sub_turbo_stat|unthrottle) continue ;;
		esac
		v=$(cat "$f" 2>/dev/null || echo 0)
		total=$((total + v))
	done
	echo "$total"
}

case $ACTION in
show)
	report
	echo "  throttle events since boot: $(throttled)"
	exit 0
	;;
release)
	[ -w "$CPUFREQ/boost" ] && echo 1 > "$CPUFREQ/boost"
	for p in $(policies); do
		echo "$ceiling" > "$p/scaling_max_freq"
		echo "$floor" > "$p/scaling_min_freq"
	done
	echo "  released to $floor - $ceiling kHz, boost on"
	report
	exit 0
	;;
hold)
	;;
*)
	echo "usage: pin-clock.sh [hold|release|show]" >&2
	exit 2
	;;
esac

# A frequency the table offers, so the governor is not asked for a value it
# will silently round.
if [ -n "$WANTED" ]; then
	tr ' ' '\n' < "$one/scaling_available_frequencies" | grep -qx "$WANTED" || {
		echo "  $WANTED kHz is not in the pstate table" >&2
		exit 1
	}
	nominal=$WANTED
fi

before=$(throttled)

# Boost first: while it is on, the clock can sit above anything set below.
if [ -w "$CPUFREQ/boost" ]; then
	echo 0 > "$CPUFREQ/boost"
else
	echo "  warning: no boost control; the firmware may still move the clock" >&2
fi

# Maximum down before minimum up, or the minimum would exceed the maximum.
for p in $(policies); do
	echo "$nominal" > "$p/scaling_max_freq"
done
for p in $(policies); do
	echo "$nominal" > "$p/scaling_min_freq"
done

echo "  asked for $nominal kHz on every cpu"
report

# Whether it took. A pin that did not hold is worse than none, so this fails
# loudly rather than leaving a run to be interpreted as if it had.
sleep 1
bad=0
for p in $(policies); do
	cur=$(cat "$p/scaling_cur_freq")
	[ "$cur" = "$nominal" ] || bad=$((bad + 1))
done
after=$(throttled)

if [ "$bad" -ne 0 ]; then
	echo "  $bad policies are not at $nominal kHz" >&2
	exit 1
fi
if [ "$after" -ne "$before" ]; then
	echo "  throttled while pinning ($((after - before)) events): the clock will move" >&2
	exit 1
fi

echo "  held: every cpu at $nominal kHz, no throttling"
