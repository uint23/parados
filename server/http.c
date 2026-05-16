#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "scan.h"
#include "users.h"
#include "util.h"

struct response {
	const char* status;
	const char* ctype;
	const char* extra;
	const void* body;

	size_t      len;
	int         send_body;
	int         preflight;
};

static void auth_delay_sleep(void);
static int cors_build(char* out, size_t outsz, const char* hdr, int preflight);
static int cors_origin_allowed(const char* origin);
static const struct item* find_item(uint64_t id);
static int item_path_for_id(char out[4096], uint64_t id, const struct user* u);
static const char* mime_from_path(const char* path);
static int parse_hex64(const char* s, uint64_t* out);
static int parse_range(const char* hdr, size_t total, size_t* start, size_t* end);
static int read_request(int c, char* method, size_t msz, char* path, size_t psz, char* hdr, size_t hsz);
static void reply(int c, const char* hdr, const struct response* res);
static int route_dispatch(int c, const char* hdr, const char* path, const struct user* u, int head_only);
static int route_item_path(struct item* it, const char* path, const char* prefix, const struct user* u, int c, const char* hdr);
static int route_library(int c, const char* hdr, const struct user* u, int head_only);
static int route_method(int c, const char* hdr, const char* method, int* head_only);
static int route_rescan(int c, const char* hdr, const struct user* u);
static int stat_item(const struct item* it, struct stat* st);
static int stream_file(int c, const struct item* it, const char* hdr, int head_only);

extern struct library lib;
extern pthread_mutex_t lib_lock;

/**
 * @brief Sleep for configured auth delay
 *
 * @note Retries on EINTR.
 */
static void auth_delay_sleep(void)
{
	if (auth_delay <= 0)
		return;

	struct timespec req;
	struct timespec rem;

	/* convert ms to timespec */
	req.tv_sec = auth_delay / 1000;
	req.tv_nsec = (long)(auth_delay % 1000) * 1000000L;

	while (nanosleep(&req, &rem) < 0) {
		if (errno != EINTR)
			break;
		req = rem;
	}
}

/**
 * @brief Check whether origin is allowed by CORS config
 *
 * @param origin Request Origin header value
 *
 * @return 1=Allowed, 0=Not allowed
 */
static int cors_origin_allowed(const char* origin)
{
	if (cors_origin[0] == '\0')
		return 0;

	if (strcmp(cors_origin, "*") == 0)
		return 1;

	/* comma/space separated allowlist */
	const char* p = cors_origin;
	while (*p) {
		while (*p == ' ' || *p == '\t' || *p == ',')
			p++;

		const char* s = p;
		while (*p && *p != ',')
			p++;

		const char* e = p;
		while (e > s && (e[-1] == ' ' || e[-1] == '\t'))
			e--;

		size_t n = (size_t)(e - s);
		if (n > 0 && strlen(origin) == n && strncmp(origin, s, n) == 0)
			return 1;

		if (*p == ',')
			p++;
	}

	return 0;
}

/**
 * @brief Build CORS response headers
 *
 * @param out Output buffer
 * @param outsz Output buffer size
 * @param hdr Request header block
 * @param preflight Whether this is a preflight response
 *
 * @return 1=Built, 0=Not emitted
 */
static int cors_build(char* out, size_t outsz, const char* hdr, int preflight)
{
	char origin[512];

	if (!out || outsz == 0)
		return 0;

	out[0] = '\0';

	if (cors_origin[0] == '\0')
		return 0;

	if (hdr_get_value(origin, hdr, "origin") < 0)
		return 0;

	if (!cors_origin_allowed(origin))
		return 0;

	int n;
	const char* ao = (strcmp(cors_origin, "*") == 0) ? "*" : origin;
	if (preflight) {
		n = snprintf(
			out, outsz,

			"Access-Control-Allow-Origin: %s\r\n"
			"Vary: Origin\r\n"
			"Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
			"Access-Control-Allow-Headers: Range, Content-Type, Authorization\r\n"
			"Access-Control-Max-Age: 600\r\n",

			ao
		);
	}
	else {
		n = snprintf(
			out, outsz,
			"Access-Control-Allow-Origin: %s\r\n"
			"Vary: Origin\r\n"
			"Access-Control-Expose-Headers: Content-Length, Content-Range, Accept-Ranges, Content-Type\r\n",
			ao
		);
	}

	if (n < 0)
		return 0;
	if ((size_t)n >= outsz) {
		out[0] = '\0';
		return 0;
	}
	return 1;
}

