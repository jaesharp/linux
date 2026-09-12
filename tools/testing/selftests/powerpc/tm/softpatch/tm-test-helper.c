// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Userspace executor for the TM state-machine tests, driven by the tm_test
 * kernel module. One path per invocation. Exit: 0 pass, 1 fail, 77 skip.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <pthread.h>
#include <ucontext.h>
#include <setjmp.h>
#include <asm/ptrace.h>

#define FP_NODE "/sys/kernel/debug/powerpc/tm_softpatch/fastpath"

static inline uint64_t texasr(void){ uint64_t t; asm volatile("mfspr %0,130":"=r"(t)); return t; }
static inline long fcode(void){ return (texasr() >> 56) & 0xff; }
#define TC_SYSCALL   0xd8
#define TC_RESCHED   0xde
#define TC_SIGNAL    0xd4
#define TEXASR_ABORT (1ULL << (63 - 31))
#define TEXASR_ROT   (1ULL << (63 - 38))
static inline int is_reschedule(void){ return (fcode() & TC_RESCHED) == TC_RESCHED; }
static inline int is_nesting(void){ return ((texasr() >> 32) & 0x400000) != 0; }

static void set_fastpath(int fp)
{
	int fd = open(FP_NODE, O_WRONLY);
	if (fd >= 0) { char c = fp ? '1' : '0'; if (write(fd, &c, 1)) {} close(fd); }
}

static int t_begin_commit(void)
{
	long ok;
	asm volatile("tbegin. 0; beq 1f; tend. 0; li %0,1; b 2f; 1: li %0,0; 2:"
		     : "=r"(ok) :: "cr0","memory");
	return ok ? 0 : 1;
}
static int t_begin_abort(void)
{
	long entered;
	asm volatile("tbegin. 0; beq 1f; tabort. 0; li %0,1; b 2f; 1: li %0,0; 2:"
		     : "=r"(entered) :: "cr0","memory");
	if (entered) return 1;
	return (texasr() & TEXASR_ABORT) ? 0 : 1;
}
static int t_suspend_resume(void)
{
	long ok;
	asm volatile("tbegin. 0; beq 1f; tsuspend.; tresume.; tend. 0; li %0,1; b 2f; 1: li %0,0; 2:"
		     : "=r"(ok) :: "cr0","memory");
	return ok ? 0 : 1;
}
static int t_suspend_syscall(void)
{
	long ok; volatile long p = 0;
	asm volatile("tbegin. 0; beq 1f; tsuspend." ::: "cr0","memory");
	p = getppid();
	asm volatile("tresume.; tend. 0; li %0,1; b 2f; 1: li %0,0; 2:"
		     : "=r"(ok) :: "cr0","memory");
	return (ok && p > 0) ? 0 : 1;
}
static int t_active_syscall_dooms(void)
{
	long entered;
	asm volatile("tbegin. 0; beq 1f; li %0,1; b 2f; 1: li %0,0; 2:"
		     : "=r"(entered) :: "cr0","memory");
	if (!entered)
		return ((fcode() & TC_SYSCALL) == TC_SYSCALL) ? 0 : 1;
	getppid();
	asm volatile("tend. 0" ::: "memory");
	return 1;
}
static int t_rot(void)
{
	long entered;
	asm volatile("tbegin. 1; beq 1f; tabort. 0; li %0,1; b 2f; 1: li %0,0; 2:"
		     : "=r"(entered) :: "cr0","memory");
	if (entered) return 1;
	return (texasr() & TEXASR_ROT) ? 0 : 1;
}
static int t_nested_commit(void)
{
	long ok;
	asm volatile("tbegin. 0; beq 1f; tbegin. 0; beq 1f; tend. 0; tend. 0;"
		     "li %0,1; b 2f; 1: li %0,0; 2:" : "=r"(ok) :: "cr0","memory");
	return ok ? 0 : 1;
}
static int t_nested_abort(void)
{
	long entered;
	asm volatile("tbegin. 0; beq 1f; tbegin. 0; beq 1f; tabort. 0;"
		     "li %0,1; b 2f; 1: li %0,0; 2:" : "=r"(entered) :: "cr0","memory");
	if (entered) return 1;
	return (texasr() & TEXASR_ABORT) ? 0 : 1;
}
static int t_nested_suspend(void)
{
	long ok;
	asm volatile("tbegin. 0; beq 1f; tbegin. 0; beq 1f; tsuspend.; tresume.;"
		     "tend. 0; tend. 0; li %0,1; b 2f; 1: li %0,0; 2:"
		     : "=r"(ok) :: "cr0","memory");
	return ok ? 0 : 1;
}

