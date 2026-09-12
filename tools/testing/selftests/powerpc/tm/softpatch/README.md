# TM state-machine test harness

Kernel-driven exerciser for the POWER9 transactional memory state machine on
DD2.2, where hardware suspend is disabled and the host softpatch handler
emulates suspend and resume. The kernel module is the control plane, driven
from debugfs; the real transactions run in a userspace helper it launches,
because suspend and resume are only safe in user mode on this hardware.

This is not part of the automated powerpc selftest run: the module has to be
built against the running kernel and loaded as root.

## Build and run

    make KDIR=/path/to/kernel/build      # builds tm_test.ko and the helper
    doas insmod tm_test.ko
    echo all > /sys/kernel/debug/tm_test/run     # every case, fast path off then on
    cat        /sys/kernel/debug/tm_test/results

KDIR defaults to `/lib/modules/$(uname -r)/build`. The helper is built to
`/var/tmp/tm-test-helper`, the path the module launches; override with `HELPER=`.

Each case runs twice, with the softpatch fast path off and on, and the verdict
flags any disagreement. Exit code 0 is a pass, 77 a skip (the case could not
create its condition), non-zero a fail.

## What it covers

begin/commit, the three abort forms, suspend/resume, syscall dooming in
transactional state and surviving in suspended, rollback-only, three nested
cases, signal-in-transaction, reschedule-reclaim-rollback, page faults, SPR
checkpointing, nesting overflow, reserved-TS rejection, and a core-suspend
concurrency probe that is SMT-aware: it tests the core's online siblings and
passes if the core keeps at least one transaction. The last is how the
four-thread-suspend collapse at SMT4 was found.
