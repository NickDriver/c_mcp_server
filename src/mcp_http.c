/* HTTP transport for MCP, covering two wire profiles for maximum
 * client compatibility:
 *
 *   1. Streamable HTTP (MCP 2025-03-26+):
 *        POST /mcp   — JSON body in; JSON or SSE out (by Accept header).
 *        GET  /mcp   — opens a long-lived SSE stream for server→client
 *                      messages. We never push unsolicited messages, so the
 *                      stream only carries keepalive comments; it exists so
 *                      strict clients that open the stream at startup can.
 *        OPTIONS /mcp — CORS preflight.
 *
 *   2. Legacy HTTP+SSE (MCP 2024-11-05):
 *        GET  /sse            — opens SSE stream; first event is `endpoint`
 *                               pointing at /message?sessionId=<id>.
 *        POST /message?sessionId=<id>
 *                             — JSON-RPC request; 202 Accepted is returned
 *                               immediately, and the response is pushed as
 *                               an SSE `message` event on the matching
 *                               /sse stream.
 *
 * Concurrency: one detached pthread per TCP connection. A global mutex
 * serializes calls into mcp_handle_message so the single-threaded server
 * core stays consistent. SSE streams use a per-session write mutex so POST
 * handlers in other threads can push events without interleaving writes.
 *
 * Not implemented: chunked transfer encoding, keep-alive (each response has
 * Connection: close), Last-Event-ID resume. */

#define _POSIX_C_SOURCE 200809L

#include "mcp_internal.h"

#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define HTTP_MAX_HEADER     (8U * 1024U)
#define HTTP_MAX_BODY       (1U * 1024U * 1024U)
#define HTTP_LISTEN_BACKLOG 32
#define SSE_KEEPALIVE_SECS  15

/* ------------------------------------------------------------------ */
/*  Signals + global state                                            */
/* ------------------------------------------------------------------ */

static volatile sig_atomic_t g_shutdown = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_shutdown = 1;
}

/* Serializes mcp_handle_message across connection threads. The SDK core
 * is not thread-safe; a coarse mutex is plenty at this scale. */
static pthread_mutex_t g_dispatch_lock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ */
/*  Legacy HTTP+SSE session table                                     */
/* ------------------------------------------------------------------ */

typedef struct sse_session {
    char                id[33];      /* 32 hex + NUL */
    int                 fd;          /* socket for the SSE stream */
    pthread_mutex_t     write_lock;
    atomic_int          refcount;    /* 1 owned by the SSE thread + acquirers */
    atomic_bool         closed;
    struct sse_session *next;
} sse_session_t;

static pthread_mutex_t g_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
static sse_session_t  *g_sessions_head = NULL;

/* Fills out[33] with 32 hex chars + NUL. Returns 0 on success. */
static int gen_session_id(char out[33])
{
    unsigned char raw[16];
    FILE *f = fopen("/dev/urandom", "r");
    if (f == NULL) {
        return -1;
    }
    size_t got = fread(raw, 1, sizeof raw, f);
    (void)fclose(f);
    if (got != sizeof raw) {
        return -1;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof raw; ++i) {
        out[i * 2]     = hex[(raw[i] >> 4) & 0x0f];
        out[i * 2 + 1] = hex[raw[i] & 0x0f];
    }
    out[32] = '\0';
    return 0;
}

static sse_session_t *session_create(int fd)
{
    sse_session_t *s = calloc(1, sizeof *s);
    if (s == NULL) {
        return NULL;
    }
    if (gen_session_id(s->id) != 0) {
        free(s);
        return NULL;
    }
    s->fd = fd;
    if (pthread_mutex_init(&s->write_lock, NULL) != 0) {
        free(s);
        return NULL;
    }
    atomic_store(&s->refcount, 1);
    atomic_store(&s->closed, false);

    pthread_mutex_lock(&g_sessions_lock);
    s->next = g_sessions_head;
    g_sessions_head = s;
    pthread_mutex_unlock(&g_sessions_lock);
    return s;
}

