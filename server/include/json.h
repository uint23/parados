#ifndef JSON_H
#define JSON_H

#include <stddef.h>
#include <stdint.h>

#include "scan.h"

struct json {
	char*          buf;
	size_t         len;
	size_t         cap;
};

/* Free memory owned by JSON buffer
   Safe to call on NULL or already freed buffers */
void json_free(struct json* j);

/* Encode library to JSON
   On failure, j is freed/emptied
   Returns 0=Success, -1=Failure */
int json_library(struct json* j, const struct library* l);

/* Encode item metadata to JSON
   On failure, j is freed/emptied
   Returns 0=Success, -1=Failure */
int json_meta(struct json* j, const struct item* it, size_t size, long mtime, const char* type);

#endif /* JSON_H */