/**
 * @brief Find library item by id
 *
 * @param id Item id
 *
 * @return Item Ptr=Found, NULL=Not found
 */
static const struct item* find_item(uint64_t id)
{
	for (size_t i = 0; i < lib.len; i++)
		if (lib.items[i].id == id)
			return &lib.items[i];

	return NULL;
}

/**
 * @brief Resolve relative item path for a given id
 *
 * @param out Output path buffer
 * @param id Item id
 * @param u Authenticated user filter
 *
 * @return 0=Success, 1=Not found, -1=Failure
 */
static int item_path_for_id(char out[4096], uint64_t id, const struct user* u)
{
	int ret = -1;

	if (!out)
		return -1;

	out[0] = '\0';

	(void)pthread_mutex_lock(&lib_lock);

	const struct item* it = find_item(id);
	if (!it) {
		ret = 1; /* not found */
		goto out;
	}

	if (u && !user_allows_path(u, it->path)) {
		ret = 1; /* not found */
		goto out;
	}

	if (snprintf(out, 4096, "%s", it->path) >= 4096) {
		ret = -1;
		goto out;
	}

	ret = 0;

out:
	(void)pthread_mutex_unlock(&lib_lock);
	return ret; /* 0 ok, 1 not found, -1 error */
}

/**
 * @brief Infer MIME type from file path extension
 *
 * @param path File path
 *
 * @return MIME type string
 */
static const char* mime_from_path(const char* path)
{
	static const struct {
		const char* ext;
		const char* type;
	} types[] = {
		/* audio */
		{ "mp3",  "audio/mpeg" }, { "m4a",  "audio/mp4" },
		{ "aac",  "audio/aac" }, { "flac", "audio/flac" },
		{ "wav",  "audio/wav" }, { "ogg",  "audio/ogg" },
		{ "opus", "audio/opus" },

		/* video */
		{ "mp4",  "video/mp4" }, { "mkv",  "video/x-matroska" },
		{ "webm", "video/webm" }, { "mov",  "video/quicktime" },

		/* images */
		{ "jpg",  "image/jpeg" }, { "jpeg", "image/jpeg" },
		{ "png",  "image/png" }, { "gif",  "image/gif" },
		{ "webp", "image/webp" },

		/* subtitles */
		{ "vtt", "text/vtt" }, { "srt", "application/x-subrip" },

		/* misc */
		{ "pdf", "application/pdf" },
	};
	const char* dot = strrchr(path, '.');

	if (!dot || dot[1] == '\0')
		return "application/octet-stream";

	dot++;

	for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
		if (strcasecmp(dot, types[i].ext) == 0)
			return types[i].type;

	return "application/octet-stream";
}

/**
 * @brief Parse 16-digit hex string to uint64
 *
 * @param s Input string
 * @param out Output value
 *
 * @return 0=Success, -1=Failure
 */
static int parse_hex64(const char* s, uint64_t* out)
{
	if (!s || !out)
		return -1;

	if (strlen(s) != 16)
		return -1;

	for (size_t i = 0; i < 16; i++) {
		unsigned char c = (unsigned char)s[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			return -1;
	}

	char* end = NULL;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 16);
	if (errno != 0 || !end || *end != '\0')
		return -1;

	*out = (uint64_t)v;
	return 0;
}

/**
 * @brief Parse HTTP Range header
 *
 * @param hdr Request header block
 * @param total Total file size in bytes
 * @param start Output start offset
 * @param end Output end offset
 *
 * @return RANGE_NONE=No header, RANGE_OK=Valid range, RANGE_BAD=Malformed, RANGE_UNSAT=Unsatisfiable
 */