static sse_session_t *session_acquire(const char *id)
{
    pthread_mutex_lock(&g_sessions_lock);
    sse_session_t *s = g_sessions_head;
    while (s != NULL && strcmp(s->id, id) != 0) {
        s = s->next;
    }
    if (s != NULL) {
        atomic_fetch_add(&s->refcount, 1);
    }
    pthread_mutex_unlock(&g_sessions_lock);
    return s;
}

static void session_release(sse_session_t *s)
{
    if (atomic_fetch_sub(&s->refcount, 1) == 1) {
        pthread_mutex_destroy(&s->write_lock);
        free(s);
    }
}

static void session_unlink(sse_session_t *s)
{
    pthread_mutex_lock(&g_sessions_lock);
    sse_session_t **pp = &g_sessions_head;
    while (*pp != NULL) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_sessions_lock);
}

/* ------------------------------------------------------------------ */
/*  Socket I/O helpers                                                */
/* ------------------------------------------------------------------ */

static ssize_t read_full(int fd, char *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static int write_full(int fd, const char *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

static ssize_t read_headers(int fd, char *buf, size_t cap, size_t *hdr_end_out)
{
    size_t got = 0;
    size_t scan_from = 0;
    while (got < cap) {
        ssize_t r = recv(fd, buf + got, cap - got, 0);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (r == 0) {
            return -1;
        }
        got += (size_t)r;

        size_t start = (scan_from >= 3) ? scan_from - 3 : 0;
        if (got >= 4) {
            for (size_t i = start; i + 3 < got; ++i) {
                if (buf[i] == '\r' && buf[i + 1] == '\n' &&
                    buf[i + 2] == '\r' && buf[i + 3] == '\n') {
                    *hdr_end_out = i + 4;
                    return (ssize_t)got;
                }
            }
        }
        scan_from = got;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Small string utilities                                            */
/* ------------------------------------------------------------------ */

static bool ci_startswith(const char *hay, const char *needle)
{
    while (*needle != '\0') {
        char a = (char)tolower((unsigned char)*hay);
        char b = (char)tolower((unsigned char)*needle);
        if (a != b) {
            return false;
        }
        ++hay;
        ++needle;
    }
    return true;
}

/* Case-insensitive substring search into a fixed-size buffer. Copies
 * `haystack` lowercased, then uses strstr. Returns true if found. */
static bool contains_ci(const char *haystack, const char *needle)
{
    if (haystack == NULL) {
        return false;
    }
    char  buf[512];
    size_t n = strlen(haystack);
    if (n >= sizeof buf) {
        n = sizeof buf - 1;
    }
    for (size_t i = 0; i < n; ++i) {
        buf[i] = (char)tolower((unsigned char)haystack[i]);
    }
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* Case-insensitive: does `accept` prefer text/event-stream over application/json?
 * Rule: first match wins; SSE-only ⇒ true; JSON-only/missing ⇒ false. */
static bool accept_prefers_sse(const char *accept)
{
    if (accept == NULL || accept[0] == '\0') {
        return false;
    }
    char  buf[512];
    size_t n = strlen(accept);
    if (n >= sizeof buf) {
        n = sizeof buf - 1;
    }
    for (size_t i = 0; i < n; ++i) {
        buf[i] = (char)tolower((unsigned char)accept[i]);
    }
    buf[n] = '\0';
    const char *sse  = strstr(buf, "text/event-stream");
    const char *json = strstr(buf, "application/json");
    if (sse != NULL && json == NULL) {
        return true;
    }
    if (sse != NULL && json != NULL) {
        return sse < json;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  HTTP response helpers                                             */
/* ------------------------------------------------------------------ */

static int send_response(int fd, int status, const char *reason,
                         const char *content_type,
                         const char *body, size_t body_len)
{
    char head[512];
    int  hl = snprintf(head, sizeof head,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: POST, GET, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Accept, Mcp-Session-Id, MCP-Protocol-Version\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason, content_type, body_len);
    if (hl < 0 || (size_t)hl >= sizeof head) {
        return -1;
    }
    if (write_full(fd, head, (size_t)hl) < 0) {
        return -1;
    }
    if (body_len > 0 && write_full(fd, body, body_len) < 0) {
        return -1;
    }
    return 0;
}

static int send_json_error(int fd, int status, const char *reason, const char *json_body)
{
    return send_response(fd, status, reason, "application/json",
                         json_body, strlen(json_body));
}

/* Send SSE response headers. No Content-Length; stream terminates on close. */
static int send_sse_headers(int fd, const char *session_id /* may be NULL */)
{
    char head[512];
    int  hl = snprintf(head, sizeof head,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Expose-Headers: Mcp-Session-Id\r\n"
        "%s%s%s"
        "\r\n",
        session_id ? "Mcp-Session-Id: " : "",
        session_id ? session_id         : "",
        session_id ? "\r\n"             : "");
    if (hl < 0 || (size_t)hl >= sizeof head) {
        return -1;
    }
    return write_full(fd, head, (size_t)hl);
}

/* Write one SSE event: `event: <name>\ndata: <data>\n\n`.
 * If name is NULL, the `event:` line is omitted (default event type).
 * Caller must hold the session's write_lock if shared. */
static int sse_write_event_locked(int fd, const char *name, const char *data)
{
    if (name != NULL) {
        if (write_full(fd, "event: ", 7) < 0) {
            return -1;
        }
        if (write_full(fd, name, strlen(name)) < 0) {
            return -1;
        }
        if (write_full(fd, "\n", 1) < 0) {
            return -1;
        }
    }
    if (write_full(fd, "data: ", 6) < 0) {
        return -1;
    }
    if (write_full(fd, data, strlen(data)) < 0) {
        return -1;
    }
    return write_full(fd, "\n\n", 2);
}

/* ------------------------------------------------------------------ */
/*  Dispatch                                                          */
/* ------------------------------------------------------------------ */

static cJSON *dispatch_locked(mcp_server_t *srv, const cJSON *msg)
{
    pthread_mutex_lock(&g_dispatch_lock);
    cJSON *resp = mcp_handle_message(srv, msg);
    pthread_mutex_unlock(&g_dispatch_lock);
    return resp;
}

/* ------------------------------------------------------------------ */
/*  Landing page (GET /)                                              */
/* ------------------------------------------------------------------ */

static int handle_root(int fd)
{
    static const char body[] =
        "<!doctype html><meta charset=utf-8><title>MCP server</title>"
        "<h1>MCP HTTP server</h1>"
        "<p>Streamable HTTP: <code>POST /mcp</code> (JSON or SSE).</p>"
        "<p>Legacy SSE: <code>GET /sse</code> + <code>POST /message?sessionId=...</code>.</p>";
    return send_response(fd, 200, "OK", "text/html; charset=utf-8",
                         body, sizeof body - 1);
}

/* ------------------------------------------------------------------ */
/*  Streamable HTTP: GET /mcp  (long-lived, keepalive-only SSE)       */
/* ------------------------------------------------------------------ */

static void handle_mcp_get_stream(int fd)
{
    if (send_sse_headers(fd, NULL) < 0) {
        return;
    }
    /* Initial comment so the client immediately sees the stream is up. */
    if (write_full(fd, ": open\n\n", 8) < 0) {
        return;
    }

    /* Hold the connection; send keepalives periodically. Detect disconnect
     * via recv() returning 0 or error. */
    while (g_shutdown == 0) {
        struct timeval tv = {.tv_sec = SSE_KEEPALIVE_SECS, .tv_usec = 0};
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char tmp[128];
        ssize_t r = recv(fd, tmp, sizeof tmp, 0);
        if (r == 0) {
            return; /* peer closed */
        }
        if (r > 0) {
            /* Client shouldn't be sending data on this stream; ignore. */
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            if (write_full(fd, ": keepalive\n\n", 13) < 0) {
                return;
            }
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/*  Legacy HTTP+SSE: GET /sse                                         */
/* ------------------------------------------------------------------ */

static void handle_legacy_sse_get(int fd)
{
    sse_session_t *s = session_create(fd);
    if (s == NULL) {
        (void)send_json_error(fd, 500, "Internal Server Error",
                              "{\"error\":\"failed to create session\"}");
        return;
    }

    bool failed = false;
    pthread_mutex_lock(&s->write_lock);
    if (send_sse_headers(fd, s->id) < 0) {
        failed = true;
    } else {
        /* Per the old spec, first event is `endpoint` with a URL the client
         * should POST future JSON-RPC requests to. */
        char ep[128];
        int  n = snprintf(ep, sizeof ep, "/message?sessionId=%s", s->id);
        assert(n > 0 && (size_t)n < sizeof ep);
        if (sse_write_event_locked(fd, "endpoint", ep) < 0) {
            failed = true;
        }
    }
    pthread_mutex_unlock(&s->write_lock);

    if (!failed) {
        while (g_shutdown == 0 && !atomic_load(&s->closed)) {
            struct timeval tv = {.tv_sec = SSE_KEEPALIVE_SECS, .tv_usec = 0};
            (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            char tmp[128];
            ssize_t r = recv(fd, tmp, sizeof tmp, 0);
            if (r == 0) {
                break;
            }
            if (r > 0) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                pthread_mutex_lock(&s->write_lock);
                int wrc = write_full(fd, ": keepalive\n\n", 13);
                pthread_mutex_unlock(&s->write_lock);
                if (wrc < 0) {
                    break;
                }
                continue;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
    }

    atomic_store(&s->closed, true);
    session_unlink(s);
    session_release(s);
}

/* ------------------------------------------------------------------ */
/*  Legacy HTTP+SSE: POST /message?sessionId=<id>                     */
/* ------------------------------------------------------------------ */

static bool extract_session_id(const char *path, char out[33])
{
    const char *q = strchr(path, '?');
    if (q == NULL) {
        return false;
    }
    /* Scan querystring for "sessionId=" (case-sensitive — matches what we emit). */
    const char *p = q + 1;
    while (*p != '\0') {
        if (strncmp(p, "sessionId=", 10) == 0) {
            p += 10;
            size_t i = 0;
            while (i < 32 && p[i] != '\0' && p[i] != '&') {
                out[i] = p[i];
                ++i;
            }
            out[i] = '\0';
            return i > 0;
        }
        const char *amp = strchr(p, '&');
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return false;
}

static void handle_legacy_post_message(mcp_server_t *srv, int fd,
                                       const char *path,
                                       const char *body, size_t body_len)
{
    char sid[33];
    if (!extract_session_id(path, sid)) {
        (void)send_json_error(fd, 400, "Bad Request",
                              "{\"error\":\"missing sessionId query parameter\"}");
        return;
    }

    sse_session_t *s = session_acquire(sid);
    if (s == NULL) {
        (void)send_json_error(fd, 404, "Not Found",
                              "{\"error\":\"session not found\"}");
        return;
    }

    cJSON *msg = cJSON_ParseWithLength(body, body_len);
    if (msg == NULL) {
        session_release(s);
        (void)send_json_error(fd, 400, "Bad Request",
            "{\"jsonrpc\":\"2.0\",\"id\":null,"
            "\"error\":{\"code\":-32700,\"message\":\"parse error\"}}");
        return;
    }

    cJSON *resp = dispatch_locked(srv, msg);
    cJSON_Delete(msg);

    /* Acknowledge the POST immediately. The RPC response (if any) travels
     * via the SSE stream. */
    (void)send_response(fd, 202, "Accepted", "application/json", "", 0);

    if (resp != NULL) {
        char *txt = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (txt != NULL) {
            pthread_mutex_lock(&s->write_lock);
            if (!atomic_load(&s->closed)) {
                (void)sse_write_event_locked(s->fd, "message", txt);
            }
            pthread_mutex_unlock(&s->write_lock);
            free(txt);
        }
    }

    session_release(s);
}

/* ------------------------------------------------------------------ */
/*  Streamable HTTP: POST /mcp                                        */
/* ------------------------------------------------------------------ */

static void handle_mcp_post(mcp_server_t *srv, int fd,
                            const char *accept,
                            const char *body, size_t body_len)
{
    cJSON *msg = cJSON_ParseWithLength(body, body_len);
    if (msg == NULL) {
        (void)send_json_error(fd, 400, "Bad Request",
            "{\"jsonrpc\":\"2.0\",\"id\":null,"
            "\"error\":{\"code\":-32700,\"message\":\"parse error\"}}");
        return;
    }

    cJSON *resp = dispatch_locked(srv, msg);
    cJSON_Delete(msg);

    if (resp == NULL) {
        /* Notification — per Streamable HTTP spec, 202 Accepted + empty body. */
        (void)send_response(fd, 202, "Accepted", "application/json", "", 0);
        return;
    }

    char *txt = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (txt == NULL) {
        (void)send_json_error(fd, 500, "Internal Server Error",
                              "{\"error\":\"response serialization failed\"}");
        return;
    }

    if (accept_prefers_sse(accept)) {
        /* Single-event SSE response: one `message` event then close. */
        if (send_sse_headers(fd, NULL) == 0) {
            (void)sse_write_event_locked(fd, "message", txt);
        }
    } else {
        (void)send_response(fd, 200, "OK", "application/json",
                            txt, strlen(txt));
    }
    free(txt);
}

/* ------------------------------------------------------------------ */
/*  Request router                                                    */
/* ------------------------------------------------------------------ */

static void handle_connection(mcp_server_t *srv, int fd)
{
    char   hdr[HTTP_MAX_HEADER];
    size_t hdr_end = 0;
    ssize_t got = read_headers(fd, hdr, sizeof hdr, &hdr_end);
    if (got < 0) {
        return;
    }

    /* Parse request line: METHOD SP PATH SP HTTP/VER CRLF */
    char *line_cr = memchr(hdr, '\r', (size_t)got);
    if (line_cr == NULL || line_cr + 1 >= hdr + got || line_cr[1] != '\n') {
        return;
    }
    *line_cr = '\0';

    char method[16] = {0};
    char path[1024] = {0};
    {
        char *sp1 = strchr(hdr, ' ');
        if (sp1 == NULL) {
            return;
        }
        size_t mlen = (size_t)(sp1 - hdr);
        if (mlen == 0 || mlen >= sizeof method) {
            return;
        }
        memcpy(method, hdr, mlen);

        char *sp2 = strchr(sp1 + 1, ' ');
        if (sp2 == NULL) {
            return;
        }
        size_t plen = (size_t)(sp2 - (sp1 + 1));
        if (plen == 0 || plen >= sizeof path) {
            return;
        }
        memcpy(path, sp1 + 1, plen);
    }

    /* Parse headers we care about: Content-Length and Accept. */
    size_t content_length = 0;
    bool   have_length    = false;
    char   accept[256]    = {0};

    char *p        = line_cr + 2;
    char *hdr_stop = hdr + hdr_end;
    while (p < hdr_stop) {
        char *eol = memchr(p, '\r', (size_t)(hdr_stop - p));
        if (eol == NULL || eol + 1 >= hdr_stop || eol[1] != '\n') {
            break;
        }
        if (eol == p) {
            break;
        }
        *eol = '\0';

        if (ci_startswith(p, "content-length:")) {
            const char *v = p + strlen("content-length:");
            while (*v == ' ' || *v == '\t') {
                ++v;
            }
            char *endp = NULL;
            unsigned long n = strtoul(v, &endp, 10);
            if (endp != v) {
                content_length = (size_t)n;
                have_length    = true;
            }
        } else if (ci_startswith(p, "accept:")) {
            const char *v = p + strlen("accept:");
            while (*v == ' ' || *v == '\t') {
                ++v;
            }
            size_t vlen = strlen(v);
            if (vlen >= sizeof accept) {
                vlen = sizeof accept - 1;
            }
            memcpy(accept, v, vlen);
            accept[vlen] = '\0';
        }

        p = eol + 2;
    }

    /* Path matching: split on '?' for route comparison. */
    char  path_only[1024];
    {
        size_t plen = strlen(path);
        const char *q = memchr(path, '?', plen);
        size_t pn = q ? (size_t)(q - path) : plen;
        if (pn >= sizeof path_only) {
            pn = sizeof path_only - 1;
        }
        memcpy(path_only, path, pn);
        path_only[pn] = '\0';
    }

    /* Universal CORS preflight. */
    if (strcmp(method, "OPTIONS") == 0) {
        (void)send_response(fd, 204, "No Content", "text/plain", "", 0);
        return;
    }

    /* Landing page. */
    if (strcmp(method, "GET") == 0 && strcmp(path_only, "/") == 0) {
        (void)handle_root(fd);
        return;
    }

    /* --- Streamable HTTP endpoint --- */
    if (strcmp(path_only, "/mcp") == 0) {
        if (strcmp(method, "GET") == 0) {
            /* Only open the stream if the client actually asks for SSE. */
            if (!contains_ci(accept, "text/event-stream")) {
                (void)send_json_error(fd, 405, "Method Not Allowed",
                    "{\"error\":\"GET /mcp requires Accept: text/event-stream\"}");
                return;
            }
            handle_mcp_get_stream(fd);
            return;
        }
        if (strcmp(method, "POST") != 0) {
            (void)send_json_error(fd, 405, "Method Not Allowed",
                                  "{\"error\":\"method not allowed\"}");
            return;
        }
        if (!have_length || content_length == 0 || content_length > HTTP_MAX_BODY) {
            (void)send_json_error(fd, 400, "Bad Request",
                "{\"error\":\"missing/invalid Content-Length (max 1MiB)\"}");
            return;
        }

        size_t already = (size_t)got - hdr_end;
        if (already > content_length) {
            already = content_length;
        }
        char *body = malloc(content_length + 1);
        if (body == NULL) {
            (void)send_json_error(fd, 500, "Internal Server Error",
                                  "{\"error\":\"out of memory\"}");
            return;
        }
        if (already > 0) {
            memcpy(body, hdr + hdr_end, already);
        }
        if (already < content_length) {
            if (read_full(fd, body + already, content_length - already) < 0) {
                free(body);
                return;
            }
        }
        body[content_length] = '\0';

        handle_mcp_post(srv, fd, accept, body, content_length);
        free(body);
        return;
    }

    /* --- Legacy HTTP+SSE --- */
    if (strcmp(path_only, "/sse") == 0) {
        if (strcmp(method, "GET") != 0) {
            (void)send_json_error(fd, 405, "Method Not Allowed",
                                  "{\"error\":\"use GET for /sse\"}");
            return;
        }
        handle_legacy_sse_get(fd);
        return;
    }

    if (strcmp(path_only, "/message") == 0) {
        if (strcmp(method, "POST") != 0) {
            (void)send_json_error(fd, 405, "Method Not Allowed",
                                  "{\"error\":\"use POST for /message\"}");
            return;
        }
        if (!have_length || content_length == 0 || content_length > HTTP_MAX_BODY) {
            (void)send_json_error(fd, 400, "Bad Request",
                "{\"error\":\"missing/invalid Content-Length (max 1MiB)\"}");
            return;
        }

        size_t already = (size_t)got - hdr_end;
        if (already > content_length) {
            already = content_length;
        }
        char *body = malloc(content_length + 1);
        if (body == NULL) {
            (void)send_json_error(fd, 500, "Internal Server Error",
                                  "{\"error\":\"out of memory\"}");
            return;
        }
        if (already > 0) {
            memcpy(body, hdr + hdr_end, already);
        }
        if (already < content_length) {
            if (read_full(fd, body + already, content_length - already) < 0) {
                free(body);
                return;
            }
        }
        body[content_length] = '\0';

        handle_legacy_post_message(srv, fd, path, body, content_length);
        free(body);
        return;
    }

    (void)send_json_error(fd, 404, "Not Found",
                          "{\"error\":\"not found; try /mcp or /sse\"}");
}

/* ------------------------------------------------------------------ */
/*  Per-connection thread                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    mcp_server_t *srv;
    int           fd;
} conn_arg_t;

static void *conn_thread(void *arg)
{
    conn_arg_t *ca = (conn_arg_t *)arg;

    /* Disable Nagle for snappier small responses — matters for SSE. */
    int one = 1;
    (void)setsockopt(ca->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    handle_connection(ca->srv, ca->fd);
    (void)shutdown(ca->fd, SHUT_RDWR);
    (void)close(ca->fd);
    free(ca);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Public entry point                                                */
/* ------------------------------------------------------------------ */

int mcp_server_run_http(mcp_server_t *srv, const char *host, unsigned short port)
{
    assert(srv != NULL);

    struct sigaction sa_ign = {.sa_handler = SIG_IGN};
    (void)sigaction(SIGPIPE, &sa_ign, NULL);

    struct sigaction sa_stop = {.sa_handler = on_signal};
    (void)sigaction(SIGINT,  &sa_stop, NULL);
    (void)sigaction(SIGTERM, &sa_stop, NULL);

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        MCP_LOG_ERROR("socket: %s", strerror(errno));
        return MCP_ERR_IO;
    }

    int one = 1;
    (void)setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (host == NULL || host[0] == '\0' || strcmp(host, "0.0.0.0") == 0) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            MCP_LOG_ERROR("invalid host: %s", host);
            (void)close(sock);
            return MCP_ERR_INVALID_ARG;
        }
    }

    if (bind(sock, (const struct sockaddr *)&addr, sizeof addr) < 0) {
        MCP_LOG_ERROR("bind %s:%u: %s",
                      (host && *host) ? host : "0.0.0.0",
                      (unsigned)port, strerror(errno));
        (void)close(sock);
        return MCP_ERR_IO;
    }
    if (listen(sock, HTTP_LISTEN_BACKLOG) < 0) {
        MCP_LOG_ERROR("listen: %s", strerror(errno));
        (void)close(sock);
        return MCP_ERR_IO;
    }

    MCP_LOG_INFO("HTTP MCP server listening on http://%s:%u "
                 "(POST /mcp, GET /sse, POST /message)",
                 (host && *host) ? host : "0.0.0.0", (unsigned)port);

    while (g_shutdown == 0) {
        struct sockaddr_in caddr;
        socklen_t          clen = sizeof caddr;
        int cfd = accept(sock, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            MCP_LOG_WARN("accept: %s", strerror(errno));
            continue;
        }

        conn_arg_t *ca = malloc(sizeof *ca);
        if (ca == NULL) {
            (void)close(cfd);
            continue;
        }
        ca->srv = srv;
        ca->fd  = cfd;

        pthread_t tid;
        int trc = pthread_create(&tid, NULL, conn_thread, ca);
        if (trc != 0) {
            MCP_LOG_WARN("pthread_create: %s", strerror(trc));
            (void)close(cfd);
            free(ca);
            continue;
        }
        (void)pthread_detach(tid);
    }

    (void)close(sock);

    /* Best-effort: signal any live SSE streams so they break their loops. */
    pthread_mutex_lock(&g_sessions_lock);
    for (sse_session_t *s = g_sessions_head; s != NULL; s = s->next) {
        atomic_store(&s->closed, true);
        (void)shutdown(s->fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&g_sessions_lock);

    MCP_LOG_INFO("HTTP server shut down");
    return MCP_OK;
}
