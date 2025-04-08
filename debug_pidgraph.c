#include "util.h"
#include <stdlib.h>
#include <stdio.h>

int main()
{
	pid_node **pidgraph = calloc(MAX_PIDS, sizeof(pid_node *));
	if (get_pidgraph(pidgraph)) {
		_pg_print_children(1, pidgraph);
	} else {
		printf("error getting pidgraph");
	}
}