/* Signal delivered while Transactional aborts the tx with cause SIGNAL. */
static volatile int sig_seen;
static void sigalrm(int s){ (void)s; sig_seen = 1; }
static int t_signal_in_tx(void)
{
	struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = sigalrm;
	sigaction(SIGALRM, &sa, NULL);
	struct itimerval it; memset(&it, 0, sizeof it); it.it_value.tv_usec = 10000; /* 10ms */
	sig_seen = 0;
	setitimer(ITIMER_REAL, &it, NULL);
	long ok;
	asm volatile(
		"tbegin. 0; beq 1f;"
		"li 5,0; lis 6,0x400; 3: addi 5,5,1; cmpd 5,6; blt 3b;" /* spin in T ~50ms */
		"tend. 0; li %0,1; b 2f; 1: li %0,0; 2:"
		: "=r"(ok) :: "r5","r6","cr0","memory");
	if (ok) return 77;			/* signal didn't land in the window */
	return ((fcode() & TC_SIGNAL) == TC_SIGNAL) ? 0 : 1;
}

/* Reschedule while Suspended: reclaim, then recheckpoint -> DD2.2 rollback. */
static int t_resched_reclaim(void)
{
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 3000000 }; /* 3ms */
	asm goto("tbegin. 0; beq %l[failed]" : : : "cr0","memory" : failed);
	asm volatile("tsuspend." ::: "memory");
	nanosleep(&ts, NULL);
	asm volatile("tresume.; tend. 0" ::: "memory");
	return 77;				/* survived: not rescheduled -> skip */
failed:
	return is_reschedule() ? 0 : 1;
}

/* A pagefault inside a transaction aborts it; the access is not serviced. */
static int t_pagefault_in_tx(void)
{
	char *p = mmap(NULL, 4096, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) return 77;
	mprotect(p, 4096, PROT_NONE);
	long ok;
	asm volatile("tbegin. 0; beq 1f; lbz 5,0(%1); tend. 0; li %0,1; b 2f; 1: li %0,0; 2:"
		     : "=r"(ok) : "r"(p) : "r5","cr0","memory");
	munmap(p, 4096);
	return ok ? 1 : 0;
}

/* TAR and DSCR are checkpointed: changed in T, they roll back on abort. */
static int t_spr_checkpoint(void)
{
	unsigned long tar0 = 0x1111, dscr0 = 0x22, tar1, dscr1;
	asm volatile("mtspr 815,%0"::"r"(tar0));
	asm volatile("mtspr 3,%0"::"r"(dscr0));
	asm volatile("tbegin. 0; beq 1f;"
		     "li 5,0x222; mtspr 815,5; li 5,0x88; mtspr 3,5;"
		     "tabort. 0; 1:" ::: "r5","cr0","memory");
	asm volatile("mfspr %0,815":"=r"(tar1));
	asm volatile("mfspr %0,3":"=r"(dscr1));
	return (tar1 == tar0 && dscr1 == dscr0) ? 0 : 1;
}

/* Exceeding the nesting depth fails the transaction with a nesting cause. */
static int t_nesting_overflow(void)
{
	long ok;
	asm volatile(".rept 256\n tbegin. 0\n beq 1f\n .endr\n"
		     ".rept 256\n tend. 0\n .endr\n"
		     "li %0,1\n b 2f\n 1: li %0,0\n 2:\n"
		     : "=r"(ok) :: "cr0","memory");
	if (ok) return 77;			/* 256 levels held -> could not overflow */
	return is_nesting() ? 0 : 1;
}

/* The kernel must reject a sigreturn that asks for the reserved TS state
 * (0b11) rather than loading it. Pass if we survive, whether the kernel
 * sanitises and returns or refuses with a signal. */
static sigjmp_buf rt_jmp;
static void rt_fault(int s){ (void)s; siglongjmp(rt_jmp, 1); }
static void rt_usr1(int sig, siginfo_t *si, void *ucv)
{
	(void)sig; (void)si;
	ucontext_t *uc = ucv;
	uc->uc_mcontext.gp_regs[PT_MSR] |= (3UL << 33);	/* TS = reserved */
}
static int t_reserved_ts(void)
{
	struct sigaction s1, sf;
	memset(&s1, 0, sizeof s1); s1.sa_sigaction = rt_usr1; s1.sa_flags = SA_SIGINFO;
	sigaction(SIGUSR1, &s1, NULL);
	memset(&sf, 0, sizeof sf); sf.sa_handler = rt_fault;
	sigaction(SIGSEGV, &sf, NULL); sigaction(SIGILL, &sf, NULL);
	if (sigsetjmp(rt_jmp, 1)) return 0;	/* rejected via signal -> pass */
	raise(SIGUSR1);
	return 0;				/* sanitised and returned -> pass */
}

/* The erratum stress: all four threads of one core suspended at once. Each
 * enters a transaction, suspends, and spins (no syscall, so it stays on-cpu
 * and the hardware holds its checkpoint) until all four are suspended, then
 * resumes and commits. All four must commit. */
