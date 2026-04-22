#ifndef MCP_MCP_H
#define MCP_MCP_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_PROTOCOL_VERSION "2025-06-18"

#define MCP_OK                    0
#define MCP_ERR_INVALID_ARG      -1
#define MCP_ERR_OOM              -2
#define MCP_ERR_PARSE            -3
#define MCP_ERR_DUPLICATE        -4
#define MCP_ERR_NOT_INITIALIZED  -5
#define MCP_ERR_IO               -6
#define MCP_ERR_INTERNAL         -7

typedef struct mcp_server mcp_server_t;

typedef struct mcp_tool_response mcp_tool_response_t;

/* cJSON is re-exposed because tool handlers parse arguments from JSON
 * and parsing/building JSON is the whole job — hiding it would just
 * force users to re-vendor a JSON lib. The include is cheap. */
struct cJSON;

/* Return 0 on success, non-zero on error. The handler must populate `resp`
 * via mcp_tool_response_* helpers. On non-zero return, the response is sent
 * as a tool error unless the handler already called mcp_tool_response_set_error. */
typedef int (*mcp_tool_handler_fn)(const struct cJSON *arguments,
                                   mcp_tool_response_t *resp,
                                   void *userdata);

/* ------------------------------------------------------------------ */
/*  Server lifecycle                                                  */
/* ------------------------------------------------------------------ */

mcp_server_t *mcp_server_create(const char *name, const char *version);
void          mcp_server_destroy(mcp_server_t *srv);

/* Register a tool.
 *   name               — unique id used in tools/call.
 *   title              — human-readable display name (optional; NULL ok).
 *   description        — what the tool does (optional; NULL ok).
 *   input_schema_json  — a JSON Schema document as a string. May be NULL,
 *                        in which case an empty object schema is used.
 *   handler, userdata  — invoked on tools/call.
 */
int mcp_server_add_tool(mcp_server_t       *srv,
                        const char         *name,
                        const char         *title,
                        const char         *description,
                        const char         *input_schema_json,
                        mcp_tool_handler_fn handler,
                        void               *userdata);

/* Blocking run-loop on stdin/stdout. Returns MCP_OK on clean EOF. */
int mcp_server_run_stdio(mcp_server_t *srv);

/* Blocking run-loop serving JSON-RPC over HTTP on POST /mcp.
 *   host — listen address (e.g. "0.0.0.0", "127.0.0.1"); NULL ⇒ all interfaces.
 *   port — TCP port.
 * Exits on SIGINT/SIGTERM. Returns MCP_OK on clean shutdown. */
int mcp_server_run_http(mcp_server_t *srv, const char *host, unsigned short port);

/* ------------------------------------------------------------------ */
/*  Tool response builder (called from inside a handler)              */
/* ------------------------------------------------------------------ */

int  mcp_tool_response_add_text(mcp_tool_response_t *resp, const char *text);
void mcp_tool_response_set_error(mcp_tool_response_t *resp, const char *message);

#ifdef __cplusplus
}
#endif
#endif /* MCP_MCP_H */
