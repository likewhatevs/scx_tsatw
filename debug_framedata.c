#include "util.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main()
{
	mangoapp_msg_v1 buf = { 0 };
	memset(&buf, 0, sizeof(buf));
	unsigned long long poll_interval = 1; /* wait a us between checks*/
	unsigned long long poll_window = 0; /* ignore poll window */
	unsigned long long pause_time = 500000; /* check every .5 seconds */
	while (true) {
		printf("start\n");
		if (update_framedata_poll_usec(&buf, poll_window,
					       poll_interval))
			_ma_pretty_print_msg(&buf);
		printf("dostuff\n");
		usleep(pause_time);
	}
}
