#include "ubench.h"
#include "util.h"

UBENCH_EX(pidgraph, get_root_pid_vec)
{
	unsigned short pid_arr[MAX_PIDS] = { 0 };

	UBENCH_DO_BENCHMARK()
	{
		unsigned short idx = 0;
		get_pidgraph();
		_pg_get_descendants(1, pid_arr, &idx);
		_pg_reset_pidgraph();
	}
}

UBENCH_EX(prefcore, get_a_cpu_ordering)
{
	prefcore_state ps;
	UBENCH_DO_BENCHMARK()
	{
		prefcore_ranking(&ps);
	}
}

UBENCH_EX(mangoapp, try_get_game_metadata)
{
	mangoapp_msg_v1 buf = { 0 };
	memset(&buf, 0, sizeof(buf));
	unsigned long long poll_interval = 1; /* wait a us between checks*/
	unsigned long long poll_window = 0; /* ignore poll window */
	UBENCH_DO_BENCHMARK()
	{
		update_framedata_poll_usec(&buf, poll_window, poll_interval);
	}
}

UBENCH_EX(mangoapp, get_game_metadata)
{
	mangoapp_msg_v1 buf = { 0 };
	memset(&buf, 0, sizeof(buf));
	unsigned long long poll_interval = 1; /* wait a us between checks*/
	unsigned long long poll_window = 0; /* ignore poll window */
	int len = 0;
	UBENCH_DO_BENCHMARK()
	{
		do {
			len = update_framedata_poll_usec(&buf, poll_window,
							 poll_interval);
		} while (!(len > 0));
	}
}

UBENCH_MAIN()
