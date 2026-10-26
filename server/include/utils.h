#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

/* Case-insensitive substring search
   Returns Ptr to first match in hay, NULL=Not found */
const char* cistrstr(const char* hay, const char* nee);

/* Extract header value from raw HTTP request header block
   Returns 0=Success, -1=Not found / Malformed */
int hdr_get_value(char out[512], const char* hdr, const char* key);

/* Join two path fragments
   Returns 0=Success, -1=Overflow / Invalid input */
int join_path(char* out, size_t outsz, const char* a, const char* b);

/* Securely zero memory
   Prevents compiler from optimising the wipe away */
void secure_bzero(void* p, size_t n);

/* Write exactly n bytes to a file descriptor
   Retries on EINTR.
   Returns 0=Success, -1=Failure */
int write_all(int fd, const void* buf, size_t n);

#endif /* UTILS_H */

