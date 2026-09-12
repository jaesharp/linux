// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tm_test: kernel-driven harness for the POWER9 TM state machine. debugfs is
 * the control plane; the real transactions run in a userspace helper it
 * launches, because suspend and resume are only safe in user mode on DD2.2.
 *
 *   echo all         > /sys/kernel/debug/tm_test/run
 *   echo <testname>  > /sys/kernel/debug/tm_test/run
 *   cat                /sys/kernel/debug/tm_test/results
 *
 * Each test is run twice, fast path off then on, and both must agree.
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/umh.h>
#include <linux/mutex.h>
#include <linux/slab.h>

static char helper_path[128] = "/var/tmp/tm-test-helper";
static char results[8192];
static DEFINE_MUTEX(tm_test_lock);

static const char * const tests[] = {
	"begin_commit", "begin_abort", "suspend_resume", "suspend_syscall",
	"active_syscall_dooms", "rot",
	"nested_commit", "nested_abort", "nested_suspend",
	"signal_in_tx", "resched_reclaim", "pagefault_in_tx",
	"spr_checkpoint", "nesting_overflow",
	"reserved_ts", "four_thread_suspend",
};

/* call_usermodehelper with UMH_WAIT_PROC returns the child's exit code in the
 * low byte of the wait status; decode it, or a negative errno if it could not
 * be run. */
static int run_helper(const char *test, int fp)
{
	char fps[2] = { fp ? '1' : '0', 0 };
	char *argv[] = { helper_path, (char *)test, fps, NULL };
	char *envp[] = { "PATH=/usr/bin:/bin", NULL };
	int ret = call_usermodehelper(helper_path, argv, envp, UMH_WAIT_PROC);

	if (ret < 0)
		return ret;			/* could not launch */
	return (ret >> 8) & 0xff;		/* exit code */
}

static const char *verdict(int off, int on)
{
	if (off < 0 || on < 0)		return "ERROR(launch)";
	if (off == 77 && on == 77)	return "skip";
	if (off == 0 && on == 0)	return "PASS";
	if (off != on)			return "FAIL(fastpath differs)";
	return "FAIL";
}

static void run_all(void)
{
	int i, off, on, n = 0;

	n += scnprintf(results + n, sizeof(results) - n,
		       "%-24s %-6s %-6s %s\n", "test", "fp=off", "fp=on", "verdict");
	for (i = 0; i < ARRAY_SIZE(tests); i++) {
		off = run_helper(tests[i], 0);
		on  = run_helper(tests[i], 1);
		n += scnprintf(results + n, sizeof(results) - n,
			       "%-24s %-6d %-6d %s\n", tests[i], off, on,
			       verdict(off, on));
	}
}

static ssize_t run_write(struct file *f, const char __user *u, size_t len, loff_t *o)
{
	char buf[32];
	int i;

	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, u, len))
		return -EFAULT;
	buf[len] = 0;
	strim(buf);

	mutex_lock(&tm_test_lock);
	if (!strcmp(buf, "all")) {
		run_all();
	} else {
		int off = -EINVAL, on = -EINVAL, m = 0;

		for (i = 0; i < ARRAY_SIZE(tests); i++)
			if (!strcmp(buf, tests[i])) {
				off = run_helper(buf, 0);
				on  = run_helper(buf, 1);
			}
		m += scnprintf(results + m, sizeof(results) - m,
			       "%-24s %-6d %-6d %s\n", buf, off, on, verdict(off, on));
	}
	mutex_unlock(&tm_test_lock);
	return len;
}

static ssize_t results_read(struct file *f, char __user *u, size_t len, loff_t *o)
{
	return simple_read_from_buffer(u, len, o, results, strlen(results));
}

static const struct file_operations run_fops = { .write = run_write, .llseek = noop_llseek };
static const struct file_operations results_fops = { .read = results_read, .llseek = default_llseek };

static struct dentry *dir;

static int __init tm_test_init(void)
{
	dir = debugfs_create_dir("tm_test", NULL);
	debugfs_create_file("run", 0200, dir, NULL, &run_fops);
	debugfs_create_file("results", 0400, dir, NULL, &results_fops);
	strscpy(results, "no run yet\n", sizeof(results));
	pr_info("tm_test: loaded; echo all > /sys/kernel/debug/tm_test/run\n");
	return 0;
}

static void __exit tm_test_exit(void)
{
	debugfs_remove_recursive(dir);
}

module_init(tm_test_init);
module_exit(tm_test_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel-driven harness for the POWER9 TM state machine");
