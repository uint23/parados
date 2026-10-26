#ifndef LOG_H
#define LOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

/**
 * @brief Format local time as "dd/mm/YYYY HH:MM:SS"
 *
 * @param out Output buffer (must be at least 20 bytes)
 */
static inline void log_datetime(char out[20])
{
	time_t t = time(NULL);
	struct tm tmv;

	localtime_r(&t, &tmv);
	strftime(out, 20, "%d/%m/%Y %H:%M:%S", &tmv);
}

/**
 * @brief Print log message if verbose=true
 *
 * @note In DEBUG builds, also prints [file : line : func()]
 *
 * @param verbose Whether to emit the log line
 * @param tag Short tag (e.g. "HTTP", "CONF")
 * @param fmt printf-style format string
 * @param ap va_list
 */
static inline void log_vprint(bool verbose, const char* tag,
#if defined(DEBUG)
		const char* file, int line, const char* func,
#endif
		const char* fmt, va_list ap)
{
	if (!verbose)
		return;

	char dt[20];
	log_datetime(dt);
	fprintf(stderr, "%s [%s] ", dt, tag);

#if defined(DEBUG)
	fprintf(stderr, "[%s : %d : %s()] ", file, line, func);
#endif

	vfprintf(stderr, fmt, ap);

	fputc('\n', stderr);
}

/**
 * @brief Print a log message if verbose=true
 *
 * @param verbose Whether to emit the log line
 * @param tag Short tag (e.g. "HTTP", "CONF")
 * @param fmt printf-style format string
 */
static inline void log_print(bool verbose, const char* tag,
#if defined(DEBUG)
		const char* file, int line, const char* func,
#endif
		const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);

	log_vprint(verbose, tag,
#if defined(DEBUG)
		file, line, func,
#endif
		fmt, ap);

	va_end(ap);
}

/**
 * @brief Log macro
 *
 * @param verbose_log Whether to emit the log line
 * @param tag Short tag string
 * @param fmt printf-style format string
 */
#if defined(DEBUG)
#define LOG(verbose_log, tag, fmt, ...) \
	log_print((verbose_log), (tag), __FILE__, __LINE__, __func__, (fmt), ##__VA_ARGS__)
#else
#define LOG(verbose_log, tag, fmt, ...) \
	log_print((verbose_log), (tag), (fmt), ##__VA_ARGS__)
#endif

/* Align a label before a printf-style message. */
#define LOG_FIELD(verbose, tag, label, fmt, ...) \
	LOG((verbose), (tag), "%-24s " fmt, (label), ##__VA_ARGS__)

#endif /* LOG_H */

