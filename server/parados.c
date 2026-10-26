/*            parados
	the simple home media server

	See parados(1), parados(7), parados.conf(5)
	for usage information

	This software is licensed under ISC.
	Check LICENCE for more details.
*/

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>

#include "config.h"
#include "http.h"
#include "log.h"
#include "scan.h"
#include "util.h"

#ifndef GIT_VER
#define GIT_VER "unknown"
#endif /* GIT_VER */

static void apply_rlimits(void);
static void* client_thread(void* arg);
static void fd_set_cloexec(int fd);
static void sock_set_timeouts(int fd);
static void release_slot(void);
static int try_acquire_slot(void);

void die(const char* s, int e);
void run(void);
void setup(void);

static int sock;
struct library lib;
pthread_mutex_t lib_lock;
static pthread_mutex_t slots_lock;
static int slots_available;

/**
 * @brief Apply process resource limits
 */
static void apply_rlimits(void)
{
	struct rlimit rl;

	rl.rlim_cur = 0;
	rl.rlim_max = 0;
	(void)setrlimit(RLIMIT_CORE, &rl);

	rl.rlim_cur = 1024;
	rl.rlim_max = 1024;
	(void)setrlimit(RLIMIT_NOFILE, &rl);
}

/**
 * @brief Handle a single accepted client connection
 *
 * @param arg Client socket cast through void*
 */
static void* client_thread(void* arg)
{
	int c = (int)(intptr_t)arg;

	sock_set_timeouts(c);
	(void)http_handle(c);
	shutdown(c, SHUT_WR);
	close(c);

	release_slot();
	return NULL;
}

/**
 * @brief Set close on exec on file descriptor
 *
 * @param fd File descriptor
 */
static void fd_set_cloexec(int fd)
{
	int f = fcntl(fd, F_GETFD);
	if (f >= 0)
		(void)fcntl(fd, F_SETFD, f | FD_CLOEXEC);
}

/**
 * @brief Apply configured socket IO timeouts
 *
 * @param fd File descriptor
 */
static void sock_set_timeouts(int fd)
{
	struct timeval tv;
	tv.tv_sec = http_io_timeout;
	tv.tv_usec = 0;

	(void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/**
 * @brief Release one client slot back to the pool
 */
static void release_slot(void)
{
	if (pthread_mutex_lock(&slots_lock) != 0)
		return;

	if (slots_available < max_clients)
		slots_available++;

	(void)pthread_mutex_unlock(&slots_lock);
}

/**
 * @brief Try to reserve one client slot
 *
 * @return 1=Acquired, 0=Full, -1=Failure
 */
static int try_acquire_slot(void)
{
	int ok = 0;

	if (pthread_mutex_lock(&slots_lock) != 0)
		return -1;

	if (slots_available > 0) {
		slots_available--;
		ok = 1;
	}

	(void)pthread_mutex_unlock(&slots_lock);
	return ok;
}

void die(const char* s, int e)
{
	perror(s);
	exit(e);
}

void run(void)
{
	for (;;) {
		int c;

		c = accept(sock, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR)
				continue;
			continue;
		}

		fd_set_cloexec(c);

		/* client cap. if full 503 and close */
		if (try_acquire_slot() != 1) {
			/* short 503 + retry after */
			static const char resp[] =
				"HTTP/1.1 503 Service Unavailable\r\n"
				"Content-Type: text/plain\r\n"
				"Retry-After: 1\r\n"
				"Content-Length: 5\r\n"
				"Connection: close\r\n"
				"\r\n"
				"busy\n";
			(void)write_all(c, resp, sizeof(resp) - 1);

			close(c);
			LOG_FIELD(true, "CORE", "Connection", "Busy (503)");
			continue;
		}

		LOG_FIELD(verbose_log, "CORE", "Connection", "Accepted");
		pthread_t t;
		int err = pthread_create(&t, NULL, client_thread, (void*)(intptr_t)c);
		if (err != 0) {
			LOG_FIELD(true, "CORE", "pthread_create FAILED", "%d", err);
			close(c);
			release_slot();
			continue;
		}

		(void)pthread_detach(t);
	}
}

void setup(void)
{
	signal(SIGPIPE, SIG_IGN);

	config_load();
	apply_rlimits();

	if (pthread_mutex_init(&lib_lock, NULL) != 0)
		die("pthread_mutex_init", EXIT_FAILURE);

	if (pthread_mutex_init(&slots_lock, NULL) != 0)
		die("pthread_mutex_init", EXIT_FAILURE);
	slots_available = max_clients;

	int ret = 1;
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		die("socket", EXIT_FAILURE);
	fd_set_cloexec(sock);

	int yes = 1;
	ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	if (ret < 0)
		die("setsockopt", EXIT_FAILURE);

	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(server_port);
	ret = inet_pton(AF_INET, server_addr, &a.sin_addr);
	if (ret != 1)
		die("inet_pton", EXIT_FAILURE);

	ret = bind(sock, (struct sockaddr*)&a, sizeof(a));
	if (ret < 0)
		die("bind", EXIT_FAILURE);

	if (scan_library(&lib, media_dir) < 0)
		die("scan_library", EXIT_FAILURE);
	LOG_FIELD(verbose_log, "SCAN", "Cached items", "%zu", lib.len);

	ret = listen(sock, LISTEN_BACKLOG);
	if (ret < 0)
		die("listen", EXIT_FAILURE);

	LOG_FIELD(verbose_log, "CORE", "Listening on", "%s:%d", server_addr, server_port);
#ifdef __OpenBSD__
	if (unveil(media_dir, "r") < 0)
		die("unveil", EXIT_FAILURE);
	if (unveil(NULL, NULL) < 0)
		die("unveil", EXIT_FAILURE);
#endif
}

int main(int argc, char* argv[])
{
	if (argc > 1) {
		if (strcmp(argv[1], "-v") == 0) {
			printf(
				"parados v%s-%s\n"
				"\n"
				"This software is licensed under ISC.\n"
				"See LICENSE for license information.\n"
				"\n"
				"See parados(1), parados(7), parados.conf(5)\n"
				"for usage information\n",
				VERSION, GIT_VER
			);
			return EXIT_SUCCESS;
		}
		else {
			printf("Invalid command\n");
			printf("See parados(1) for more information\n");
			return EXIT_FAILURE;
		}
	}

	setup();
#ifdef __OpenBSD__
	if (pledge("stdio inet proc rpath", NULL) < 0)
		die("pledge", EXIT_FAILURE);
#endif /* __OpenBSD__ */
	run();
	return EXIT_SUCCESS;
}

