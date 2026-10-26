#ifndef HTTP_H
#define HTTP_H

#define HTTP_200    "HTTP/1.1 200 OK\r\n"
#define HTTP_204    "HTTP/1.1 204 No Content\r\n"
#define HTTP_206    "HTTP/1.1 206 Partial Content\r\n"
#define HTTP_400    "HTTP/1.1 400 Bad Request\r\n"
#define HTTP_403    "HTTP/1.1 403 Forbidden\r\n"
#define HTTP_404    "HTTP/1.1 404 Not Found\r\n"
#define HTTP_405    "HTTP/1.1 405 Method Not Allowed\r\n"
#define HTTP_416    "HTTP/1.1 416 Range Not Satisfiable\r\n"
#define HTTP_500    "HTTP/1.1 500 Internal Server Error\r\n"

#define HTTP_BIN    "Content-Type: application/octet-stream\r\n"
#define HTTP_CLOSE  "Connection: close\r\n"
#define HTTP_CRG    "Content-Range: bytes %zu-%zu/%zu\r\n"
#define HTTP_CTYPE  "Content-Type: %s\r\n"
#define HTTP_JSON   "Content-Type: application/json\r\n"
#define HTTP_LENGTH "Content-Length: %zu\r\n"
#define HTTP_RANGE  "Accept-Ranges: bytes\r\n"
#define HTTP_TEXT   "Content-Type: text/plain\r\n"

enum {
	RANGE_NONE  = 0,   /* no range header */
	RANGE_OK    = 1,   /* valid satisfiable range */
	RANGE_BAD   = -1,  /* malformed (=> 400) */
	RANGE_UNSAT = -2,  /* unsatisfiable (=> 416) */
};

enum {
	HTTP_REQ_MAX   = 8192,
	HTTP_RESP_MAX  = 8192,
	LISTEN_BACKLOG = 64,
};

/* Handle a single HTTP request connected socket
   This does not close the socket
   Returns 0=Handled (includes sending error responses), -1=Internal Server Error */
int http_handle(int c);

#endif /* HTTP_H */

