# c_mcp_server

A pure-C23 SDK for writing [Model Context Protocol](https://modelcontextprotocol.io) servers.
Targets MCP protocol revision **2025-06-18**.

The official MCP SDKs live in TypeScript, Python, Kotlin, etc. This project fills the
C-native gap: you implement a few tool callbacks, hand them to the library, and call a
blocking run-loop. The library handles JSON-RPC framing, capability negotiation, the
`initialize`/`initialized` handshake, and `tools/list` + `tools/call` dispatch.

## Status

Early — **v0.1.0**. The tools primitive works end-to-end over both transports. Resources,
prompts, and change notifications are not implemented yet.

| Area               | State           |
|--------------------|-----------------|
| stdio transport    | working         |
| HTTP transport (POST `/mcp`) | working |
| Tools              | working         |
| Resources          | not implemented |
| Prompts            | not implemented |
| Notifications (`*/list_changed`) | not implemented |

## Requirements

- Clang with C23 support (the build system warns on other compilers but does not hard-fail).
- CMake ≥ 3.25.
- POSIX threads.

cJSON is vendored under [third_party/cJSON/](third_party/cJSON/); no system install needed.

## Build

```sh
cmake -S . -B build -DCMAKE_C_COMPILER=clang
cmake --build build
ctest --test-dir build --output-on-failure
```

Builds default to `Debug`. `NDEBUG` is stripped from all release flavors — asserts stay
live in every configuration by design.

## Quick start

```c
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

int main(void)
{
    mcp_server_t *srv = mcp_server_create("echo-server", "0.1.0");
    mcp_server_add_tool(
        srv, "echo", "Echo", "Return the input text unchanged.",
        "{\"type\":\"object\","
        "\"properties\":{\"text\":{\"type\":\"string\"}},"
        "\"required\":[\"text\"]}",
        echo_tool, NULL);
    int rc = mcp_server_run_stdio(srv);
    mcp_server_destroy(srv);
    return rc;
}
```

Link against the `mcp` CMake target — it pulls in `cjson` and `Threads::Threads`
transitively:

```cmake
target_link_libraries(your_server PRIVATE mcp)
```

## Public API

The entire surface lives in [include/mcp/mcp.h](include/mcp/mcp.h):

| Function | Purpose |
|----------|---------|
| `mcp_server_create` / `mcp_server_destroy` | Lifecycle. |
| `mcp_server_add_tool` | Register a tool with a JSON-Schema input doc and a callback. |
| `mcp_server_run_stdio` | Blocking run-loop over stdin/stdout (newline-delimited JSON-RPC). |
| `mcp_server_run_http` | Blocking HTTP server on `POST /mcp`; stops on SIGINT/SIGTERM. |
| `mcp_tool_response_add_text` | Append a text block to the tool result. |
| `mcp_tool_response_set_error` | Mark the result as a tool error with a message. |

Tool handlers receive the parsed `arguments` object as a `cJSON *` — cJSON is
deliberately re-exposed so handlers don't have to re-vendor a JSON parser.

## Transports

### stdio

Each line on stdin is a single JSON-RPC request; each response is written on its own line
to stdout. Logs go to stderr. This is the transport every MCP host (Claude Desktop,
Claude Code, etc.) speaks natively.

### HTTP

`mcp_server_run_http(srv, host, port)` starts a minimal HTTP/1.1 server that accepts
`POST /mcp` with a JSON-RPC body and replies with a JSON-RPC response. No SSE, no
authentication — intended for local development and trusted networks.

```sh
./build/http_demo 127.0.0.1 8080
curl -s http://127.0.0.1:8080/mcp -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
```

## Examples

Three example servers live in [examples/](examples/) and are built by default:

- [examples/echo_server/](examples/echo_server/) — `echo` + `add`, stdio.
- [examples/demo_server/](examples/demo_server/) — `greet`, `get_time`, `motd`, stdio.
- [examples/http_demo/](examples/http_demo/) — same three tools over HTTP.

## Layout

```
include/mcp/mcp.h         public SDK header
src/mcp_server.c          server + tool registry
src/mcp_jsonrpc.c         JSON-RPC request/response dispatch
src/mcp_stdio.c           stdio run-loop
src/mcp_http.c            HTTP run-loop
src/mcp_log.c             stderr logging
tests/                    ctest-driven tests (basic, http)
third_party/cJSON/        vendored JSON parser (MIT)
```

## License

Not yet chosen. Vendored cJSON remains under its original MIT license (see
[third_party/cJSON/LICENSE](third_party/cJSON/LICENSE)).
