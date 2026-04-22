/* HTTP demo MCP server. Same three tools as demo_server, but served over
 * HTTP (POST /mcp) instead of stdio.
 *
 * Usage:  http_demo [host [port]]     (defaults: 0.0.0.0 8080)
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "mcp/mcp.h"

static int greet_tool(const cJSON *args, mcp_tool_response_t *resp, void *userdata)
{
    (void)userdata;

    const char  *name = "world";
    const cJSON *n    = cJSON_GetObjectItemCaseSensitive(args, "name");
    if (cJSON_IsString(n) && n->valuestring != NULL && n->valuestring[0] != '\0') {
        name = n->valuestring;
    }

    char msg[256];
    int written = snprintf(msg, sizeof msg, "Hello, %s! Welcome to the C23 MCP SDK over HTTP.", name);
    if (written < 0 || (size_t)written >= sizeof msg) {
        mcp_tool_response_set_error(resp, "name too long");
        return 1;
    }
    return mcp_tool_response_add_text(resp, msg);
}

static int get_time_tool(const cJSON *args, mcp_tool_response_t *resp, void *userdata)
{
    (void)args;
    (void)userdata;

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        mcp_tool_response_set_error(resp, "clock_gettime failed");
        return 1;
    }
    struct tm tmv;
    if (gmtime_r(&ts.tv_sec, &tmv) == NULL) {
        mcp_tool_response_set_error(resp, "gmtime_r failed");
        return 1;
    }

    char buf[64];
    char msg[128];
    (void)strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    (void)snprintf(msg, sizeof msg, "Current UTC time is %s", buf);
    return mcp_tool_response_add_text(resp, msg);
}

static int motd_tool(const cJSON *args, mcp_tool_response_t *resp, void *userdata)
{
    (void)args;
    (void)userdata;
    return mcp_tool_response_add_text(
        resp,
        "Message of the day: all bugs are shallow when tests run fast.");
}

int main(int argc, char **argv)
{
    const char    *host = (argc > 1) ? argv[1] : "0.0.0.0";
    unsigned short port = 8080;
    if (argc > 2) {
        char *endp = NULL;
        unsigned long v = strtoul(argv[2], &endp, 10);
        if (endp == argv[2] || *endp != '\0' || v == 0 || v > 65535) {
            (void)fprintf(stderr, "invalid port: %s\n", argv[2]);
            return EXIT_FAILURE;
        }
        port = (unsigned short)v;
    }

    mcp_server_t *srv = mcp_server_create("http-demo-server", "0.1.0");
    if (srv == NULL) {
        (void)fprintf(stderr, "failed to create server\n");
        return EXIT_FAILURE;
    }

    struct {
        const char         *name;
        const char         *title;
        const char         *desc;
        const char         *schema;
        mcp_tool_handler_fn fn;
    } tools[] = {
        {
            "greet", "Greet", "Return a friendly greeting for the given name.",
            "{\"type\":\"object\","
            "\"properties\":{\"name\":{\"type\":\"string\",\"description\":\"who to greet\"}}}",
            greet_tool,
        },
        {
            "get_time", "Get Time", "Return the current UTC time as a message.",
            "{\"type\":\"object\",\"properties\":{}}",
            get_time_tool,
        },
        {
            "motd", "Message of the Day", "Return a static daily message.",
            "{\"type\":\"object\",\"properties\":{}}",
            motd_tool,
        },
    };

    for (size_t i = 0; i < sizeof tools / sizeof tools[0]; ++i) {
        int rc = mcp_server_add_tool(srv, tools[i].name, tools[i].title,
                                     tools[i].desc, tools[i].schema,
                                     tools[i].fn, NULL);
        if (rc != MCP_OK) {
            (void)fprintf(stderr, "add_tool(%s) failed: %d\n", tools[i].name, rc);
            mcp_server_destroy(srv);
            return EXIT_FAILURE;
        }
    }

    int rc = mcp_server_run_http(srv, host, port);
    mcp_server_destroy(srv);
    return rc == MCP_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
