#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "json.h"
#include "log.h"

static const char* json_basename(const char* path);
static int json_grow(struct json* j, size_t need);
static int json_hex64(struct json* j, uint64_t v);
static const char* json_kind_from_type(const char* type);
static int json_putc(struct json* j, const char c);
static int json_putn(struct json* j, const char* s, size_t n);
static int json_puts(struct json* j, const char* s);
static int json_string(struct json* j, const char* s);

/**
 * @brief Extract basename from path
 *
 * @param path Input path
 *
 * @return Ptr to basename
 */
static const char* json_basename(const char* path)
{
	if (!path)
		return "";

	const char* p = strrchr(path, '/');
	return p ? (p + 1) : path;
}

/**
 * @brief Grow JSON buffer capacity
 *
 * @param j JSON buffer
 * @param need Additional bytes needed
 *
 * @return 0=Success, -1=Failure
 */
static int json_grow(struct json* j, size_t need)
{
	if (j->len + need <= j->cap)
		return 0;

	size_t ncap = j->cap ? j->cap : 1024; /* initial buffer */
	while (ncap < j->len + need)
		ncap *= 2;

	char* nb = realloc(j->buf, ncap);
	if (!nb) {
		LOG_FIELD(verbose_log, "JSON", "realloc FAILED", "OOM");
		return -1;
	}

	j->buf = nb;
	j->cap = ncap;

	return 0;
}

/**
 * @brief Encode uint64 as 16-digit hex string
 *
 * @param j Output JSON buffer
 * @param v Input value
 *
 * @return 0=Success, -1=Failure
 */
static int json_hex64(struct json* j, uint64_t v)
{
	char hex[17];
	static const char* b16 = "0123456789abcdef";

	for (int i = 15; i >= 0; i--) {
		hex[i] = b16[v & 0xf];
		v >>= 4;
	}
	hex[16] = '\0';

	return json_string(j, hex);
}

/**
 * @brief Map MIME type to item kind
 *
 * @param type MIME type string
 *
 * @return Item kind string
 */
static const char* json_kind_from_type(const char* type)
{
	if (!type || type[0] == '\0')
		return "other";

	if (strncmp(type, "audio/", 6) == 0)
		return "audio";
	if (strncmp(type, "video/", 6) == 0)
		return "video";
	if (strncmp(type, "image/", 6) == 0)
		return "image";

	return "other";
}

/**
 * @brief Append single byte to JSON buffer
 *
 * @param j Output JSON buffer
 * @param c Byte to append
 *
 * @return 0=Success, -1=Failure
 */
static int json_putc(struct json* j, char c)
{
	return json_putn(j, &c, 1);
}

/**
 * @brief Append byte range to JSON buffer
 *
 * @param j Output JSON buffer
 * @param s Input bytes
 * @param n Number of bytes to append
 *
 * @return 0=Success, -1=Failure
 */
static int json_putn(struct json* j, const char* s, size_t n)
{
	if (json_grow(j, n + 1) < 0)
		return -1;

	memcpy(j->buf + j->len, s, n);
	j->len += n;
	j->buf[j->len] = '\0';

	return 0;
}

/**
 * @brief Append string to JSON buffer
 *
 * @param j Output JSON buffer
 * @param s Input string
 *
 * @return 0=Success, -1=Failure
 */
static int json_puts(struct json* j, const char* s)
{
	return json_putn(j, s, strlen(s));
}

/**
 * @brief Append JSON string with escaping
 *
 * @param j Output JSON buffer
 * @param s Input string
 *
 * @return 0=Success, -1=Failure
 */
static int json_string(struct json* j, const char* s)
{
	/* opening quote */
	if (json_putc(j, '"') < 0)
		return -1;

	/* emit string body */
	for (; *s; s++) {
		unsigned char c = (unsigned char)(*s);

		/* escape quote and backslash */
		if (c == '"' || c == '\\') {
			if (json_putc(j, '\\') < 0)
				return -1;
			if (json_putc(j, (char)c) < 0)
				return -1;
			continue;
		}

		/* common escapes */
		if (c == '\n') {
			if (json_puts(j, "\\n") < 0)
				return -1;
			continue;
		}
		if (c == '\r') {
			if (json_puts(j, "\\r") < 0)
				return -1;
			continue;
		}
		if (c == '\t') {
			if (json_puts(j, "\\t") < 0)
				return -1;
			continue;
		}

		/* other control chars->\uxxxx */
		if (c < 0x20) {
			char esc[7];
			snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
			if (json_puts(j, esc) < 0)
				return -1;
			continue;
		}

		/* literal byte */
		if (json_putc(j, (char)c) < 0)
			return -1;
	}

	/* closing quote */
	if (json_putc(j, '"') < 0)
		return -1;

	return 0;
}

void json_free(struct json* j)
{
	if (!j)
		return;

	free(j->buf);
	j->buf = NULL;
	j->len = 0;
	j->cap = 0;
}

int json_library(struct json* j, const struct library* l)
{
	memset(j, 0, sizeof(*j));

	/* header */
	if (json_puts(j, "{\"proto\":1,\"items\":[") < 0)
		goto fail;

	/* items[] */
	for (size_t i = 0; i < l->len; i++) {
		/* item separator */
		if (i > 0) {
			if (json_putc(j, ',') < 0)
				goto fail;
		}

		/* object open */
		if (json_puts(j, "{\"id\":") < 0)
			goto fail;
		if (json_hex64(j, l->items[i].id) < 0)
			goto fail;

		/* path */
		if (json_puts(j, ",\"path\":") < 0)
			goto fail;
		if (json_string(j, l->items[i].path) < 0)
			goto fail;

		/* object close */
		if (json_putc(j, '}') < 0)
			goto fail;
	}

	/* footer */
	if (json_puts(j, "]}") < 0)
		goto fail;

	return 0;

fail:
	json_free(j);
	return -1;
}

int json_meta(struct json* j, const struct item* it, size_t size, long mtime, const char* type)
{
	memset(j, 0, sizeof(*j));

	if (json_puts(j, "{\"proto\":1,\"id\":") < 0)
		goto fail;
	if (json_hex64(j, it->id) < 0)
		goto fail;

	if (json_puts(j, ",\"path\":") < 0)
		goto fail;
	if (json_string(j, it->path) < 0)
		goto fail;

	/* basename */
	if (json_puts(j, ",\"name\":") < 0)
		goto fail;
	if (json_string(j, json_basename(it->path)) < 0)
		goto fail;

	if (json_puts(j, ",\"size\":") < 0)
		goto fail;

	/* decimal size */
	char tmp[32];
	snprintf(tmp, sizeof(tmp), "%zu", size);
	if (json_puts(j, tmp) < 0)
		goto fail;


	/* mtime (epoch seconds) */
	if (json_puts(j, ",\"mtime\":") < 0)
		goto fail;
	snprintf(tmp, sizeof(tmp), "%ld", mtime);
	if (json_puts(j, tmp) < 0)
		goto fail;

	/* formats */
	if (json_puts(j, ",\"type\":") < 0)
		goto fail;
	if (json_string(j, type) < 0)
		goto fail;

	if (json_puts(j, ",\"kind\":") < 0)
		goto fail;
	if (json_string(j, json_kind_from_type(type)) < 0)
		goto fail;

	if (json_putc(j, '}') < 0)
		goto fail;

	return 0;

fail:
	json_free(j);
	return -1;
}

