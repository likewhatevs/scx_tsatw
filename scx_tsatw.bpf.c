/* SPDX-License-Identifier: GPL-2.0 */
/*
 * dum dum scheduler for my gaming handheld.
 * fork of scx simple
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */

#ifdef LSP
#undef LSP
#define __bpf__
#endif

#include <scx/common.bpf.h>
#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

char _license[] SEC("license") = "GPL";

const volatile bool fifo_sched;
const volatile unsigned short care_cpus[8];
const volatile unsigned short no_care_cpus[16];
private(care_cpumask) struct bpf_cpumask __kptr *care_cpumask;
private(no_care_cpumask) struct bpf_cpumask __kptr *no_care_cpumask;

static u64 vtime_now;
UEI_DEFINE(uei);

/*
 * Built-in DSQs such as SCX_DSQ_GLOBAL cannot be used as priority queues
 * (meaning, cannot be dispatched to with scx_bpf_dsq_insert_vtime()). We
 * therefore create a separate DSQ with ID 0 that we dispatch to and consume
 * from. If scx_tsatw only supported global FIFO scheduling, then we could just
 * use SCX_DSQ_GLOBAL.
 */
#define SHARED_DSQ 0
#define CARE_DSQ 1
#define NO_CARE_DSQ 2

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(u32));
	__uint(value_size, sizeof(u64));
	__uint(max_entries, 2); /* [local, global] */
} stats SEC(".maps");

// write care pids to maps local to all cpus.
// 
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	// key is pid
	__type(key, u32); // kinda sucks btf type errors if u16.
	__type(value, u32);
	// hardcoded value from util.h MAX_PIDS
	__uint(max_entries, 4096);
	__uint(map_flags, 0);
} care_pids SEC(".maps");

// write frametime to maps local to all cpus.
//	uint64_t app_frametime_ns;
struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	// key is 0
	__type(key, u32); // again, kinda sucks u32.
	// value is frametime
	__type(value, u64);
	__uint(max_entries, 1);
	__uint(map_flags, 0);
} frametime SEC(".maps");


static void stat_inc(u32 idx)
{
	u64 *cnt_p = bpf_map_lookup_elem(&stats, &idx);
	if (cnt_p)
		(*cnt_p)++;
}

s32 BPF_STRUCT_OPS(tsatw_select_cpu, struct task_struct *p, s32 prev_cpu,
		   u64 wake_flags)
{
	// look for idle && keep care away from no care.
	// will probably need some sticky later on.
	s32 cpu;
	u32 tgid = p->tgid;

	
	if (bpf_map_lookup_elem(&care_pids, &tgid)) {
		if(care_cpumask)
			cpu = scx_bpf_pick_any_cpu(cast_mask(care_cpumask), 0);
	} else {
		if(no_care_cpumask)
			cpu = scx_bpf_pick_any_cpu(cast_mask(no_care_cpumask), 0);
	}

	return cpu;
}

void BPF_STRUCT_OPS(tsatw_enqueue, struct task_struct *p, u64 enq_flags)
{
	stat_inc(1); /* count global queueing */

	u64* frametime_avg;
	u64 frametime_frac = 0;
	u64 z_ptr = 0;

	frametime_avg = bpf_map_lookup_elem(&frametime, &z_ptr);
	if (!frametime_avg) {
		scx_bpf_error("frametime lookup failed");
		return;
	}

	// need to use systing to get a sense of a decent fraction.
	frametime_frac = (*frametime_avg)/5;

	u64 vtime = p->scx.dsq_vtime;

	/*
	* Limit the amount of budget that an idling task can accumulate
	* to one slice.
	*/
	if (time_before(vtime, vtime_now - frametime_frac))
		vtime = vtime_now - frametime_frac;

	u32 tgid = p->tgid;

	if (bpf_map_lookup_elem(&care_pids, &tgid)) {
		scx_bpf_dsq_insert_vtime(p, CARE_DSQ, frametime_frac, vtime,
						enq_flags);
	} else {
		scx_bpf_dsq_insert_vtime(p, NO_CARE_DSQ, frametime_frac, vtime,
						enq_flags);
	}

}

void BPF_STRUCT_OPS(tsatw_dispatch, s32 cpu, struct task_struct *prev)
{
	// should be idk not this lol.
	for(int i = 0; i < 16; i++) {
		if (i < 8){
			if (cpu == care_cpus[i]) {
				scx_bpf_dsq_move_to_local(CARE_DSQ);
			}
		} 
		if (cpu == no_care_cpus[i]) {
			scx_bpf_dsq_move_to_local(NO_CARE_DSQ);
		}		
	}
}