static int parse_range(const char* hdr, size_t total, size_t* start, size_t* end)
{
	const char* p = cistrstr(hdr, "\nrange:");
	if (!p)
		p = cistrstr(hdr, "\rrange:");
	if (!p)
		p = cistrstr(hdr, "range:");
	if (!p)
		return RANGE_NONE;

	const char* q = cistrstr(p, "bytes=");
	if (!q)
		return RANGE_BAD;
	q += 6;

	while (*q == ' ')
		q++;

	if (*q == '-') {
		/* bytes=-SUFFIX */
		const char* r = q + 1;
		char* e2 = NULL;

		errno = 0;
		unsigned long long suf = strtoull(r, &e2, 10);
		if (errno != 0 || e2 == r)
			return RANGE_BAD;
		if (suf == 0)
			return RANGE_BAD;

		if (total == 0)
			return RANGE_UNSAT;

		if ((size_t)suf >= total) {
			*start = 0;
			*end = total - 1;
			return RANGE_OK;
		}

		*start = total - (size_t)suf;
		*end = total - 1;
		return RANGE_OK;
	}

	/* bytes=START- or bytes=START-END */
	char* e1 = NULL;

	errno = 0;
	unsigned long long a = strtoull(q, &e1, 10);
	if (errno != 0 || e1 == q)
		return RANGE_BAD;

	if (*e1 != '-')
		return RANGE_BAD;

	const char* r = e1 + 1;

	if (*r == '\r' || *r == '\n' || *r == '\0') {
		/* bytes=START- */
		if ((size_t)a >= total)
			return RANGE_UNSAT;
		*start = (size_t)a;
		*end = total - 1;
		return RANGE_OK;
	}

	char* e2 = NULL;

	errno = 0;
	unsigned long long b = strtoull(r, &e2, 10);
	if (errno != 0 || e2 == r)
		return RANGE_BAD;

	if ((size_t)a >= total)
		return RANGE_UNSAT;
	if ((size_t)b >= total)
		b = total - 1;
	if (b < a)
		return RANGE_BAD;

	*start = (size_t)a;
	*end = (size_t)b;
	return RANGE_OK;
}

/**
 * @brief Read and parse a single HTTP request
 *
 * @param c Connected client socket file descriptor
 * @param method Output method buffer
 * @param msz Method buffer size
 * @param path Output path buffer
 * @param psz Path buffer size
 * @param hdr Output raw header buffer
 * @param hsz Header buffer size
 *
 * @return 0=Success, -1=Failure
 */
static int read_request(int c, char* method, size_t msz, char* path, size_t psz, char* hdr, size_t hsz)
{
	size_t used = 0;

	for (;;) {
		if (used + 1 >= hsz)
			return -1;

		ssize_t r = read(c, hdr + used, hsz - 1 - used);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (r == 0)
			return -1;

		used += (size_t)r;
		hdr[used] = '\0';

		if (strstr(hdr, "\r\n\r\n"))
			break;
	}

	char* eol = strstr(hdr, "\r\n");
	if (!eol)
		return -1;
	*eol = '\0';

	if (sscanf(hdr, "%15s %1023s", method, path) != 2)
		return -1;

	method[msz-1] = '\0';
	path[psz-1] = '\0';

	*eol = '\r';

	return 0;
}

/**
 * @brief Send HTTP response header and optional body
 *
 * @param c Connected client socket file descriptor
 * @param hdr Request header block
 * @param res Response descriptor
 */
static void reply(int c, const char* hdr, const struct response* res)
{
	char resp[HTTP_RESP_MAX];
	char cors[512];

	(void)cors_build(cors, sizeof(cors), hdr, res->preflight);

	int n = snprintf(
		resp, sizeof(resp),

		"%s"
		"%s"
		"%s"
		"%s"
		HTTP_LENGTH
		HTTP_CLOSE
		"\r\n",

		res->status,
		cors,
		(res->ctype && res->ctype[0]) ? res->ctype : "",
		(res->extra && res->extra[0]) ? res->extra : "",
		res->len
	);

	if (n < 0)
		return;

	if ((size_t)n >= sizeof(resp)) {
		struct response err = {
			.status = HTTP_500,
			.ctype = HTTP_TEXT,
			.extra = NULL,
			.body = "Server Error\n",
			.len = sizeof("Server Error\n") - 1,
			.send_body = 1,
			.preflight = 0,
		};
		reply(c, hdr, &err);
		return;
	}

	(void)write_all(c, resp, (size_t)n);

	if (res->send_body && res->body && res->len > 0)
		(void)write_all(c, res->body, res->len);
}

