/* HTTP transport integration test.
 *
 * Forks a child that runs the MCP HTTP server on 127.0.0.1:<TEST_PORT>,
 * then the parent drives it over real sockets. Covers:
 *   - GET /              → 200 landing page
 *   - OPTIONS /mcp       → 204 CORS preflight
 *   - POST /mcp (JSON)   → 200 application/json
 *   - POST /mcp (SSE)    → 200 text/event-stream, single message event
 *   - POST /mcp (notify) → 202 Accepted
 *   - POST /mcp bad JSON → 400 with -32700
 *   - POST /mcp bad len  → 400
 *   - GET /mcp no Accept → 405
 *   - GET /mcp SSE       → 200 text/event-stream, `: open` comment
 *   - GET /bogus         → 404
 *   - Legacy: GET /sse + POST /message?sessionId=... round trip
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "mcp/mcp.h"

#define TEST_PORT 18765

static int stub_tool(const cJSON *args, mcp_tool_response_t *resp, void *u)
{
    (void)args;
    (void)u;
    return mcp_tool_response_add_text(resp, "stub-ok");
}

static int connect_local(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port   = htons(TEST_PORT);
    (void)inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        (void)close(fd);
        return -1;
    }
    return fd;
}

static void wait_for_server(void)
{
    for (int i = 0; i < 200; ++i) {
        int fd = connect_local();
        if (fd >= 0) {
            (void)close(fd);
            return;
        }
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000 * 1000};
        (void)nanosleep(&ts, NULL);
    }
    (void)fprintf(stderr, "test_http: server never started\n");
    abort();
}

/* Read until EOF or buf full. NUL-terminates. Returns bytes read. */
static size_t read_all(int fd, char *buf, size_t cap)
{
    size_t total = 0;
    while (total + 1 < cap) {
        ssize_t r = recv(fd, buf + total, cap - total - 1, 0);
        if (r <= 0) {
            break;
        }
        total += (size_t)r;
    }
    buf[total] = '\0';
    return total;
}

/* Read until `needle` appears in the buffer, or until `deadline_ms` passes. */
static size_t read_until(int fd, char *buf, size_t cap, const char *needle,
                         int deadline_ms)
{
    size_t total = 0;
    struct timeval tv = {.tv_sec = 0, .tv_usec = 100 * 1000}; /* 100ms */
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    int waited = 0;
    while (total + 1 < cap && waited < deadline_ms) {
        ssize_t r = recv(fd, buf + total, cap - total - 1, 0);
        if (r > 0) {
            total += (size_t)r;
            buf[total] = '\0';
            if (strstr(buf, needle) != NULL) {
                return total;
            }
        } else if (r == 0) {
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                waited += 100;
                continue;
            }
            break;
        }
    }
    buf[total] = '\0';
    return total;
}

/* Build + send a single HTTP request. Returns response bytes read. */
static size_t do_http(const char *method, const char *path,
                      const char *ctype, const char *accept,
                      const char *body,
                      char *out, size_t out_cap)
{
    int fd = connect_local();
    assert(fd >= 0);

    char req[8192];
    size_t body_len = body != NULL ? strlen(body) : 0;
    int n = snprintf(req, sizeof req,
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "%s%s%s"
        "%s%s%s"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        method, path, TEST_PORT,
        ctype  ? "Content-Type: " : "", ctype  ? ctype  : "", ctype  ? "\r\n" : "",
        accept ? "Accept: "       : "", accept ? accept : "", accept ? "\r\n" : "",
        body_len);
    assert(n > 0 && (size_t)n < sizeof req);

    /* Send headers + body in one go. */
    (void)send(fd, req, (size_t)n, 0);
    if (body_len > 0) {
        (void)send(fd, body, body_len, 0);
    }
    size_t got = read_all(fd, out, out_cap);
    (void)close(fd);
    return got;
}

static int http_status(const char *resp)
{
    int s = -1;
    if (sscanf(resp, "HTTP/1.1 %d", &s) != 1) {
        return -1;
    }
    return s;
}