void BPF_STRUCT_OPS(tsatw_running, struct task_struct *p)
{
	if (fifo_sched)
		return;
	/*
	 * Global vtime always progresses forward as tasks start executing. The
	 * test and update can be performed concurrently from multiple CPUs and
	 * thus racy. Any error should be contained and temporary. Let's just
	 * live with it.
	 */
	if (time_before(vtime_now, p->scx.dsq_vtime))
		vtime_now = p->scx.dsq_vtime;
}

void BPF_STRUCT_OPS(tsatw_stopping, struct task_struct *p, bool runnable)
{
	if (fifo_sched)
		return;

	/*
	 * Scale the execution time by the inverse of the weight and charge.
	 *
	 * Note that the default yield implementation yields by setting
	 * @p->scx.slice to zero and the following would treat the yielding task
	 * as if it has consumed all its slice. If this penalizes yielding tasks
	 * too much, determine the execution time by taking explicit timestamps
	 * instead of depending on @p->scx.slice.
	 */

	u64* frametime_avg;
	u64 frametime_frac = 0;
	u64 z_ptr = 0;

	frametime_avg = bpf_map_lookup_elem(&frametime, &z_ptr);
	if (!frametime_avg) {
		scx_bpf_error("frametime lookup failed");
		return;
	}


	p->scx.dsq_vtime +=
		(frametime_frac - p->scx.slice) * 100 / p->scx.weight;
}

void BPF_STRUCT_OPS(tsatw_enable, struct task_struct *p)
{
	p->scx.dsq_vtime = vtime_now;
}

// first 8 cpus are care, last 16 are no care.
// ordering is specified by prefcore_ranking.
// in theory best for fps, not bad for energy
// because energy would swap order
// (need to check this is the case).
//
s32 BPF_STRUCT_OPS_SLEEPABLE(tsatw_init)
{
	struct bpf_cpumask *tmp_no_care_cpumask, *tmp_care_cpumask;
	s32 i;

	tmp_care_cpumask = bpf_cpumask_create();
	if (!tmp_care_cpumask){
		return -ENOMEM;
	}
	tmp_no_care_cpumask = bpf_cpumask_create();
	if (!tmp_no_care_cpumask) {
		bpf_cpumask_release(tmp_care_cpumask);
		return -ENOMEM;
	}
	// well need to figure out why this doesn't work sometime lol.
	// bpf_for(i, 0, 16) {
	// 	if (i < 8)
	// 		bpf_cpumask_set_cpu(care_cpus[i], tmp_care_cpumask);
	// 	bpf_cpumask_set_cpu(no_care_cpus[i], tmp_no_care_cpumask);
	// }

	// won't work with prefcore/power modes rly.
	bpf_for(i, 0, 4){
		bpf_cpumask_set_cpu(i, tmp_care_cpumask);
		bpf_cpumask_set_cpu(i+12, tmp_care_cpumask);
	}
	bpf_for(i, 4, 12){
		bpf_cpumask_set_cpu(i, tmp_no_care_cpumask);
		bpf_cpumask_set_cpu(i+12, tmp_no_care_cpumask);
	}

	tmp_care_cpumask = bpf_kptr_xchg(&care_cpumask, tmp_care_cpumask);
	if (tmp_care_cpumask){
		bpf_cpumask_release(tmp_care_cpumask);
	}


	tmp_no_care_cpumask = bpf_kptr_xchg(&no_care_cpumask, tmp_no_care_cpumask);
	if (tmp_no_care_cpumask){
		bpf_cpumask_release(tmp_no_care_cpumask);
	}
	
	scx_bpf_create_dsq(CARE_DSQ, -1);
	scx_bpf_create_dsq(NO_CARE_DSQ, -1);

	return scx_bpf_create_dsq(SHARED_DSQ, -1);
}

void BPF_STRUCT_OPS(tsatw_exit, struct scx_exit_info *ei)
{
	UEI_RECORD(uei, ei);
}

SCX_OPS_DEFINE(tsatw_ops, .select_cpu = (void *)tsatw_select_cpu,
	       .enqueue = (void *)tsatw_enqueue,
	       .dispatch = (void *)tsatw_dispatch,
	       .running = (void *)tsatw_running,
	       .stopping = (void *)tsatw_stopping,
	       .enable = (void *)tsatw_enable, .init = (void *)tsatw_init,
	       .exit = (void *)tsatw_exit, .name = "tsatw");
