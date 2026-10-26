#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "config.h"
#include "log.h"
#include "users.h"
#include "util.h"

struct users users;

static int allow_push(struct user* u, const char* s);
static int b64_decode(unsigned char* out, size_t outsz, const char* in);
static int b64_val(int c);
static int ct_equal(const char* a, const char* b);
static const struct user* find_user(const char* name);

/**
 * @brief Append allow prefix to user entry
 *
 * @param u User entry
 * @param s Allow prefix string
 *
 * @return 0=Success, -1=Failure
 */
static int allow_push(struct user* u, const char* s)
{
	if (u->allow_len == u->allow_cap) {
		size_t ncap = u->allow_cap ? (u->allow_cap * 2) : 8;
		char** nv = realloc(u->allow, ncap * sizeof(*nv));
		if (!nv)
			return -1;

		u->allow = nv;
		u->allow_cap = ncap;
	}

	char* p = strdup(s);
	if (!p)
		return -1;

	u->allow[u->allow_len++] = p;
	return 0;
}

/**
 * @brief Decode base64 string
 *
 * @param out Output buffer
 * @param outsz Output buffer size
 * @param in Input base64 string
 *
 * @return 0=Success, -1=Failure
 */
static int b64_decode(unsigned char* out, size_t outsz, const char* in)
{
	size_t olen = 0;
	int buf = 0;
	int bits = 0;

	for (; *in; in++) {
		int v = b64_val((unsigned char)*in);
		if (v == -1)
			continue; /* skip whitespace/junk */

		if (v == -2)
			break; /* padding */

		buf = (buf << 6) | v; /* append 6 bits */
		bits += 6;

		if (bits >= 8) {
			bits -= 8;
			if (olen + 1 >= outsz)
				return -1; /* overflow */

			out[olen++] = (unsigned char)((buf >> bits) & 0xff);
		}
	}

	if (olen >= outsz)
		return -1;

	out[olen] = '\0';
	return 0;
}

/**
 * @brief Map base64 byte to sextet value
 *
 * @param c Input byte
 *
 * @return 0..63=Value, -2=Padding, -1=Invalid
 */
static int b64_val(int c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	if (c == '=')
		return -2;

	return -1;
}
/**
 * @brief Compare two strings in constant time
 *
 * @param a Left string
 * @param b Right string
 *
 * @return 1=Equal, 0=Not equal
 */
static int ct_equal(const char* a, const char* b)
{
	size_t la = strlen(a);
	size_t lb = strlen(b);
	size_t n = (la > lb) ? la : lb;

	unsigned char diff = 0;

	for (size_t i = 0; i < n; i++) {
		unsigned char ca = (i < la) ? (unsigned char)a[i] : 0;
		unsigned char cb = (i < lb) ? (unsigned char)b[i] : 0;
		diff |= (unsigned char)(ca ^ cb);
	}

	return (diff == 0) && (la == lb);
}

/**
 * @brief Find user by name
 *
 * @param name Username
 *
 * @return User Ptr=Found, NULL=Not found
 */
static const struct user* find_user(const char* name)
{
	for (size_t i = 0; i < users.len; i++)
		if (strcmp(users.v[i].name, name) == 0)
			return &users.v[i];

	return NULL;
}

/**
 * @brief Check whether path matches allow prefix
 *
 * @param path Relative media path
 * @param pre Allow prefix
 *
 * @return 1=Match, 0=No match
 */
static int prefix_ok(const char* path, const char* pre)
{
	size_t n = strlen(pre);
	if (n == 0)
		return 0;

	if (strcmp(pre, "*") == 0)
		return 1;

	if (strncmp(path, pre, n) != 0)
		return 0;

	/* allow match or dir boundary */
	if (path[n] == '\0')
		return 1;
	if (pre[n - 1] == '/')
		return 1;
	if (path[n] == '/')
		return 1;

	return 0;
}

bool user_allows_path(const struct user* u, const char* relpath)
{
	if (!u || !relpath)
		return false;

	/* no allow list -> allow nothing */
	if (u->allow_len == 0)
		return false;

	for (size_t i = 0; i < u->allow_len; i++)
		if (prefix_ok(relpath, u->allow[i]))
			return true;

	return false;
}

int users_add_allow(const char* prefix)
{
	if (users.len == 0 || !prefix) {
		LOG_FIELD(true, "AUTH", "AddAllow FAILED", "No user or prefix");
		return -1;
	}

	LOG_FIELD(verbose_log, "AUTH", "Allow directories", "%s %s", users.v[users.len - 1].name, prefix);

	return allow_push(&users.v[users.len - 1], prefix);
}

