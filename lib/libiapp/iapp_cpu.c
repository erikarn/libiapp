#include <stdio.h>
#include <err.h>

#include <sys/types.h>
#include <sys/sysctl.h>

#include "iapp_cpu.h"

int
iapp_get_ncpus(void)
{
	int r;
	int v;
	size_t l;

	l = sizeof(v);
	r = sysctlbyname("hw.ncpu", &v, &l, NULL, 0);

	if (r < 0) {
		warn("%s: sysctlbyname (hw.ncpu)", __func__);
		return (-1);
	}

	return v;
}
