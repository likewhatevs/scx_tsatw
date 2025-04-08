#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdbool.h>
#include "util.h"
//mangoapp stuff
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/time.h>

unsigned long long get_usec_now(void)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return ((unsigned long long)tv.tv_sec * 1000000ULL) +
	       (unsigned long long)tv.tv_usec;
}

// Pretty print function
void _ma_pretty_print_msg(mangoapp_msg_v1 *buf)
{
	printf("=== mangoapp_msg_v1 ===\n");
	printf("Header:\n");
	printf("  msg_type: %ld\n", buf->hdr.msg_type);
	printf("  version: %u\n", buf->hdr.version);
	printf("Payload:\n");
	printf("  pid: %u\n", buf->pid);
	printf("  app_frametime_ns: %" PRIu64 "\n", buf->app_frametime_ns);
	printf("  fsrUpscale: %u\n", buf->fsrUpscale);
	printf("  fsrSharpness: %u\n", buf->fsrSharpness);
	printf("  visible_frametime_ns: %" PRIu64 "\n",
	       buf->visible_frametime_ns);
	printf("  latency_ns: %" PRIu64 "\n", buf->latency_ns);
	printf("  outputWidth: %u\n", buf->outputWidth);
	printf("  outputHeight: %u\n", buf->outputHeight);
	printf("  displayRefresh: %u\n", buf->displayRefresh);
	printf("  bAppWantsHDR: %s\n", buf->bAppWantsHDR ? "true" : "false");
	printf("  bSteamFocused: %s\n", buf->bSteamFocused ? "true" : "false");
	printf("  engineName: %.*s\n", (int)sizeof(buf->engineName),
	       buf->engineName);
}

int _ma_try_read(mangoapp_msg_v1 *buf, int msgid)
{
	return msgrcv(msgid, buf,
		      sizeof(mangoapp_msg_v1) -
			      sizeof(long) /*msg_type type on header*/,
		      0 /* head of list */, MSG_COPY | IPC_NOWAIT);
}

bool update_framedata_poll_usec(mangoapp_msg_v1 *buf,
				unsigned long long poll_try_window,
				unsigned long long poll_interval)
{
	int key = ftok("mangoapp", 65);
	int msgid = msgget(key, 0 /*get key*/);
	int len = _ma_try_read(buf, msgid);

	if (poll_try_window == 0) {
		do {
			if (len > 0)
				return true;
			usleep(poll_interval);
			len = _ma_try_read(buf, msgid);
		} while (!(len > 0));

		if (len > 0)
			return true;

		return false;
	}

	unsigned long long start = get_usec_now();

	do {
		if (len > 0)
			return true;
		usleep(poll_interval);
		len = _ma_try_read(buf, msgid);
	} while (!(len > 0) && get_usec_now() - start < poll_try_window);

	if (len > 0)
		return true;

	return false;
}

// prefcore

bool prefcore_ranking(prefcore_state *state)
{
	memset(state->prefcore_ranking, -1, sizeof(state->prefcore_ranking));

	state->dir = opendir(POLICY_PATH);
	if (!state->dir) {
		perror("Error opening policy directory");
		return false;
	}

	while ((state->entry = readdir(state->dir)) != NULL) {
		if (strncmp(state->entry->d_name, "policy", 6) == 0) {
			int policy_cpu = atoi(state->entry->d_name + 6);
			snprintf(state->filepath, sizeof(state->filepath),
				 "%s%s/amd_pstate_prefcore_ranking",
				 POLICY_PATH, state->entry->d_name);
			FILE *fp = fopen(state->filepath, "r");
			if (fp) {
				if (fgets(state->buffer, sizeof(state->buffer),
					  fp)) {
					int ranking = atoi(state->buffer);
					if (policy_cpu >= 0 &&
					    policy_cpu < MAX_CPUS) {
						state->prefcore_ranking
							[policy_cpu] = ranking;
					}
				}
				fclose(fp);
			}
		}
	}
	closedir(state->dir);
	return true;
}

void _pc_print_prefcore_state(const prefcore_state *state)
{
	printf("prefcore_state contents:\n");
	printf("filepath: %s\n", state->filepath);
	printf("buffer: %s\n", state->buffer);

	printf("prefcore_ranking:\n");
	for (int i = 0; i < MAX_CPUS; i++) {
		if (state->prefcore_ranking[i] == -1)
			return;
		printf("CPU %d: Rank %d\n", i, state->prefcore_ranking[i]);
	}
}

// pidgraph

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int _pg_is_numeric(const char *str)
{
	for (; *str; ++str)
		if (!isdigit(*str))
			return 0;
	return 1;
}

void _pg_add_child(pid_t ppid, pid_t child, pid_node **children)
{
	pid_node *node = malloc(sizeof(pid_node));
	node->pid = child;
	node->next = children[ppid];
	children[ppid] = node;
}

bool get_pidgraph(pid_node **children)
{
	DIR *proc = opendir(PROC_DIR);
	if (!proc) {
		perror("opendir");
		return false;
	}

	struct dirent *entry;
	char path[256], line[256];
	FILE *file;
	while ((entry = readdir(proc))) {
		if (!_pg_is_numeric(entry->d_name))
			continue;

		pid_t pid = atoi(entry->d_name);

		snprintf(path, sizeof(path), PROC_DIR "/%d/" STATUS_FILE, pid);
		file = fopen(path, "r");
		if (!file)
			continue;

		pid_t ppid = -1;
		while (fgets(line, sizeof(line), file)) {
			if (!strncmp(line, "PPid:", 5)) {
				sscanf(line + 5, "%d", &ppid);
				break;
			}
		}
		fclose(file);

		if (ppid >= 0 && ppid < MAX_PIDS)
			_pg_add_child(ppid, pid, children);
	}
	closedir(proc);
	return true;
}

bool _pg_pid_arr(pid_t parent, pid_node **children, int *pid_arr, int *index)
{
	pid_node *n = children[parent];
	if (!n)
		return false;

	while (n) {
		pid_arr[(*index)++] = n->pid;
		// Recursive call to add grandchildren (and deeper levels)
		_pg_pid_arr(n->pid, children, pid_arr, index);
		n = n->next;
	}
	return true;
}

void _pg_print_children(pid_t parent, pid_node **children)
{
	int pid_arr[MAX_PIDS] = { 0 };
	int index = 0;

	if (_pg_pid_arr(parent, children, pid_arr, &index)) {
		for (int i = 0; i < MAX_PIDS; i++) {
			if (pid_arr[i] == 0)
				break;
			printf("%d\n", pid_arr[i]);
		}
	} else {
		printf("failed to obtain pid children");
	}
}

void _pg_reset_free(pid_node **children)
{
	// this is kinda garbage
	// fix it later.
	for (int i = 0; i < MAX_PIDS; i++) {
		pid_node *node = children[i];
		while (node) {
			pid_node *tmp = node;
			node = node->next;
			free(tmp);
		}
	}
	memset(children, 0, MAX_PIDS * sizeof(pid_node *));
}
