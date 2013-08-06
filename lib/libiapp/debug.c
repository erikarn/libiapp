/*-
 * Copyright (c) 2013 Netflix, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of Netflix, Inc. nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>
#include <errno.h>

#include <sys/time.h>

#include <pthread.h>

#include "debug.h"

char * debug_level_strs[DEBUG_SECTION_MAX];
debug_mask_t debug_levels[DEBUG_TYPE_MAX][DEBUG_SECTION_MAX];

static pthread_mutex_t debug_lock;

FILE *debug_file = NULL;
char *debug_filename = NULL;
int debug_syslog_enable = 0;
int debug_syslog_facility = 0;
int debug_syslog_logopt = 0;
char *debug_syslog_ident = NULL;

void
debug_init(const char *progname)
{

	pthread_mutex_init(&debug_lock, NULL);
	bzero(debug_level_strs, sizeof(debug_level_strs));
	bzero(debug_levels, sizeof(debug_levels));

	openlog(progname, LOG_NDELAY | LOG_NOWAIT | LOG_PID, LOG_DAEMON);
}

void
debug_shutdown(void)
{
	int i;

	/* XXX shutdown log files, etc */

	for (i = 0; i < DEBUG_SECTION_MAX; i++) {
		if (debug_level_strs[i] != NULL) {
			free(debug_level_strs[i]);
			debug_level_strs[i] = NULL;
		}
	}
	bzero(debug_levels, sizeof(debug_levels));
	pthread_mutex_destroy(&debug_lock);
}

debug_section_t
debug_register(const char *dbgname)
{
	int i;

	if (dbgname == NULL)
		return (-1);

	for (i = 0; i < DEBUG_SECTION_MAX; i++) {
		if (debug_level_strs[i] == NULL) {
			debug_level_strs[i] = strdup(dbgname);
			debug_levels[DEBUG_TYPE_PRINT][i] = 0x0;
			debug_levels[DEBUG_TYPE_LOG][i] = 0x0;
			debug_levels[DEBUG_TYPE_SYSLOG][i] = 0x0;
			return (i);
		}
	}
	
	/*
	 * XXX should log to say, log type 0 (which should be "debug")
	 */
	fprintf(stderr, "%s: couldn't allocate debug level for '%s'\n",
	    __func__,
	    dbgname);
	return (-1);
}

void
debug_setlevel(debug_section_t s, debug_type_t t, debug_mask_t mask)
{

	if (t >= DEBUG_TYPE_MAX)
		return;

	fprintf(stderr, "%s: setting section (%d) (type %d) = mask %llx\n",
	    __func__, s, t, (long long) mask);
	(void) pthread_mutex_lock(&debug_lock);
	debug_levels[t][s] = mask;
	(void) pthread_mutex_unlock(&debug_lock);
}

void
debug_set_filename(const char *filename)
{

	(void) pthread_mutex_lock(&debug_lock);
	if (debug_filename != NULL)
		free(debug_filename);
	debug_filename = strdup(filename);
	(void) pthread_mutex_unlock(&debug_lock);
}

static void
debug_file_open_locked(void)
{

	if (debug_file != NULL) {
		fclose(debug_file);
		debug_file = NULL;
	}

	if (debug_filename == NULL)
		return;

	debug_file = fopen(debug_filename, "a+");

	if (debug_file == NULL) {
		/* XXX should debuglog this! */
		fprintf(stderr, "%s: fopen failed (%s): %s\n",
		    __func__,
		    debug_filename,
		    strerror(errno));
	}
}

static void
debug_file_close_locked(void)
{

	if (debug_file == NULL) {
		return;
	}
	fflush(debug_file);
	fclose(debug_file);
	debug_file = NULL;
}

void
debug_file_open(void)
{

	(void) pthread_mutex_lock(&debug_lock);
	debug_file_open_locked();
	(void) pthread_mutex_unlock(&debug_lock);
}

void
debug_file_close(void)
{

	(void) pthread_mutex_lock(&debug_lock);
	debug_file_close_locked();
	(void) pthread_mutex_unlock(&debug_lock);
}

