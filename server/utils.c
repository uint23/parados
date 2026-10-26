#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "utils.h"

static int ci_key_match(const char* a, const char* b, size_t n);

/**
 * @brief Compare two header keys case-insensitively
 *
 * @param a Left string
 * @param b Right string
 * @param n Number of bytes to compare
 *
 * @return 1=Match, 0=No match
 */
static int ci_key_match(const char* a, const char* b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char ca = (unsigned char)a[i];
		unsigned char cb = (unsigned char)b[i];
		if (tolower(ca) != tolower(cb))
			return 0;
	}
	return 1;
}

const char* cistrstr(const char* hay, const char* nee)
{
	size_t nl = strlen(nee);
	if (nl == 0)
		return hay;

	for (; *hay; hay++) {
		size_t i = 0;

		for (;;) {
			if (i == nl)
				return hay;

			unsigned char a = (unsigned char)hay[i];
			unsigned char b = (unsigned char)nee[i];

			if (a == '\0')
				break;

			if (tolower(a) != tolower(b))
				break;

			i++;
		}
	}

	return NULL;
}

int hdr_get_value(char out[512], const char* hdr, const char* key)
{
	size_t klen = strlen(key);
	if (!hdr || !key || klen == 0)
		return -1;

	const char* p = hdr;

	/* skip request line */
	const char* eol = strstr(p, "\r\n");
	if (!eol)
		return -1;
	p = eol + 2;

	for (;;) {
		if (p[0] == '\r' && p[1] == '\n')
			break; /* end of headers */

		const char* line_end = strstr(p, "\r\n");
		if (!line_end)
			break;

		const char* colon = memchr(p, ':', (size_t)(line_end - p));
		if (colon) {
			size_t n = (size_t)(colon - p);

			if (n == klen && ci_key_match(p, key, klen)) {
				const char* v = colon + 1;
				while (*v == ' ' || *v == '\t')
					v++;

				size_t outn = 0;
				while (v < line_end) {
					if (outn + 1 >= 512)
						return -1;
					out[outn++] = *v++;
				}
				out[outn] = '\0';

				if (outn == 0)
					return -1;

				return 0;
			}
		}

		p = line_end + 2;
	}

	return -1;
}

int join_path(char* out, size_t outsz, const char* a, const char* b)
{
	if (!out || outsz == 0 || !a || !b)
		return -1;

	if (a[0] == '\0') {
		int n = snprintf(out, outsz, "%s", b);
		if (n < 0 || (size_t)n >= outsz)
			return -1;
		return 0;
	}

	if (b[0] == '\0') {
		int n = snprintf(out, outsz, "%s", a);
		if (n < 0 || (size_t)n >= outsz)
			return -1;
		return 0;
	}

	size_t al = strlen(a);

	/* avoid double slashes */
	if (a[al - 1] == '/')
		return snprintf(out, outsz, "%s%s", a, b) < (int)outsz ? 0 : -1;

	return snprintf(out, outsz, "%s/%s", a, b) < (int)outsz ? 0 : -1;
}

void secure_bzero(void* p, size_t n)
{
	if (!p || n == 0)
		return;

#if defined(__OpenBSD__)
	explicit_bzero(p, n);
#else
	volatile unsigned char* vp = (volatile unsigned char*)p;
	while (n--)
		*vp++ = 0;
#endif
}

int write_all(int fd, const void* buf, size_t n)
{
	const char* p = buf;

	while (n > 0) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		p += (size_t)w;
		n -= (size_t)w;
	}

	return 0;
}