const struct user* users_auth_from_hdr(const char* hdr)
{
	if (users.len == 0)
		return NULL;

	char auth[512];
	if (hdr_get_value(auth, hdr, "authorization") < 0) {
		LOG_FIELD(true, "AUTH", "Authorization", "Not found");
		return NULL;
	}

	/* given "Basic XXXX" */
	const char* p = auth;
	while (*p == ' ' || *p == '\t')
		p++;

	if (strncasecmp(p, "Basic", 5) != 0) {
		LOG_FIELD(verbose_log, "AUTH", "Authorization", "Not Basic");
		return NULL;
	}

	p += 5;
	while (*p == ' ' || *p == '\t')
		p++;

	if (*p == '\0') {
		LOG_FIELD(verbose_log, "AUTH", "Basic Auth", "Missing token");
		return NULL;
	}

	unsigned char dec[512];
	if (b64_decode(dec, sizeof(dec), p) < 0) {
		LOG_FIELD(verbose_log, "AUTH", "Basic Auth", "B64 decode failed");
		secure_bzero(dec, sizeof(dec));
		return NULL;
	}

	char* sep = strchr((char*)dec, ':');
	if (!sep) {
		LOG_FIELD(verbose_log, "AUTH", "Basic Auth", "Decoded but missing ':'");
		secure_bzero(dec, sizeof(dec));
		return NULL;
	}

	*sep = '\0';
	const char* user = (const char*)dec;
	const char* pass = (const char*)(sep + 1);

	const struct user* u = find_user(user);
	if (!u) {
		LOG(true, "AUTH", "Login FAILED Unknown user '%s'", user);
		secure_bzero(dec, sizeof(dec));
		return NULL;
	}

	/* empty password -> passwordless account */
	if (u->pass[0] == '\0') {
		if (pass[0] == '\0') {
			LOG_FIELD(true, "AUTH", "Login OK", "%s", user);
			secure_bzero(dec, sizeof(dec));
			return u;
		}
		LOG_FIELD(true, "AUTH", "Login FAILED", "%s", user);
		secure_bzero(dec, sizeof(dec));
		return NULL;
	}

	if (!ct_equal(u->pass, pass)) {
		LOG(true, "AUTH", "Login FAILED Bad password %s", user);
		secure_bzero(dec, sizeof(dec));
		return NULL;
	}

	LOG_FIELD(verbose_log, "AUTH", "Login OK", "%s", user);
	secure_bzero(dec, sizeof(dec));
	return u;
}

void users_free(void)
{
	for (size_t i = 0; i < users.len; i++) {
		for (size_t j = 0; j < users.v[i].allow_len; j++)
			free(users.v[i].allow[j]);
		free(users.v[i].allow);

		users.v[i].allow = NULL;
		users.v[i].allow_len = 0;
		users.v[i].allow_cap = 0;
	}

	free(users.v);
	users.v = NULL;
	users.len = 0;
	users.cap = 0;
}

int users_push(const char* name)
{
	if (!name || name[0] == '\0') {
		LOG_FIELD(true, "AUTH", "UserAdd FAILED", "Empty username");
		return -1;
	}

	if (users.len == users.cap) {
		size_t ncap = users.cap ? (users.cap * 2) : 8;
		struct user* nv = realloc(users.v, ncap * sizeof(*nv));
		if (!nv) {
			LOG_FIELD(true, "AUTH", "realloc FAILED", "(cap=%zu -> %zu)", users.cap, ncap);
			return -1;
		}
		users.v = nv;
		users.cap = ncap;
	}

	memset(&users.v[users.len], 0, sizeof(users.v[users.len]));
	snprintf(users.v[users.len].name, sizeof(users.v[users.len].name), "%s", name);

	LOG_FIELD(verbose_log, "AUTH", "UserAdd SUCCESS", "%s", users.v[users.len].name);

	users.len++;
	return 0;
}

int users_set_pass(const char* pass)
{
	if (users.len == 0) {
		LOG_FIELD(true, "AUTH", "SetPass FAILED", "No users");
		return -1;
	}

	if (!pass)
		pass = "";

	/* copy pass->user.pass */
	snprintf(users.v[users.len - 1].pass, sizeof(users.v[users.len - 1].pass), "%s", pass);

	LOG_FIELD(verbose_log, "AUTH", "Password set for", "%s (%s)",
		users.v[users.len - 1].name, (pass[0] == '\0') ? "none" : "set"
	);
	return 0;
}

