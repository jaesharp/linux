#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Assert the compiler emitted what the critical sections require.
#
# Three of this library's operations are correct only in the machine code, and
# nothing in the C says so loudly enough for a reader or a compiler to be held
# to it:
#
#   - a wake is sync, copy, paste., sync, in that order. The copy buffer is
#     hidden state the architecture may discard at any interrupt, so nothing
#     may be scheduled between the copy and the paste that could take one; and
#     the barriers are what order the caller's stores against the wake, which
#     carries no data of its own.
#
#   - a wait is the wait instruction, encoded by value under VAS_ISA_DIRECT
#     because a toolchain old enough to build this may not assemble the
#     mnemonic. If it were dropped or turned into a nop the loop would spin
#     and every measurement would still look plausible.
#
#   - the flag a waiter loops on is loaded with acquire ordering, which on
#     this architecture is a load, a comparison against itself, a branch that
#     is never taken, and isync. Lose the isync and the loop may see the flag
#     before the stores the sender made under it.
#
# None of that is visible in a build log, and all of it would still run. So it
# is checked here rather than trusted.
#
#   OBJDUMP=powerpc64le-linux-gnu-objdump ./compiled-as-expected.sh ../vas.o

set -u

OBJDUMP=${OBJDUMP:-objdump}
OBJECT=${1:-../vas.o}
failed=0

if [ ! -f "$OBJECT" ]; then
	echo "no $OBJECT: build the library first" >&2
	exit 1
fi

body() {
	"$OBJDUMP" -d "$OBJECT" --disassemble="$1" 2>/dev/null |
		sed -n "/<$1>:/,/blr/p"
}

# The mnemonics of $2..$n appear in $body in that order.
in_order() {
	name=$1
	shift
	text=$(body "$name")
	if [ -z "$text" ]; then
		echo "  FAIL $name: not found in $OBJECT"
		failed=$((failed + 1))
		return
	fi
	rest=$text
	for want in "$@"; do
		# Cut everything up to and including the first line matching.
		match=$(printf '%s\n' "$rest" | grep -n -m1 "[[:space:]]$want" | cut -d: -f1)
		if [ -z "$match" ]; then
			echo "  FAIL $name: expected '$want' after the ones before it"
			failed=$((failed + 1))
			return
		fi
		rest=$(printf '%s\n' "$rest" | tail -n "+$((match + 1))")
	done
	echo "  ok   $name: $*"
}

# $3 is the instruction immediately after $2, with nothing in between.
adjacent() {
	name=$1
	from=$2
	to=$3
	line=$(body "$name" | grep -n "[[:space:]]$from" | head -1 | cut -d: -f1)
	if [ -z "$line" ]; then
		echo "  FAIL $name: no $from found"
		failed=$((failed + 1))
		return
	fi
	next=$(body "$name" | sed -n "$((line + 1))p")
	if ! printf '%s\n' "$next" | grep -q "[[:space:]]$to"; then
		echo "  FAIL $name: $to does not immediately follow $from"
		echo "         got:$(printf '%s' "$next" | sed 's/^[^\t]*\t*//')"
		failed=$((failed + 1))
		return
	fi
	echo "  ok   $name: $to immediately follows $from"
}

# No instruction matching $2 appears between the $3 and $4 lines.
nothing_between() {
	name=$1
	forbid=$2
	from=$3
	to=$4
	span=$(body "$name" | sed -n "/[[:space:]]$from/,/[[:space:]]$to/p")
	# An absent function gives an empty span, and every instruction is
	# absent from an empty span. Nothing is not proof of anything.
	if ! printf '%s\n' "$span" | grep -q "[[:space:]]$from"; then
		echo "  FAIL $name: no $from to check between (function missing?)"
		failed=$((failed + 1))
		return
	fi
	if ! printf '%s\n' "$span" | grep -q "[[:space:]]$to"; then
		echo "  FAIL $name: $from with no $to after it"
		failed=$((failed + 1))
		return
	fi
	if printf '%s\n' "$span" | grep -q "[[:space:]]$forbid"; then
		echo "  FAIL $name: '$forbid' between $from and $to"
		printf '%s\n' "$span" | sed 's/^/         /'
		failed=$((failed + 1))
		return
	fi
	echo "  ok   $name: no $forbid between $from and $to"
}

# No instruction matching $2 appears anywhere in $1.
absent() {
	name=$1
	forbid=$2
	text=$(body "$name")
	if [ -z "$text" ]; then
		echo "  FAIL $name: not found in $OBJECT"
		failed=$((failed + 1))
		return
	fi
	if printf '%s\n' "$text" | grep -q "[[:space:]]$forbid"; then
		echo "  FAIL $name: contains '$forbid'"
		failed=$((failed + 1))
		return
	fi
	echo "  ok   $name: no $forbid"
}

echo "disassembly of $OBJECT with $OBJDUMP:"

# The wake: ordered, and nothing at all between the copy and the paste.
# Section 4.4 asks for that directly -- "it is always best to avoid
# unnecessary instructions between the copy and the paste" -- because every
# instruction there is another chance to take the interruption that discards
# the copy buffer. The paste address is loaded before the barrier so that the
# pair can be adjacent, and this is what holds it that way.
in_order vas_wake hwsync copy 'paste\.' hwsync
adjacent vas_wake copy 'paste\.'
in_order vas_send hwsync copy 'paste\.' hwsync
adjacent vas_send copy 'paste\.'

# The send in two halves: the stage is the copy and nothing after it, the
# commit is the paste and nothing before it, and the abandon is the abort.
# Whichever way the library was built to issue them, this is what it must
# come to.
in_order vas_send_stage hwsync copy
absent vas_send_stage 'paste\.'
in_order vas_send_commit 'paste\.' hwsync
absent vas_send_commit copy
in_order vas_send_abandon cpabort

# The wait, in the primitive and in the loop built on it.
in_order vas_wait wait
in_order vas_destination_wait wait

# The acquire the loop's load needs. Compilers lay the loop out either way
# round -- load first with the wait at the bottom, or the other way -- and
# what must hold in both is that the flag's load is followed by an isync
# before the loop can leave, and that the wait is there.
in_order vas_destination_wait lwz isync
in_order vas_destination_wait wait

if [ "$failed" -ne 0 ]; then
	echo "$failed check(s) failed" >&2
	exit 1
fi

exit 0
