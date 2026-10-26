#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "log.h"
#include "users.h"

static int load_path(const char* path);
static int parse_bool(const char* s, bool* out);
static int set_kv(const char* k, const char* v);
static char* trim(char* s);

/* defaults */
char media_dir[4096] = "/mnt/sto/Media/";
char server_addr[64] = "127.0.0.1";
int  server_port = 8088;
bool verbose_log = true;
char cors_origin[1024] = "";
int http_io_timeout = 5;
int max_clients = 64;
int auth_delay = 250;

/**
 * @brief Load configuration from a file path
 *
 * @param path Path to config file
 *
 * @return 0=Success, -1=Failure
 */
static int load_path(const char* path)
{
	FILE* f = fopen(path, "r");
	if (!f)
		return -1;

	char* line = NULL;
	size_t cap = 0;
	ssize_t n;

	while ((n = getline(&line, &cap, f)) >= 0) {
		(void)n;

		char* s = trim(line);
		if (s[0] == '\0')
			continue;
		if (s[0] == '#')
			continue;

		char* eq = strchr(s, '=');
		if (!eq)
			continue;

		*eq = '\0';
		char* k = trim(s);
		char* v = trim(eq + 1);

		if (k[0] == '\0')
			continue;

		if (set_kv(k, v) < 0)
			LOG_FIELD(true, "CONF", "Config Error", "%s=%s", k, v);
	}

	free(line);
	fclose(f);
	return 0;
}

/**
 * @brief Parse bool string value
 *
 * @param s Input string
 * @param out Output bool
 *
 * @return 0=Success, -1=Failure
 */
static int parse_bool(const char* s, bool* out)
{
	if (!s || !out)
		return -1;

	if (strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0)
		*out = true;
	else if (strcmp(s, "0") == 0 || strcasecmp(s, "false") == 0 || strcasecmp(s, "no") == 0)
		*out = false;
	else
		return -1;

	return 0;
}

/**
 * @brief Apply a config key/value pair
 *
 * @param k Config key
 * @param v Config value
 *
 * @return 0=Success, -1=Failure
 */
