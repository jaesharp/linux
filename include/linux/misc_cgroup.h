/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Miscellaneous cgroup controller.
 *
 * Copyright 2020 Google LLC
 * Author: Vipin Sharma <vipinsh@google.com>
 */
#ifndef _MISC_CGROUP_H_
#define _MISC_CGROUP_H_

/**
 * enum misc_res_type - Types of misc cgroup entries supported by the host.
 */
enum misc_res_type {
#ifdef CONFIG_KVM_AMD_SEV
	/** @MISC_CG_RES_SEV: AMD SEV ASIDs resource */
	MISC_CG_RES_SEV,
	/** @MISC_CG_RES_SEV_ES: AMD SEV-ES ASIDs resource */
	MISC_CG_RES_SEV_ES,
#endif
#ifdef CONFIG_INTEL_TDX_HOST
	/** @MISC_CG_RES_TDX: Intel TDX HKIDs resource */
	MISC_CG_RES_TDX,
#endif
#ifdef CONFIG_PPC_VAS
	/** @MISC_CG_RES_VAS_WIN: POWER VAS user window resource */
	MISC_CG_RES_VAS_WIN,
	/** @MISC_CG_RES_VAS_WIN_QOS: POWER VAS quality-of-service window resource */
	MISC_CG_RES_VAS_WIN_QOS,
#endif
	/** @MISC_CG_RES_TYPES: count of enum misc_res_type constants */
	MISC_CG_RES_TYPES
};

struct misc_cg;

#ifdef CONFIG_CGROUP_MISC

#include <linux/cgroup.h>

/**
 * struct misc_res: Per cgroup per misc type resource
 * @max: Maximum limit on the resource.
 * @watermark: Historical maximum usage of the resource.
 * @usage: Current usage of the resource.
 * @events: Number of times, the resource limit exceeded.
 */
struct misc_res {
	u64 max;
	atomic64_t watermark;
	atomic64_t usage;
	atomic64_t events;
	atomic64_t events_local;
};

/**
 * struct misc_cg - Miscellaneous controller's cgroup structure.
 * @css: cgroup subsys state object.
 * @events_file: Handle for the misc resources events file.
 * @res: Array of misc resources usage in the cgroup.
 */
struct misc_cg {
	struct cgroup_subsys_state css;

	/* misc.events */
	struct cgroup_file events_file;
	/* misc.events.local */
	struct cgroup_file events_local_file;

	struct misc_res res[MISC_CG_RES_TYPES];
};

int misc_cg_set_capacity(enum misc_res_type type, u64 capacity);
int misc_cg_try_charge(enum misc_res_type type, struct misc_cg *cg, u64 amount);
void misc_cg_uncharge(enum misc_res_type type, struct misc_cg *cg, u64 amount);

/**
 * css_misc() - Get misc cgroup from the css.
 * @css: cgroup subsys state object.
 *
 * Context: Any context.
 * Return:
 * * %NULL - If @css is null.
 * * struct misc_cg* - misc cgroup pointer of the passed css.
 */
static inline struct misc_cg *css_misc(struct cgroup_subsys_state *css)
{
	return css ? container_of(css, struct misc_cg, css) : NULL;
}

/*
 * get_current_misc_cg() - Find and get the misc cgroup of the current task.
 *
 * Returned cgroup has its ref count increased by 1. Caller must call
 * put_misc_cg() to return the reference.
 *
 * Return: Misc cgroup to which the current task belongs to.
 */
static inline struct misc_cg *get_current_misc_cg(void)
{
	return css_misc(task_get_css(current, misc_cgrp_id));
}

/*
 * put_misc_cg() - Put the misc cgroup and reduce its ref count.
 * @cg - cgroup to put.
 */
static inline void put_misc_cg(struct misc_cg *cg)
{
	if (cg)
		css_put(&cg->css);
}

/**
 * misc_cg_charge_current() - charge the current task's cgroup for a resource
 * @type: the resource being charged
 * @cgp: where to record the cgroup charged
 * @amount: how much to charge
 *
 * Charges @amount and, on success, stores the cgroup it was charged to in
 * @cgp together with the reference that keeps it alive. That cgroup, not
 * whichever one the releasing task happens to be in, is what
 * misc_cg_uncharge_put() must be given: a charge stays where it was made
 * until the resource is freed, and the object outliving or moving between
 * cgroups is the normal case rather than the exception.
 *
 * Return: 0 on success, negative errno on failure, with @cgp set to NULL.
 */
static inline int misc_cg_charge_current(enum misc_res_type type,
					 struct misc_cg **cgp, u64 amount)
{
	struct misc_cg *cg = get_current_misc_cg();
	int ret = misc_cg_try_charge(type, cg, amount);

	if (ret) {
		put_misc_cg(cg);
		*cgp = NULL;
		return ret;
	}
	*cgp = cg;
	return 0;
}

/**
 * misc_cg_uncharge_put() - return a charge made by misc_cg_charge_current()
 * @type: the resource being returned
 * @cgp: the cgroup recorded by the charge, cleared here
 * @amount: how much to return, matching the charge
 *
 * Does nothing when @cgp is already NULL, so a second call is harmless and
 * an object can be torn down by whichever path gets there first without
 * each of them having to know whether the charge was ever made.
 */
static inline void misc_cg_uncharge_put(enum misc_res_type type,
					struct misc_cg **cgp, u64 amount)
{
	if (!*cgp)
		return;
	misc_cg_uncharge(type, *cgp, amount);
	put_misc_cg(*cgp);
	*cgp = NULL;
}

#else /* !CONFIG_CGROUP_MISC */

static inline int misc_cg_set_capacity(enum misc_res_type type, u64 capacity)
{
	return 0;
}

static inline int misc_cg_try_charge(enum misc_res_type type,
				     struct misc_cg *cg,
				     u64 amount)
{
	return 0;
}

static inline void misc_cg_uncharge(enum misc_res_type type,
				    struct misc_cg *cg,
				    u64 amount)
{
}

static inline struct misc_cg *get_current_misc_cg(void)
{
	return NULL;
}

static inline void put_misc_cg(struct misc_cg *cg)
{
}

static inline int misc_cg_charge_current(enum misc_res_type type,
					 struct misc_cg **cgp, u64 amount)
{
	*cgp = NULL;
	return 0;
}

static inline void misc_cg_uncharge_put(enum misc_res_type type,
					struct misc_cg **cgp, u64 amount)
{
}

#endif /* CONFIG_CGROUP_MISC */
#endif /* _MISC_CGROUP_H_ */
