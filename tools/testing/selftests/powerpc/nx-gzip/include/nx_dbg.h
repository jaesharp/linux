/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright 2020 IBM Corporation
 *
 */

#ifndef _NXU_DBG_H_
#define _NXU_DBG_H_

#include <sys/file.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

extern FILE * nx_gzip_log;
extern int nx_gzip_trace;
extern unsigned int nx_gzip_inflate_impl;
extern unsigned int nx_gzip_deflate_impl;
extern unsigned int nx_gzip_inflate_flags;
extern unsigned int nx_gzip_deflate_flags;

extern int nx_dbg;
pthread_mutex_t mutex_log;

/* Selectable trace classes, held as a bitmask in nx_gzip_trace. */
enum nx_gzip_trace_bit {
	NX_GZIP_TRACE_ANY		= 0x01,
	NX_GZIP_TRACE_HW		= 0x02,
	NX_GZIP_TRACE_SW		= 0x04,
	NX_GZIP_TRACE_STATISTICS	= 0x08,
	NX_GZIP_TRACE_PER_STREAM	= 0x10,
};

static inline int nx_gzip_tracing(enum nx_gzip_trace_bit bits)
{
	return nx_gzip_trace & bits;
}

static inline int nx_gzip_trace_enabled(void)
{
	return nx_gzip_tracing(NX_GZIP_TRACE_ANY);
}

static inline int nx_gzip_hw_trace_enabled(void)
{
	return nx_gzip_tracing(NX_GZIP_TRACE_HW);
}

static inline int nx_gzip_sw_trace_enabled(void)
{
	return nx_gzip_tracing(NX_GZIP_TRACE_SW);
}

static inline int nx_gzip_gather_statistics(void)
{
	return nx_gzip_tracing(NX_GZIP_TRACE_STATISTICS);
}

static inline int nx_gzip_per_stream_stat(void)
{
	return nx_gzip_tracing(NX_GZIP_TRACE_PER_STREAM);
}

#define prt(fmt, ...) do { \
	pthread_mutex_lock(&mutex_log);					\
	flock(fileno(nx_gzip_log), LOCK_EX);				\
	time_t t; struct tm *m; time(&t); m = localtime(&t);		\
	fprintf(nx_gzip_log, "[%04d/%02d/%02d %02d:%02d:%02d] "		\
		"pid %d: " fmt,	\
		(int)m->tm_year + 1900, (int)m->tm_mon+1, (int)m->tm_mday, \
		(int)m->tm_hour, (int)m->tm_min, (int)m->tm_sec,	\
		(int)getpid(), ## __VA_ARGS__);				\
	fflush(nx_gzip_log);						\
	flock(fileno(nx_gzip_log), LOCK_UN);				\
	pthread_mutex_unlock(&mutex_log);				\
} while (0)

/* Use in case of an error */
#define prt_err(fmt, ...) do { if (nx_dbg >= 0) {			\
	prt("%s:%u: Error: "fmt,					\
		__FILE__, __LINE__, ## __VA_ARGS__);			\
}} while (0)

/* Use in case of an warning */
#define prt_warn(fmt, ...) do {	if (nx_dbg >= 1) {			\
	prt("%s:%u: Warning: "fmt,					\
		__FILE__, __LINE__, ## __VA_ARGS__);			\
}} while (0)

/* Informational printouts */
#define prt_info(fmt, ...) do {	if (nx_dbg >= 2) {			\
	prt("Info: "fmt, ## __VA_ARGS__);				\
}} while (0)

/* Trace zlib wrapper code */
#define prt_trace(fmt, ...) do { if (nx_gzip_trace_enabled()) {		\
	prt("### "fmt, ## __VA_ARGS__);					\
}} while (0)

/* Trace statistics */
#define prt_stat(fmt, ...) do {	if (nx_gzip_gather_statistics()) {	\
	prt("### "fmt, ## __VA_ARGS__);					\
}} while (0)

/* Trace zlib hardware implementation */
#define hw_trace(fmt, ...) do {						\
		if (nx_gzip_hw_trace_enabled())				\
			fprintf(nx_gzip_log, "hhh " fmt, ## __VA_ARGS__); \
	} while (0)

/* Trace zlib software implementation */
#define sw_trace(fmt, ...) do {						\
		if (nx_gzip_sw_trace_enabled())				\
			fprintf(nx_gzip_log, "sss " fmt, ## __VA_ARGS__); \
	} while (0)


/**
 * str_to_num - Convert string into number and copy with endings like
 *              KiB for kilobyte
 *              MiB for megabyte
 *              GiB for gigabyte
 */
uint64_t str_to_num(char *str);
void nx_lib_debug(int onoff);

#endif	/* _NXU_DBG_H_ */
