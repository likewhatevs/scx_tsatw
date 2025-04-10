/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates.
 * Copyright (c) 2022 Tejun Heo <tj@kernel.org>
 * Copyright (c) 2022 David Vernet <dvernet@meta.com>
 */

#include "util.h"
#undef LSP

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <libgen.h>
#include <bpf/bpf.h>
#include <scx/common.h>
#include "scx_tsatw.bpf.skel.h"
#include <scx/user_exit_info.h>

#define FT_PTR_MAX 8

const char help_fmt[] =
	"A simple sched_ext scheduler.\n"
	"\n"
	"See the top-level comment in .bpf.c for more details.\n"
	"\n"
	"Usage: %s [-f] [-v]\n"
	"\n"
	"  -f            Use FIFO scheduling instead of weighted vtime scheduling\n"
	"  -v            Print libbpf debug messages\n"
	"  -h            Display this help and exit\n";

static bool verbose;
static volatile int exit_req;

static int libbpf_print_fn(enum libbpf_print_level level, const char *format,
			   va_list args)
{
	if (level == LIBBPF_DEBUG && !verbose)
		return 0;
	return vfprintf(stderr, format, args);
}

static void sigint_handler(int simple)
{
	exit_req = 1;
}

static void read_stats(struct scx_tsatw_bpf *skel, __u64 *stats)
{
	int nr_cpus = libbpf_num_possible_cpus();
	__u64 cnts[2][nr_cpus];
	__u32 idx;

	memset(stats, 0, sizeof(stats[0]) * 2);

	for (idx = 0; idx < 2; idx++) {
		int ret, cpu;

		ret = bpf_map_lookup_elem(bpf_map__fd(skel->maps.stats), &idx,
					  cnts[idx]);
		if (ret < 0)
			continue;
		for (cpu = 0; cpu < nr_cpus; cpu++)
			stats[idx] += cnts[idx][cpu];
	}
}

// this is kinda shitty,
// but simple for now.
// aka I don't wanna ssh to restart sched
// every time i change games.

static bool key_in_array(int key, unsigned short *arr) {
    for (size_t i = 0; i < MAX_PIDS; i++)
        if (arr[i] == key)
            return true;
    return false;
}

int clear_stuff_except(struct scx_tsatw_bpf *skel, unsigned short *preserve_keys) {
    int key, next_key, ret;

    ret = bpf_map_get_next_key(bpf_map__fd(skel->maps.care_pids), NULL, &key);
    while (ret == 0) {
        ret = bpf_map_get_next_key(bpf_map__fd(skel->maps.care_pids), &key, &next_key);

        if (!key_in_array(key, preserve_keys)) {
            if (bpf_map_delete_elem(bpf_map__fd(skel->maps.care_pids), &key) != 0) {
                printf("bpf_map_delete_elem error");
            }
        }

        if (ret == 0)
            key = next_key;
    }

    return 0;
}

int main(int argc, char **argv)
{
	struct scx_tsatw_bpf *skel;
	struct bpf_link *link;
	__u32 opt;
	__u64 ecode;

	libbpf_set_print(libbpf_print_fn);
	signal(SIGINT, sigint_handler);
	signal(SIGTERM, sigint_handler);
restart:
	skel = SCX_OPS_OPEN(tsatw_ops, scx_tsatw_bpf);

	while ((opt = getopt(argc, argv, "fvh")) != -1) {
		switch (opt) {
		case 'f':
			skel->rodata->fifo_sched = true;
			break;
		case 'v':
			verbose = true;
			break;
		default:
			fprintf(stderr, help_fmt, basename(argv[0]));
			return opt != 'h';
		}
	}

	prefcore_state state;
	prefcore_ranking(&state);
	unsigned short care_cpus[8];
	unsigned short no_care_cpus[16];
	// clean up later
	memcpy(care_cpus, state.cpu_ordering, sizeof(care_cpus));
	memcpy(no_care_cpus, state.cpu_ordering + 8, sizeof(no_care_cpus));
	// this is kinda ick in that it means restart if prefcore ranking changes.
	memcpy(skel->rodata->care_cpus, care_cpus, sizeof(care_cpus));
	memcpy(skel->rodata->no_care_cpus, no_care_cpus, sizeof(no_care_cpus));

	SCX_OPS_LOAD(skel, tsatw_ops, scx_tsatw_bpf, uei);
	link = SCX_OPS_ATTACH(skel, tsatw_ops, scx_tsatw_bpf);
	mangoapp_msg_v1 buf = { 0 };
	memset(&buf, 0, sizeof(buf));

	unsigned long long poll_interval = 1; /* wait a us between checks*/
	unsigned long long poll_window = 500000; /* .5 seconds poll window */
	unsigned long long pause_time = 250000; /* check every .5 seconds */
	unsigned long long pc_checks = 5; /* check x iterations before sleeping */

	int cpu_count = libbpf_num_possible_cpus();
	u64 *frametimes = calloc(cpu_count, sizeof(u64));
	u64 *care_pid_zeros = calloc(cpu_count, sizeof(u64));
	unsigned short care_pid_arr[MAX_PIDS] = { 0 };
	u32 z_ptr = 0;

	unsigned long long ft_vals[FT_PTR_MAX] = { 0 };

	unsigned char ft_ptr = 0;

	while (!exit_req && !UEI_EXITED(skel, uei)) {
		__u64 stats[2];

		read_stats(skel, stats);
		printf("local=%llu global=%llu\n", stats[0], stats[1]);
		fflush(stdout);
		if (update_framedata_poll_usec(&buf, poll_window,
					       poll_interval, pc_checks)) {
			ft_vals[ft_ptr] = buf.app_frametime_ns;
			ft_ptr++;
			if (ft_ptr >= FT_PTR_MAX) {
				ft_ptr = 0;
			}
			if (ft_vals[FT_PTR_MAX - 1] != 0) {
				unsigned long long ft_avg = 0;
				for(int i = 0; i < FT_PTR_MAX; i++) {
					ft_avg += ft_vals[i];
				}
				ft_avg /= FT_PTR_MAX;
				for(int i = 0; i < cpu_count; i++) {
					frametimes[i] = ft_avg;
				}
				int ret = bpf_map_update_elem(bpf_map__fd(skel->maps.frametime),
					&z_ptr, frametimes, BPF_ANY);
				if (ret < 0) {
					printf("error updating frametime: %d\n", ret);
				}
			}
			unsigned short idx = 0;
			get_pidgraph();
			_pg_get_descendants(buf.pid, care_pid_arr, &idx);
			for(int i = 0; i < MAX_PIDS; i++) {
				if (care_pid_arr[i] == 0) {
					break;
				}
				int key = care_pid_arr[i];
				int ret = bpf_map_update_elem(bpf_map__fd(skel->maps.care_pids),
				&key, care_pid_zeros, BPF_ANY);
				if (ret < 0) {
					printf("error updating care_pids: %d\n", ret);
				}
			}
			// so terrible.
			// clear_stuff_except(skel, care_pid_arr);
		};
		usleep(pause_time);
	}

	bpf_link__destroy(link);
	ecode = UEI_REPORT(skel, uei);
	scx_tsatw_bpf__destroy(skel);

	if (UEI_ECODE_RESTART(ecode))
		goto restart;
	return 0;
}