/**
 * @brief Dispatch request path to route handler
 *
 * @param c Connected client socket file descriptor
 * @param hdr Request header block
 * @param path Request path
 * @param u Authenticated user filter
 * @param head_only Whether request method is HEAD
 *
 * @return 0=Handled, -1=Failure
 */
static int route_dispatch(int c, const char* hdr, const char* path, const struct user* u, int head_only)
{
	if (strcmp(path, "/ping") == 0) {
		LOG(verbose_log, "HTTP", "Route              /ping");
		reply(c, hdr, &(struct response){ HTTP_200, HTTP_TEXT, NULL, "OK\n", sizeof("OK\n") - 1, 1, 0 });
		return 0;
	}

	if (strcmp(path, "/library") == 0)
		return route_library(c, hdr, u, head_only);

	if (strcmp(path, "/rescan") == 0)
		return route_rescan(c, hdr, u);

	if (strncmp(path, "/stream/", 8) == 0) {
		struct item tmp;
		char rel[4096];

		LOG(verbose_log, "HTTP", "Route              /stream");
		tmp.path = rel;

		int rc = route_item_path(&tmp, path, "/stream/", u, c, hdr);
		if (rc != 0)
			return rc < 0 ? -1 : 0;

		return stream_file(c, &tmp, hdr, head_only);
	}

	if (strncmp(path, "/meta/", 6) == 0) {
		struct item tmp;
		char rel[4096];
		struct stat st;
		const char* type;
		struct json j;

		LOG(verbose_log, "HTTP", "Route              /meta");
		tmp.path = rel;

		int rc = route_item_path(&tmp, path, "/meta/", u, c, hdr);
		if (rc != 0)
			return rc < 0 ? -1 : 0;

		if (stat_item(&tmp, &st) < 0) {
			reply(c, hdr, &(struct response){ HTTP_404, HTTP_TEXT, NULL, "Not Found\n", sizeof("Not Found\n") - 1, 1, 0 });
			return 0;
		}

		type = mime_from_path(tmp.path);
		if (json_meta(&j, &tmp, (size_t)st.st_size, (long)st.st_mtime, type) < 0) {
			reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "JSON Failed\n", sizeof("JSON Failed\n") - 1, 1, 0 });
			return -1;
		}

		reply(c, hdr, &(struct response){ HTTP_200, HTTP_JSON, NULL, j.buf, j.len, !head_only, 0 });
		json_free(&j);
		return 0;
	}

	LOG(verbose_log, "HTTP", "Route not found    %s", path);
	reply(c, hdr, &(struct response){ HTTP_404, HTTP_TEXT, NULL, "Not Found\n", sizeof("Not Found\n") - 1, 1, 0 });
	return 0;
}

/**
 * @brief Parse route id and resolve to an item
 *
 * @param it Output item
 * @param path Request path
 * @param prefix Route prefix
 * @param u Authenticated user filter
 * @param c Connected client socket file descriptor
 * @param hdr Request header block
 *
 * @return 0=Success, 1=Response sent, -1=Failure
 */
static int route_item_path(struct item* it, const char* path, const char* prefix, const struct user* u, int c, const char* hdr)
{
	uint64_t id;
	int fr;

	if (parse_hex64(path + strlen(prefix), &id) < 0) {
		reply(c, hdr, &(struct response){
			.status = HTTP_400,
			.ctype = HTTP_TEXT,
			.extra = NULL,
			.body = "Bad Request\n",
			.len = sizeof("Bad Request\n") - 1,
			.send_body = 1,
			.preflight = 0,
		});
		return 1;
	}

	fr = item_path_for_id(it->path, id, u);
	if (fr == 1) {
		reply(c, hdr, &(struct response){
			.status = HTTP_404,
			.ctype = HTTP_TEXT,
			.extra = NULL,
			.body = "Not Found\n",
			.len = sizeof("Not Found\n") - 1,
			.send_body = 1,
			.preflight = 0,
		});
		return 1;
	}
	if (fr < 0) {
		reply(c, hdr, &(struct response){
			.status = HTTP_500,
			.ctype = HTTP_TEXT,
			.extra = NULL,
			.body = "Server Error\n",
			.len = sizeof("Server Error\n") - 1,
			.send_body = 1,
			.preflight = 0,
		});
		return -1;
	}

	it->id = id;
	return 0;
}

