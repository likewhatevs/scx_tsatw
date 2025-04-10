#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <stdbool.h>
#include <ctype.h>
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
				unsigned long long poll_interval,
				unsigned long long pc_checks)
{
	int key = ftok("mangoapp", 65);
	int msgid = msgget(key, 0 /*get key*/);
	int len = _ma_try_read(buf, msgid);

	unsigned long long start = get_usec_now();
	unsigned long long pc_c = 0;
	do {
		if (len > 0)
			return true;
		if (pc_c >= pc_checks) {
			usleep(poll_interval);
			pc_c = 0;
		}
		len = _ma_try_read(buf, msgid);
		pc_c++;
	} while (!(len > 0) && get_usec_now() - start < poll_try_window);

	if (len > 0)
		return true;

	return false;
}

// prefcore

bool prefcore_ranking(prefcore_state *state)
{
	memset(state->prefcore_ranking, 255, sizeof(state->prefcore_ranking));
	memset(state->cpu_ordering, 255, sizeof(state->cpu_ordering));

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

	// Create a temporary array to store CPU IDs and their rankings
	struct {
		unsigned short cpu_id;
		unsigned char ranking;
	} cpu_rankings[MAX_CPUS];

	// Initialize the array with CPU IDs and their rankings
	int valid_cpus = 0;
	for (int i = 0; i < MAX_CPUS; i++) {
		if (state->prefcore_ranking[i] != 255) {
			cpu_rankings[valid_cpus].cpu_id = i;
			cpu_rankings[valid_cpus].ranking =
				state->prefcore_ranking[i];
			valid_cpus++;
		}
	}

	// Sort the array by ranking (descending), then by CPU ID (ascending)
	for (int i = 0; i < valid_cpus - 1; i++) {
		for (int j = 0; j < valid_cpus - i - 1; j++) {
			// First sort by ranking (descending)
			if (cpu_rankings[j].ranking <
			    cpu_rankings[j + 1].ranking) {
				// Swap
				unsigned short temp_id = cpu_rankings[j].cpu_id;
				unsigned char temp_rank =
					cpu_rankings[j].ranking;
				cpu_rankings[j].cpu_id =
					cpu_rankings[j + 1].cpu_id;
				cpu_rankings[j].ranking =
					cpu_rankings[j + 1].ranking;
				cpu_rankings[j + 1].cpu_id = temp_id;
				cpu_rankings[j + 1].ranking = temp_rank;
			}
			// If rankings are equal, sort by CPU ID (ascending)
			else if (cpu_rankings[j].ranking ==
					 cpu_rankings[j + 1].ranking &&
				 cpu_rankings[j].cpu_id >
					 cpu_rankings[j + 1].cpu_id) {
				// Swap
				unsigned short temp_id = cpu_rankings[j].cpu_id;
				unsigned char temp_rank =
					cpu_rankings[j].ranking;
				cpu_rankings[j].cpu_id =
					cpu_rankings[j + 1].cpu_id;
				cpu_rankings[j].ranking =
					cpu_rankings[j + 1].ranking;
				cpu_rankings[j + 1].cpu_id = temp_id;
				cpu_rankings[j + 1].ranking = temp_rank;
			}
		}
	}

	// Populate the cpu_ordering array with the sorted CPU IDs
	for (int i = 0; i < valid_cpus; i++) {
		state->cpu_ordering[i] = cpu_rankings[i].cpu_id;
	}

	return true;
}

void _pc_print_prefcore_state(const prefcore_state *state)
{
	printf("prefcore_state contents:\n");
	printf("filepath: %s\n", state->filepath);
	printf("buffer: %s\n", state->buffer);

	printf("prefcore_ranking:\n");
	for (unsigned short i = 0; i < MAX_CPUS; i++) {
		if (state->prefcore_ranking[i] == 255)
			break;
		printf("CPU %d: Rank %d\n", i, state->prefcore_ranking[i]);
	}
	printf("cpu_ordering:\n");
	for (unsigned short i = 0; i < MAX_CPUS; i++) {
		if (state->cpu_ordering[i] == 255)
			break;
		printf("Core Order %d\n", state->cpu_ordering[i]);
	}
}

// pidgraph

// Structure to hold child PIDs for each parent PID
typedef struct {
	unsigned short
		children[MAX_CHILDREN_PER_PID]; // Fixed-size array of child PIDs
	unsigned short count; // Number of children
} pid_children_t;

// Global array to store children for each PID
static pid_children_t pid_children[MAX_PIDS];

// Initialize the pidgraph data structure
void _pg_init_pidgraph(void)
{
	memset(pid_children, 0, sizeof(pid_children));
}

// Add a child PID to a parent PID
void _pg_add_child(unsigned short ppid, unsigned short child)
{
	// Check if we have room for another child
	if (ppid < MAX_PIDS && child < MAX_PIDS &&
	    pid_children[ppid].count < MAX_CHILDREN_PER_PID) {
		pid_children[ppid].children[pid_children[ppid].count++] = child;
	}
}

// Check if a string is numeric
int _pg_is_numeric(const char *str)
{
	for (; *str; ++str)
		if (!isdigit(*str))
			return 0;
	return 1;
}

// Build the process hierarchy
bool get_pidgraph(void)
{
	// Initialize the data structure
	_pg_init_pidgraph();

	DIR *proc = opendir(PROC_DIR);
	if (!proc) {
		perror("opendir");
		return false;
	}

	struct dirent *entry;
	char path[256], line[256];
	FILE *file;

	// Scan all processes in /proc
	while ((entry = readdir(proc))) {
		if (!_pg_is_numeric(entry->d_name))
			continue;

		unsigned short pid = atoi(entry->d_name);
		if (pid >= MAX_PIDS)
			continue;

		// Read the process status file
		snprintf(path, sizeof(path), PROC_DIR "/%d/" STATUS_FILE, pid);
		file = fopen(path, "r");
		if (!file)
			continue;

		// Find the parent PID
		unsigned short ppid = 0;
		while (fgets(line, sizeof(line), file)) {
			if (!strncmp(line, "PPid:", 5)) {
				sscanf(line + 5, "%hd", &ppid);
				break;
			}
		}
		fclose(file);

		// Add the parent-child relationship
		if (ppid > 0 && ppid < MAX_PIDS) {
			_pg_add_child(ppid, pid);
		}
	}

	closedir(proc);
	return true;
}

// Get all descendants of a PID
void _pg_get_descendants(unsigned short parent, unsigned short *pid_arr,
			 unsigned short *index)
{
	// Check if the parent PID is valid
	if (parent >= MAX_PIDS)
		return;

	// Add all direct children to the result array
	for (unsigned short i = 0; i < pid_children[parent].count; i++) {
		unsigned short child = pid_children[parent].children[i];
		pid_arr[(*index)++] = child;

		// Recursively get descendants of this child
		_pg_get_descendants(child, pid_arr, index);
	}
}

// Print all descendants of a PID
void _pg_print_children(unsigned short parent)
{
	unsigned short pid_arr[MAX_PIDS] = { 0 };
	unsigned short index = 0;

	_pg_get_descendants(parent, pid_arr, &index);

	if (index > 0) {
		for (int i = 0; i < index; i++) {
			printf("%d\n", pid_arr[i]);
		}
	} else {
		printf("No children found for PID %d\n", parent);
	}
}

// Reset the pidgraph data structure
void _pg_reset_pidgraph(void)
{
	_pg_init_pidgraph();
}