void
debug_file_reopen(void)
{

	(void) pthread_mutex_lock(&debug_lock);
	debug_file_close_locked();
	debug_file_open_locked();
	(void) pthread_mutex_unlock(&debug_lock);
}


/*
 * Called to do actual debugging.
 *
 * The macro hilarity is done so that the arguments to the debug
 * statement aren't actually evaluated unless the debugging level
 * is matched.
 */
void
do_debug(int section, debug_mask_t mask, const char *fmt, ...)
{
	va_list ap;
	struct timeval tv;
	char tbuf[128];
	char buf[512];
	int flush_stderr = 0;
	int flush_file = 0;

	(void) pthread_mutex_lock(&debug_lock);
	/* Log wall clock timestamp */
	(void) gettimeofday(&tv, NULL);
	snprintf(tbuf, 128, "%llu.%06llu| ",
	    (unsigned long long) tv.tv_sec,
	    (unsigned long long) tv.tv_usec);

	/* Log the message itself */
	va_start(ap, fmt);
	vsnprintf(buf, 512, fmt, ap);
	va_end(ap);

	/*
	 * Ok, now that it's done, we can figure out where to
	 * write it to.
	 */
	if (debug_levels[DEBUG_TYPE_PRINT][section] & mask) {
		fprintf(stderr, "%s%s", tbuf, buf);
		flush_stderr = 1;
	}
	if (debug_file != NULL &&
	    debug_levels[DEBUG_TYPE_LOG][section] & mask) {
		fprintf(debug_file, "%s%s", tbuf, buf);
		flush_file = 1;
	}
	if (debug_syslog_enable == 1 &&
	    debug_levels[DEBUG_TYPE_SYSLOG][section] & mask) {
		/* XXX should map these levels into syslog levels */
		syslog(LOG_DEBUG, "%s%s", tbuf, buf);
	}
	(void) pthread_mutex_unlock(&debug_lock);

	if (flush_stderr)
		fflush(stderr);
	if (flush_file)
		fflush(debug_file);
}

void
debug_setmask_str(const char *dbg, debug_type_t t, debug_mask_t mask)
{
	int i;
	int len;

	len = strlen(dbg);

	for (i = 0; i < DEBUG_SECTION_MAX; i++) {
		if (debug_level_strs[i] == NULL)
			break;
		if (strlen(debug_level_strs[i]) == len &&
		    strncmp(debug_level_strs[i], dbg, len) == 0)
			break;
	}
	fprintf(stderr, "%s: setting debug '%s' (%d) to %llx\n",
	    __func__, dbg, i, (unsigned long long) mask);
	if (i >= DEBUG_SECTION_MAX)
		return;		/* XXX return something useful? */

	debug_setlevel(i, t, mask);
}

int
debug_setmask_str2(const char *dbg, const char *dtype, debug_mask_t mask)
{
	int i;
	int len;
	int d_i, t_i;

	len = strlen(dbg);

	/* Section lookup */
	for (i = 0; i < DEBUG_SECTION_MAX; i++) {
		if (debug_level_strs[i] == NULL)
			break;
		if (strlen(debug_level_strs[i]) == len &&
		    strncmp(debug_level_strs[i], dbg, len) == 0)
			break;
	}
	if (i >= DEBUG_SECTION_MAX)
		return (-1);
	if (debug_level_strs[i] == NULL)
		return (-1);
	d_i = i;

	/* Type lookup */
	if (strncmp("syslog", dtype, 6) == 0) {
		t_i = DEBUG_TYPE_SYSLOG;
	} else if (strncmp("log", dtype, 4) == 0) {
		t_i = DEBUG_TYPE_LOG;
	} else if (strncmp("print", dtype, 4) == 0) {
		t_i = DEBUG_TYPE_PRINT;
	} else {
		return (-1);
	}

	fprintf(stderr, "%s: setting debug '%s' (%d) to %llx\n",
	    __func__, dbg, i, (unsigned long long) mask);

	debug_setlevel(d_i, t_i, mask);
	return (0);
}