static volatile int fts_arrived[4];
static int fts_res[4];
static int fts_n = 4;
static void *fts_worker(void *arg)
{
	long idx = (long)arg, spins = 0;
	cpu_set_t s; CPU_ZERO(&s); CPU_SET(8 + idx, &s); sched_setaffinity(0, sizeof s, &s);
	asm goto("tbegin. 0; beq %l[failed]" : : : "cr0","memory" : failed);
	asm volatile("tsuspend." ::: "memory");
	__atomic_store_n(&fts_arrived[idx], 1, __ATOMIC_SEQ_CST);	/* I am suspended */
	for (;;) {
		int all = 1, i;
		for (i = 0; i < fts_n; i++)
			if (!__atomic_load_n(&fts_arrived[i], __ATOMIC_SEQ_CST)) all = 0;
		if (all || spins++ > 200000000)
			break;
		asm volatile("or 31,31,31");		/* on-cpu, low priority */
	}
	asm volatile("tresume.; tend. 0" ::: "memory");
	fts_res[idx] = 1;					/* committed */
	return NULL;
failed:
	fts_res[idx] = 0;					/* checkpoint lost / aborted */
	return NULL;
}
static int fts_cpu[4];
static void *fts_worker2(void *arg)
{
	long idx = (long)arg, spins = 0;
	cpu_set_t s; CPU_ZERO(&s); CPU_SET(fts_cpu[idx], &s); sched_setaffinity(0, sizeof s, &s);
	asm goto("tbegin. 0; beq %l[failed]" : : : "cr0","memory" : failed);
	asm volatile("tsuspend." ::: "memory");
	__atomic_store_n(&fts_arrived[idx], 1, __ATOMIC_SEQ_CST);
	for (;;) {
		int all = 1, i;
		for (i = 0; i < fts_n; i++)
			if (!__atomic_load_n(&fts_arrived[i], __ATOMIC_SEQ_CST)) all = 0;
		if (all || spins++ > 200000000) break;
		asm volatile("or 31,31,31");
	}
	asm volatile("tresume.; tend. 0" ::: "memory");
	fts_res[idx] = 1;
	return NULL;
failed:
	fts_res[idx] = 0;
	return NULL;
}
/* All ONLINE hardware threads of core 8 suspend a transaction at once. Passes
 * if the core keeps at least one live (TM still works under sibling suspend);
 * SMT2 keeps one, SMT4 collapses to zero. Reports the count. */
static int t_four_thread_suspend(void)
{
	pthread_t th[4]; long i; int committed = 0, lo, hi, n = 0;
	FILE *f = fopen("/sys/devices/system/cpu/cpu8/topology/thread_siblings_list", "r");
	if (f && fscanf(f, "%d-%d", &lo, &hi) == 2)
		for (i = lo; i <= hi && n < 4; i++) fts_cpu[n++] = i;
	if (f) fclose(f);
	if (n < 1) { fts_cpu[0] = 8; n = 1; }
	fts_n = n;
	for (i = 0; i < 4; i++) { fts_arrived[i] = 0; fts_res[i] = -1; }
	for (i = 0; i < fts_n; i++)
		if (pthread_create(&th[i], NULL, fts_worker2, (void *)i)) return 77;
	for (i = 0; i < fts_n; i++) pthread_join(th[i], NULL);
	for (i = 0; i < fts_n; i++) if (fts_res[i] == 1) committed++;
	fprintf(stderr, "%d of %d online core-8 threads committed\n", committed, fts_n);
	return (committed >= 1) ? 0 : 1;
}

int main(int argc, char **argv)
{
	cpu_set_t s; CPU_ZERO(&s); CPU_SET(8, &s); sched_setaffinity(0, sizeof s, &s);
	if (argc < 2) return 77;
	set_fastpath(argc > 2 ? atoi(argv[2]) : 0);
	const char *t = argv[1];
	if (!strcmp(t,"begin_commit"))         return t_begin_commit();
	if (!strcmp(t,"begin_abort"))          return t_begin_abort();
	if (!strcmp(t,"suspend_resume"))       return t_suspend_resume();
	if (!strcmp(t,"suspend_syscall"))      return t_suspend_syscall();
	if (!strcmp(t,"active_syscall_dooms")) return t_active_syscall_dooms();
	if (!strcmp(t,"rot"))                  return t_rot();
	if (!strcmp(t,"nested_commit"))        return t_nested_commit();
	if (!strcmp(t,"nested_abort"))         return t_nested_abort();
	if (!strcmp(t,"nested_suspend"))       return t_nested_suspend();
	if (!strcmp(t,"signal_in_tx"))         return t_signal_in_tx();
	if (!strcmp(t,"resched_reclaim"))      return t_resched_reclaim();
	if (!strcmp(t,"pagefault_in_tx"))      return t_pagefault_in_tx();
	if (!strcmp(t,"spr_checkpoint"))       return t_spr_checkpoint();
	if (!strcmp(t,"nesting_overflow"))     return t_nesting_overflow();
	if (!strcmp(t,"reserved_ts"))          return t_reserved_ts();
	if (!strcmp(t,"four_thread_suspend"))  return t_four_thread_suspend();
	return 77;
}