/**
 * @brief Handle /library route
 *
 * @param c Connected client socket file descriptor
 * @param hdr Request header block
 * @param u Authenticated user filter
 * @param head_only Whether request method is HEAD
 *
 * @return 0=Handled, -1=Failure
 */
static int route_library(int c, const char* hdr, const struct user* u, int head_only)
{
	struct json j;
	struct library view;

	LOG(verbose_log, "HTTP", "Route              /library");
	(void)pthread_mutex_lock(&lib_lock);
	memset(&view, 0, sizeof(view));

	if (!u) {
		view = lib;
	}
	else {
		size_t i;

		if (lib.len > 0) {
			view.items = calloc(lib.len, sizeof(*view.items));
			if (!view.items) {
				(void)pthread_mutex_unlock(&lib_lock);
				reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "Server Error\n", sizeof("Server Error\n") - 1, 1, 0 });
				return -1;
			}
		}

		for (i = 0; i < lib.len; i++) {
			if (!user_allows_path(u, lib.items[i].path))
				continue;
			view.items[view.len++] = lib.items[i];
		}
		view.cap = view.len;
	}

	if (json_library(&j, &view) < 0) {
		if (u)
			free(view.items);

		(void)pthread_mutex_unlock(&lib_lock);
		LOG(verbose_log, "JSON", "Encode     FAILED");
		reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "JSON Encode Failed\n", sizeof("JSON Encode Failed\n") - 1, 1, 0 });
		return -1;
	}

	(void)pthread_mutex_unlock(&lib_lock);
	LOG(verbose_log, "JSON", "Encoded bytes      %zu bytes", j.len);
	reply(c, hdr, &(struct response){ HTTP_200, HTTP_JSON, NULL, j.buf, j.len, !head_only, 0 });
	json_free(&j);

	if (u)
		free(view.items);
	return 0;
}

/**
 * @brief Handle request method and OPTIONS
 *
 * @param c Connected client socket file descriptor
 * @param hdr Request header block
 * @param method HTTP method
 * @param head_only Output HEAD flag
 *
 * @return 0=Continue, 1=Response sent
 */
static int route_method(int c, const char* hdr, const char* method, int* head_only)
{
	if (strcmp(method, "GET") == 0) {
		*head_only = 0;
		return 0;
	}
	if (strcmp(method, "HEAD") == 0) {
		*head_only = 1;
		return 0;
	}
	if (strcmp(method, "OPTIONS") == 0) {
		reply(c, hdr, &(struct response){ HTTP_204, NULL, NULL, NULL, 0, 0, 1 });
		return 1;
	}

	LOG(verbose_log, "HTTP", "Method forbidden   %s", method);
	reply(c, hdr, &(struct response){ HTTP_405, HTTP_TEXT, NULL, "Method Not Allowed\n", sizeof("Method Not Allowed\n") - 1, 1, 0 });
	return 1;
}

/**
 * @brief Handle /rescan route
 *
 * @param c Connected client socket file descriptor
 * @param hdr Request header block
 * @param u Authenticated user
 *
 * @return 0=Handled, -1=Failure
 */
