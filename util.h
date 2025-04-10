#pragma once

//mangoapp stuff
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <inttypes.h>

// https://github.com/ValveSoftware/gamescope/blob/master/src/mangoapp.cpp
typedef struct mangoapp_msg_header {
	long msg_type;
	uint32_t version;
} __attribute__((packed)) mangoapp_msg_header;

typedef struct mangoapp_msg_v1 {
	struct mangoapp_msg_header hdr;

	uint32_t pid;
	uint64_t app_frametime_ns;
	uint8_t fsrUpscale;
	uint8_t fsrSharpness;
	uint64_t visible_frametime_ns;
	uint64_t latency_ns;
	uint32_t outputWidth;
	uint32_t outputHeight;
	uint16_t displayRefresh;
	bool bAppWantsHDR : 1;
	bool bSteamFocused : 1;
	char engineName[40];

} __attribute__((packed)) mangoapp_msg_v1;

// Pretty print function
void _ma_pretty_print_msg(mangoapp_msg_v1 *buf);

bool update_framedata_poll_usec(mangoapp_msg_v1 *buf,
				unsigned long long poll_try_window,
				unsigned long long poll_interval);

unsigned long long get_usec_now(void);

#include <dirent.h>
#include <stdbool.h>

// prefcore

#define POLICY_PATH "/sys/devices/system/cpu/cpufreq/"
// rly 32 till gaming threadripper handhelds lol.
#define MAX_CPUS 33

typedef struct prefcore_state {
	DIR *dir;
	struct dirent *entry;
	char filepath[512];
	char buffer[4096];
	int prefcore_ranking[MAX_CPUS];
} prefcore_state;

bool prefcore_ranking(prefcore_state *state);

void _pc_print_prefcore_state(const prefcore_state *state);

// pidgraph

#include <dirent.h>
#include <unistd.h>

#define PROC_DIR "/proc"
#define STATUS_FILE "status"
// ❯ cat /proc/sys/kernel/pid_max
// 4194304
// but honesly I'd just kernel.pid_max in grub.
// looking at a few boxes might cut this to 4k.
#define MAX_PIDS 8192
#define MAX_CHILDREN_PER_PID 32

// Function declarations for pidgraph
int _pg_is_numeric(const char *str);

// Initialize the pidgraph data structure
void _pg_init_pidgraph(void);

// Add a child PID to a parent PID
void _pg_add_child(pid_t ppid, pid_t child);

// Build the process hierarchy
bool get_pidgraph(void);

// Print all descendants of a PID
void _pg_print_children(pid_t parent);

// Reset the pidgraph data structure
void _pg_reset_pidgraph(void);

// Get all descendants of a PID
void _pg_get_descendants(pid_t parent, pid_t *pid_arr, int *index);
