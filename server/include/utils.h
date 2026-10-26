#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>

/**
 * @brief Case-insensitive substring search
 *
 * @param hay Haystack
 * @param nee Needle
 *
 * @return Ptr to first match in hay, NULL=Not found
 */
const char* cistrstr(const char* hay, const char* nee);

/**
 * @brief Extract header value from raw HTTP request header block
 *
 * @param out Output buffer (NULL terminated on success)
 * @param hdr Raw HTTP request header block
 * @param key Header key to search for (case-insensitive)
 *
 * @return 0=Success, -1=Not found / Malformed
 */
int hdr_get_value(char out[512], const char* hdr, const char* key);

/**
 * @brief Join two path fragments
 *
 * @param out Output path buffer
 * @param outsz Output buffer size
 * @param a Left path
 * @param b Right path
 *
 * @return 0=Success, -1=Overflow / Invalid input
 */
int join_path(char* out, size_t outsz, const char* a, const char* b);

/**
 * @brief Securely zero memory
 *
 * @note Prevents compiler from optimising the wipe away
 *
 * @param p Pointer to memory to wipe.
 * @param n Number of bytes to wipe.
 */
void secure_bzero(void* p, size_t n);

/**
 * @brief Write exactly n bytes to a file descriptor
 *
 * @note Retries on EINTR.
 *
 * @param fd File descriptor
 * @param buf Input buffer
 * @param n Number of bytes to write
 *
 * @return 0=Success, -1=Failure
 */
int write_all(int fd, const void* buf, size_t n);

#endif /* UTILS_H */

