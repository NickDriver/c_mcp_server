/* Example MCP server exposing two tools:
 *   - "echo"  : returns the `text` argument verbatim
 *   - "add"   : returns the sum of two numbers `a` and `b`
 *
 * Run it via stdio:   ./echo_server
 * and drive it with newline-delimited JSON-RPC on stdin.
 */

#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "mcp/mcp.h"

static int echo_tool(const cJSON *args, mcp_tool_response_t *resp, void *userdata)
{
    (void)userdata;

    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(args, "text");
    if (!cJSON_IsString(txt) || txt->valuestring == NULL) {
        mcp_tool_response_set_error(resp, "argument 'text' must be a string");
        return 1;
    }
    return mcp_tool_response_add_text(resp, txt->valuestring);
}

static int add_tool(const cJSON *args, mcp_tool_response_t *resp, void *userdata)
{
    (void)userdata;

    const cJSON *a = cJSON_GetObjectItemCaseSensitive(args, "a");
    const cJSON *b = cJSON_GetObjectItemCaseSensitive(args, "b");
    if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b)) {
        mcp_tool_response_set_error(resp, "arguments 'a' and 'b' must be numbers");
        return 1;
    }
    double sum = cJSON_GetNumberValue(a) + cJSON_GetNumberValue(b);
    char buf[64];
    (void)snprintf(buf, sizeof buf, "%g", sum);
    return mcp_tool_response_add_text(resp, buf);
}

int main(void)
{
    mcp_server_t *srv = mcp_server_create("echo-server", "0.1.0");
    if (srv == NULL) {
        (void)fprintf(stderr, "failed to create server\n");
        return EXIT_FAILURE;
    }

    int rc = mcp_server_add_tool(
        srv, "echo", "Echo", "Return the input text unchanged.",
        "{\"type\":\"object\","
        "\"properties\":{\"text\":{\"type\":\"string\",\"description\":\"text to echo\"}},"
        "\"required\":[\"text\"]}",
        echo_tool, NULL);
    if (rc != MCP_OK) {
        (void)fprintf(stderr, "add_tool(echo) failed: %d\n", rc);
        mcp_server_destroy(srv);
        return EXIT_FAILURE;
    }

    rc = mcp_server_add_tool(
        srv, "add", "Add", "Return a + b.",
        "{\"type\":\"object\","
        "\"properties\":{"
        "\"a\":{\"type\":\"number\"},"
        "\"b\":{\"type\":\"number\"}"
        "},\"required\":[\"a\",\"b\"]}",
        add_tool, NULL);
    if (rc != MCP_OK) {
        (void)fprintf(stderr, "add_tool(add) failed: %d\n", rc);
        mcp_server_destroy(srv);
        return EXIT_FAILURE;
    }

    rc = mcp_server_run_stdio(srv);
    mcp_server_destroy(srv);
    return rc == MCP_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
