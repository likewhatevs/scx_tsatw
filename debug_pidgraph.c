#include "util.h"
#include <stdlib.h>
#include <stdio.h>

int main()
{
	if (get_pidgraph()) {
		_pg_print_children(1);
	} else {
		printf("error getting pidgraph");
	}

	_pg_reset_pidgraph();
	return 0;
}
