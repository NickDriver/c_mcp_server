#ifndef MCP_INTERNAL_H
#define MCP_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "mcp/mcp.h"

/* JSON-RPC 2.0 error codes. */
#define JSONRPC_PARSE_ERROR      (-32700)
#define JSONRPC_INVALID_REQUEST  (-32600)
#define JSONRPC_METHOD_NOT_FOUND (-32601)
#define JSONRPC_INVALID_PARAMS   (-32602)
#define JSONRPC_INTERNAL_ERROR   (-32603)

typedef struct mcp_tool {
    char               *name;
    char               *title;           /* may be NULL */
    char               *description;     /* may be NULL */
    cJSON              *input_schema;    /* owned; never NULL */
    mcp_tool_handler_fn handler;
    void               *userdata;
} mcp_tool_t;

struct mcp_server {
    char       *name;
    char       *version;
    mcp_tool_t *tools;
    size_t      tools_count;
    size_t      tools_cap;
    bool        initialized;
    bool        shutdown_requested;
};

struct mcp_tool_response {
    cJSON *content;     /* JSON array; owned */
    cJSON *error_msg;   /* JSON string; owned; NULL if not an error */
    bool   is_error;
};

/* ---- mcp_server.c ---- */
const mcp_tool_t *mcp_server_find_tool(const mcp_server_t *srv, const char *name);

/* ---- mcp_jsonrpc.c ----
 * Handle a single parsed JSON-RPC request. Returns a cJSON response object
 * ready to be serialized, or NULL if the request was a notification (no
 * response expected). Caller owns the returned cJSON. */
cJSON *mcp_handle_message(mcp_server_t *srv, const cJSON *msg);

/* ---- mcp_log.c ----
 * Log to stderr. Stdout is reserved for JSON-RPC messages. */
void mcp_log(const char *level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define MCP_LOG_INFO(...)  mcp_log("info",  __VA_ARGS__)
#define MCP_LOG_WARN(...)  mcp_log("warn",  __VA_ARGS__)
#define MCP_LOG_ERROR(...) mcp_log("error", __VA_ARGS__)

#endif /* MCP_INTERNAL_H */
