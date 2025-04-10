#include "util.h"
#include <stdio.h>

int main()
{
	prefcore_state ps;
	prefcore_ranking(&ps);
	_pc_print_prefcore_state(&ps);

	return 0;
}