static int route_rescan(int c, const char* hdr, const struct user* u)
{
	struct timespec t0;
	struct timespec t1;
	size_t before = 0;
	size_t after = 0;

	LOG(verbose_log, "HTTP", "Route              /rescan");
	if (users.len == 0) {
		LOG(true, "HTTP", "Rescan forbidden   Auth disabled");
		reply(c, hdr, &(struct response){ HTTP_403, HTTP_TEXT, NULL, "Forbidden\n", sizeof("Forbidden\n") - 1, 1, 0 });
		return 0;
	}

	LOG(true, "SCAN", "Rescan requested   %s", u->name);
	clock_gettime(CLOCK_MONOTONIC, &t0);

	(void)pthread_mutex_lock(&lib_lock);
	before = lib.len;
	LOG(verbose_log, "SCAN", "Rescan begin       %s (%zu items)", media_dir, before);
	int ok = scan_library_rescan(&lib, media_dir);
	after = lib.len;
	(void)pthread_mutex_unlock(&lib_lock);

	clock_gettime(CLOCK_MONOTONIC, &t1);
	long ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

	if (ok < 0) {
		LOG(true, "SCAN", "Rescan FAILED      %s (%ld ms)", media_dir, ms);
		reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "Rescan Failed\n", sizeof("Rescan Failed\n") - 1, 1, 0 });
		return -1;
	}

	LOG(true, "SCAN", "Rescan OK          %zu -> %zu (%ld ms)", before, after, ms);
	reply(c, hdr, &(struct response){ HTTP_200, HTTP_TEXT, NULL, "OK\n", sizeof("OK\n") - 1, 1, 0 });
	return 0;
}

/**
 * @brief Stat media item on disk
 *
 * @param it Library item
 * @param st Output stat buffer
 *
 * @return 0=Success, -1=Failure
 */
static int stat_item(const struct item* it, struct stat* st)
{
	char full[4096];

	if (join_path(full, sizeof(full), media_dir, it->path) < 0)
		return -1;

	if (stat(full, st) < 0)
		return -1;

	if (!S_ISREG(st->st_mode))
		return -1;

	return 0;
}

/**
 * @brief Stream item file to HTTP client
 *
 * @param c Connected client socket file descriptor
 * @param it Library item
 * @param hdr Request header block
 * @param head_only Whether request method is HEAD
 *
 * @return 0=Handled, -1=Failure
 */