static const char *http_body(const char *resp)
{
    const char *p = strstr(resp, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                             */
/* ------------------------------------------------------------------ */

static void test_root_page(void)
{
    char out[4096];
    (void)do_http("GET", "/", NULL, NULL, NULL, out, sizeof out);
    assert(http_status(out) == 200);
    assert(strstr(out, "text/html") != NULL);
    assert(strstr(out, "MCP HTTP server") != NULL);
    (void)puts("ok http_root_page");
}

static void test_options_preflight(void)
{
    char out[2048];
    (void)do_http("OPTIONS", "/mcp", NULL, NULL, NULL, out, sizeof out);
    assert(http_status(out) == 204);
    assert(strstr(out, "Access-Control-Allow-Origin: *") != NULL);
    assert(strstr(out, "Access-Control-Allow-Headers:") != NULL);
    (void)puts("ok http_options_preflight");
}

static void test_not_found(void)
{
    char out[2048];
    (void)do_http("GET", "/does-not-exist", NULL, NULL, NULL, out, sizeof out);
    assert(http_status(out) == 404);
    (void)puts("ok http_not_found");
}

static void test_post_json_response(void)
{
    const char *req =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2025-06-18\"}}";
    char out[8192];
    (void)do_http("POST", "/mcp", "application/json", "application/json",
                  req, out, sizeof out);
    assert(http_status(out) == 200);
    assert(strstr(out, "Content-Type: application/json") != NULL);
    const char *body = http_body(out);
    assert(body != NULL);
    cJSON *j = cJSON_Parse(body);
    assert(j != NULL);
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(j, "result");
    assert(cJSON_IsObject(result));
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(result, "protocolVersion");
    assert(cJSON_IsString(pv));
    cJSON_Delete(j);
    (void)puts("ok http_post_json_response");
}

static void test_post_sse_response(void)
{
    const char *req =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}";
    char out[8192];
    (void)do_http("POST", "/mcp", "application/json", "text/event-stream",
                  req, out, sizeof out);
    assert(http_status(out) == 200);
    assert(strstr(out, "Content-Type: text/event-stream") != NULL);
    assert(strstr(out, "event: message\n") != NULL);

    const char *data = strstr(out, "data: ");
    assert(data != NULL);
    data += strlen("data: ");
    const char *end = strchr(data, '\n');
    assert(end != NULL);
    char json[4096] = {0};
    size_t n = (size_t)(end - data);
    assert(n < sizeof json);
    memcpy(json, data, n);

    cJSON *j = cJSON_Parse(json);
    assert(j != NULL);
    assert(cJSON_GetObjectItemCaseSensitive(j, "result") != NULL);
    cJSON_Delete(j);
    (void)puts("ok http_post_sse_response");
}

static void test_post_notification_202(void)
{
    const char *req =
        "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}";
    char out[4096];
    (void)do_http("POST", "/mcp", "application/json", NULL,
                  req, out, sizeof out);
    assert(http_status(out) == 202);
    (void)puts("ok http_post_notification_202");
}

static void test_post_parse_error(void)
{
    char out[4096];
    (void)do_http("POST", "/mcp", "application/json", NULL,
                  "this is not json", out, sizeof out);
    assert(http_status(out) == 400);
    const char *body = http_body(out);
    assert(body != NULL);
    assert(strstr(body, "-32700") != NULL);
    (void)puts("ok http_post_parse_error");
}

static void test_post_missing_length(void)
{
    /* Build a raw request with no Content-Length header. */
    int fd = connect_local();
    assert(fd >= 0);
    const char *raw =
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Connection: close\r\n"
        "\r\n";
    (void)send(fd, raw, strlen(raw), 0);
    (void)shutdown(fd, SHUT_WR);
    char out[4096];
    (void)read_all(fd, out, sizeof out);
    (void)close(fd);
    assert(http_status(out) == 400);
    (void)puts("ok http_post_missing_length");
}

static void test_get_mcp_requires_sse_accept(void)
{
    char out[4096];
    (void)do_http("GET", "/mcp", NULL, "application/json",
                  NULL, out, sizeof out);
    assert(http_status(out) == 405);
    (void)puts("ok http_get_mcp_requires_sse_accept");
}

static void test_get_mcp_sse_stream(void)
{
    /* Open a GET /mcp SSE stream, read the `: open` comment, then close. */
    int fd = connect_local();
    assert(fd >= 0);
    const char *req =
        "GET /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Accept: text/event-stream\r\n"
        "Connection: close\r\n"
        "\r\n";
    (void)send(fd, req, strlen(req), 0);

    char out[4096] = {0};
    (void)read_until(fd, out, sizeof out, ": open\n\n", 2000);
    (void)close(fd);

    assert(http_status(out) == 200);
    assert(strstr(out, "Content-Type: text/event-stream") != NULL);
    assert(strstr(out, ": open\n\n") != NULL);
    (void)puts("ok http_get_mcp_sse_stream");
}

/* Full legacy HTTP+SSE round trip:
 *   1. open GET /sse; read endpoint event, extract sessionId
 *   2. POST /message?sessionId=... with an `initialize` request
 *   3. POST returns 202 immediately
 *   4. On the SSE stream, we see `event: message` with the response. */
static void test_legacy_sse_roundtrip(void)
{
    int sse_fd = connect_local();
    assert(sse_fd >= 0);
    const char *get_req =
        "GET /sse HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Accept: text/event-stream\r\n"
        "Connection: close\r\n"
        "\r\n";
    (void)send(sse_fd, get_req, strlen(get_req), 0);

    char buf[8192] = {0};
    (void)read_until(sse_fd, buf, sizeof buf, "event: endpoint", 2000);
    assert(http_status(buf) == 200);
    assert(strstr(buf, "Content-Type: text/event-stream") != NULL);

    const char *sid_key = strstr(buf, "sessionId=");
    assert(sid_key != NULL);
    sid_key += strlen("sessionId=");
    char sid[64] = {0};
    for (size_t i = 0; i < 32 && sid_key[i] != '\0'
                       && sid_key[i] != '\n' && sid_key[i] != '\r'
                       && sid_key[i] != '&'; ++i) {
        sid[i] = sid_key[i];
    }
    assert(strlen(sid) == 32);

    char path[128];
    (void)snprintf(path, sizeof path, "/message?sessionId=%s", sid);
    char post_resp[4096];
    (void)do_http("POST", path, "application/json", NULL,
                  "{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"initialize\"}",
                  post_resp, sizeof post_resp);
    assert(http_status(post_resp) == 202);

    /* The response should now appear on the SSE stream as an event. Keep
     * reading, accumulating into `buf`. */
    size_t prev_len = strlen(buf);
    (void)read_until(sse_fd, buf + prev_len, sizeof buf - prev_len,
                     "\"id\":42", 2000);
    (void)close(sse_fd);

    const char *tail = buf + prev_len;
    assert(strstr(tail, "event: message") != NULL);
    assert(strstr(tail, "\"id\":42") != NULL);
    assert(strstr(tail, "\"result\"") != NULL);
    (void)puts("ok http_legacy_sse_roundtrip");
}

static void test_legacy_message_unknown_session(void)
{
    char out[4096];
    (void)do_http("POST", "/message?sessionId=deadbeef",
                  "application/json", NULL,
                  "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}",
                  out, sizeof out);
    assert(http_status(out) == 404);
    (void)puts("ok http_legacy_message_unknown_session");
}

/* ------------------------------------------------------------------ */
/*  Server process harness                                            */
/* ------------------------------------------------------------------ */

static int run_server_child(void)
{
    mcp_server_t *srv = mcp_server_create("test-http", "0.0.0");
    if (srv == NULL) {
        return 1;
    }
    int rc = mcp_server_add_tool(srv, "stub", "Stub", "desc",
                                 "{\"type\":\"object\",\"properties\":{}}",
                                 stub_tool, NULL);
    if (rc != MCP_OK) {
        mcp_server_destroy(srv);
        return 2;
    }
    rc = mcp_server_run_http(srv, "127.0.0.1", TEST_PORT);
    mcp_server_destroy(srv);
    return rc == MCP_OK ? 0 : 3;
}

int main(void)
{
    /* Ignore SIGPIPE in the parent too; some clients may close early. */
    struct sigaction sa = {.sa_handler = SIG_IGN};
    (void)sigaction(SIGPIPE, &sa, NULL);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        /* Child. Suppress server logs to keep test output tidy. */
        (void)freopen("/dev/null", "w", stderr);
        exit(run_server_child());
    }

    wait_for_server();

    test_root_page();
    test_options_preflight();
    test_not_found();
    test_post_json_response();
    test_post_sse_response();
    test_post_notification_202();
    test_post_parse_error();
    test_post_missing_length();
    test_get_mcp_requires_sse_accept();
    test_get_mcp_sse_stream();
    test_legacy_message_unknown_session();
    test_legacy_sse_roundtrip();

    (void)kill(pid, SIGTERM);
    int status = 0;
    (void)waitpid(pid, &status, 0);

    (void)puts("all HTTP tests passed");
    return 0;
}