static int set_kv(const char* k, const char* v)
{
	/* regular */
	if (strcmp(k, "media_dir") == 0) {
		snprintf(media_dir, sizeof(media_dir), "%s", v);
		return 0;
	}

	if (strcmp(k, "server_addr") == 0) {
		snprintf(server_addr, sizeof(server_addr), "%s", v);
		return 0;
	}

	if (strcmp(k, "server_port") == 0) {
		char* end = NULL;
		long p = strtol(v, &end, 10);

		if (end == v || *end != '\0') {
			LOG_FIELD(true, "CONF", "Config Error", "%s invalid integer '%s'", k, v);
			return -1;
		}
		if (p < 1 || p > 65535) {
			LOG_FIELD(true, "CONF", "Config Error", "server_port out of bounds (1..65535)");
			return -1;
		}

		server_port = (int)p;
		return 0;
	}

	if (strcmp(k, "verbose_log") == 0) {
		bool b;
		if (parse_bool(v, &b) < 0){
			LOG_FIELD(true, "CONF", "Config Error", "verbose_log invalid bool \"%s\"", v);
			return -1;
		}

		verbose_log = b;
		return 0;
	}

	if (strcmp(k, "cors_origin") == 0) {
		snprintf(cors_origin, sizeof(cors_origin), "%s", v);
		return 0;
	}

	if (strcmp(k, "http_io_timeout") == 0) {
		char* end = NULL;
		long t = strtol(v, &end, 10);

		if (end == v || *end != '\0') {
			LOG_FIELD(true, "CONF", "Config Error", "%s invalid integer '%s'", k, v);
			return -1;
		}
		if (t < 1 || t > 300) {
			LOG_FIELD(true, "CONF", "Config Error", "http_io_timeout too %s (1..300)", t > 300 ? "high" : "low");
			return -1;
		}

		http_io_timeout = (int)t;
		return 0;
	}

	if (strcmp(k, "max_clients") == 0) {
		char* end = NULL;
		long n = strtol(v, &end, 10);

		if (end == v || *end != '\0') {
			LOG_FIELD(true, "CONF", "Config Error", "%s invalid integer '%s'", k, v);
			return -1;
		}
		if (n < 1 || n > 1024) {
			LOG_FIELD(true, "CONF", "Config Error", "max_clients too %s (1..1024)", n > 1024 ? "high" : "low");
			return -1;
		}

		max_clients = (int)n;
		return 0;
	}

	if (strcmp(k, "auth_delay") == 0) {
		char* end = NULL;
		long n = strtol(v, &end, 10);

		if (end == v || *end != '\0') {
			LOG_FIELD(true, "CONF", "Config Error", "%s invalid integer '%s'", k, v);
			return -1;
		}
		if (n < 0 || n > INT_MAX) {
			LOG_FIELD(true, "CONF", "Config Error", "auth_delay out of bounds (0..%d)", INT_MAX);
			return -1;
		}

		auth_delay = (int)n;
		return 0;
	}

	/* auth */
	if (strcmp(k, "user") == 0) {
		int r = users_push(v);
		if (r < 0)
			LOG_FIELD(true, "CONF", "Config Error", "user invalid '%s'", v);
		return r;
	}

	if (strcmp(k, "pass") == 0) {
		int r = users_set_pass(v);
		if (r < 0)
			LOG_FIELD(true, "CONF", "Config Error", "pass invalid '%s'", v);
		return r;
	}

	if (strcmp(k, "allow") == 0) {
		int r = users_add_allow(v);
		if (r < 0)
			LOG_FIELD(true, "CONF", "Config Error", "allow invalid '%s'", v);
		return r;
	}

	/* unknown key */
	LOG_FIELD(true, "CONF", "Unknown Key Value", "%s=%s", k, v);
	return 0;
}

/**
 * @brief Trim leading and trailing whitespace
 *
 * @param s Input string
 *
 * @return Ptr to trimmed string
 */
static char* trim(char* s)
{
	while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
		s++;

	char* e = s + strlen(s);
	while (e > s) {
		char c = e[-1];
		if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
			break;
		e--;
	}
	*e = '\0';
	return s;
}

void config_load(void)
{
	const char* e = getenv("PARADOS_CONFIG");
	const char* path = "";
	if (e && e[0]) {
		(void)load_path(e);
		path = e;
		goto log;
	}

	if (load_path("/etc/parados.conf") == 0) {
		path = "/etc/parados.conf";
		goto log;
	}

	if (load_path("./parados.conf") == 0) {
		path = "./parados.conf";
		goto log;
	}

	LOG(true, "CONF", "Unable to load configuration... using defaults");
	return;

log:
	(void)0;

	char port[8];
	char tmo[16];
	char maxc[16];
	char adly[16];

	LOG_FIELD(true, "CONF", "Config File", "%s", path);
	LOG_FIELD(true, "CONF", "Media Directory", "%s", media_dir);
	LOG_FIELD(true, "CONF", "Server Address", "%s", server_addr);
	snprintf(port, sizeof(port), "%d", server_port);
	LOG_FIELD(true, "CONF", "Server Port", "%s", port);
	LOG_FIELD(true, "CONF", "Verbose Logging", "%s", (verbose_log) ? "true" : "false");
	LOG_FIELD(true, "CONF", "CORS origins", "%s", cors_origin);
	snprintf(tmo, sizeof(tmo), "%d", http_io_timeout);
	LOG_FIELD(true, "CONF", "HTTP IO Timeout", "%s", tmo);
	snprintf(maxc, sizeof(maxc), "%d", max_clients);
	LOG_FIELD(true, "CONF", "Max Clients", "%s", maxc);
	snprintf(adly, sizeof(adly), "%d", auth_delay);
	LOG_FIELD(true, "CONF", "Auth Delay", "%s (ms)", adly);

	return;
}