static int stream_file(int c, const struct item* it, const char* hdr, int head_only)
{
	char full[4096];

	/* build absolute path */
	if (join_path(full, sizeof(full), media_dir, it->path) < 0) {
		LOG(verbose_log, "HTTP", "Stream FAILED      Path too long");
		reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "Server Error\n", sizeof("Server Error\n") - 1, 1, 0 });
		return -1;
	}

	/* open file */
	int fd = open(full, O_RDONLY);
	if (fd < 0) {
		LOG(verbose_log, "HTTP", "Open FAILED        %s", it->path);
		reply(c, hdr, &(struct response){ HTTP_404, HTTP_TEXT, NULL, "Not Found\n", sizeof("Not Found\n") - 1, 1, 0 });
		return 0;
	}

	/* stat file */
	struct stat st;
	if (fstat(fd, &st) < 0) {
		close(fd);
		reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "Server Error\n", sizeof("Server Error\n") - 1, 1, 0 });
		return -1;
	}

	/* reject non-regular files */
	if (!S_ISREG(st.st_mode)) {
		close(fd);
		reply(c, hdr, &(struct response){ HTTP_404, HTTP_TEXT, NULL, "Not Found\n", sizeof("Not Found\n") - 1, 1, 0 });
		return 0;
	}

	size_t total = (size_t)st.st_size;
	size_t start = 0;
	size_t end = total ? (total - 1) : 0;
	const char* type = mime_from_path(it->path);

	int partial = parse_range(hdr, total, &start, &end);
	if (partial == RANGE_BAD) {
		close(fd);
		reply(c, hdr, &(struct response){ HTTP_400, HTTP_TEXT, NULL, "Bad Range\n", sizeof("Bad Range\n") - 1, 1, 0 });
		return 0;
	}

	if (partial == RANGE_UNSAT) {
		close(fd);
		reply(c, hdr, &(struct response){ HTTP_416, HTTP_TEXT, NULL, "Range Not Satisfiable\n", sizeof("Range Not Satisfiable\n") - 1, 1, 0 });
		return 0;
	}

	if (partial == RANGE_OK) {
		if (lseek(fd, (off_t)start, SEEK_SET) < 0) {
			close(fd);
			reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "Server Error\n", sizeof("Server Error\n") - 1, 1, 0 });
			return -1;
		}
	}

	size_t len = (total == 0) ? 0 : (end - start + 1);

	/* build response header */
	char ctype[128];
	char extra[256];

	if (snprintf(ctype, sizeof(ctype), HTTP_CTYPE, type) >= (int)sizeof(ctype)) {
		close(fd);
		reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "Server Error\n", sizeof("Server Error\n") - 1, 1, 0 });
		return -1;
	}

	if (partial == RANGE_OK) {
		/* partial */
		if (snprintf(
			extra, sizeof(extra),
			HTTP_RANGE
			HTTP_CRG,
			start, end, total
		) >= (int)sizeof(extra)) {
			close(fd);
			reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "Server Error\n", sizeof("Server Error\n") - 1, 1, 0 });
			return -1;
		}

		reply(c, hdr, &(struct response){
			.status = HTTP_206,
			.ctype = ctype,
			.extra = extra,
			.body = NULL,
			.len = len,
			.send_body = 0,
			.preflight = 0,
		});
	}
	else {
		/* non-partial */
		if (snprintf(extra, sizeof(extra), HTTP_RANGE) >= (int)sizeof(extra)) {
			close(fd);
			reply(c, hdr, &(struct response){ HTTP_500, HTTP_TEXT, NULL, "Server Error\n", sizeof("Server Error\n") - 1, 1, 0 });
			return -1;
		}

		reply(c, hdr, &(struct response){
			.status = HTTP_200,
			.ctype = ctype,
			.extra = extra,
			.body = NULL,
			.len = len,
			.send_body = 0,
			.preflight = 0,
		});
	}

	/* HEAD: no body */
	if (head_only) {
		close(fd);

		if (partial == RANGE_OK)
			LOG(verbose_log, "HTTP", "Header             %s [%zu-%zu/%zu]", it->path, start, end, total);
		else
			LOG(verbose_log, "HTTP", "Header             %s (%zu bytes)", it->path, total);

		return 0;
	}

	/* stream file */
	char buf[8192];
	size_t left = len;

	while (left > 0) {
		size_t want = sizeof(buf);
		if (want > left)
			want = left;

		ssize_t r = read(fd, buf, want);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		if (r == 0)
			break;

		if (write_all(c, buf, (size_t)r) < 0)
			break;

		left -= (size_t)r;
	}

	close(fd);

	if (partial == RANGE_OK)
		LOG(verbose_log, "HTTP", "Streamed           %s [%zu-%zu/%zu]", it->path, start, end, total);
	else
		LOG(verbose_log, "HTTP", "Streamed           %s (%zu bytes)", it->path, total);

	return 0;
}

int http_handle(int c)
{
	char method[16];
	char path[1024];
	char hdr[HTTP_REQ_MAX];
	const struct user* u = NULL;
	method[0] = '\0';
	path[0] = '\0';
	hdr[0] = '\0';

	if (read_request(c, method, sizeof(method), path, sizeof(path), hdr, sizeof(hdr)) < 0) {
		reply(c, hdr, &(struct response){ HTTP_400, HTTP_TEXT, NULL, "Bad Request\n", sizeof("Bad Request\n") - 1, 1, 0 });
		return -1;
	}
	LOG(verbose_log, "HTTP", "Request            %s %s", method, path);

	int head_only = 0;
	if (route_method(c, hdr, method, &head_only) != 0)
		return 0;

	if (users.len > 0) {
		u = users_auth_from_hdr(hdr);
		if (!u) {
			auth_delay_sleep();
			reply(c, hdr, &(struct response){
				.status = "HTTP/1.1 401 Unauthorized\r\n",
				.ctype = HTTP_TEXT,
				.extra = "WWW-Authenticate: Basic realm=\"parados\"\r\n",
				.body = "unauthorized\n",
				.len = sizeof("unauthorized\n") - 1,
				.send_body = !head_only,
				.preflight = 0,
			});
			return 0;
		}
	}

	return route_dispatch(c, hdr, path, u, head_only);
}
