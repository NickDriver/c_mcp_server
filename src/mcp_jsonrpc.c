#include "mcp_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Duplicate a cJSON id value (string | number | null) so it can be attached
 * to a response owned by a different tree. Returns a detached node or NULL. */
static cJSON *clone_id(const cJSON *id)
{
    if (id == NULL || cJSON_IsNull(id)) {
        return cJSON_CreateNull();
    }
    if (cJSON_IsString(id)) {
        const char *s = cJSON_GetStringValue(id);
        return cJSON_CreateString(s != NULL ? s : "");
    }
    if (cJSON_IsNumber(id)) {
        return cJSON_CreateNumber(cJSON_GetNumberValue(id));
    }
    /* Per JSON-RPC 2.0, id SHOULD be string/number/null — anything else is
     * invalid; represent as null so the response is still well-formed. */
    return cJSON_CreateNull();
}

static cJSON *make_envelope(const cJSON *id)
{
    cJSON *env = cJSON_CreateObject();
    if (env == NULL) {
        return NULL;
    }
    if (cJSON_AddStringToObject(env, "jsonrpc", "2.0") == NULL) {
        cJSON_Delete(env);
        return NULL;
    }
    cJSON *id_copy = clone_id(id);
    if (id_copy == NULL || !cJSON_AddItemToObject(env, "id", id_copy)) {
        cJSON_Delete(id_copy);
        cJSON_Delete(env);
        return NULL;
    }
    return env;
}

static cJSON *make_error(const cJSON *id, int code, const char *message)
{
    assert(message != NULL);
    cJSON *env = make_envelope(id);
    if (env == NULL) {
        return NULL;
    }
    cJSON *err = cJSON_AddObjectToObject(env, "error");
    if (err == NULL
        || cJSON_AddNumberToObject(err, "code", code) == NULL
        || cJSON_AddStringToObject(err, "message", message) == NULL) {
        cJSON_Delete(env);
        return NULL;
    }
    return env;
}

static cJSON *make_result(const cJSON *id, cJSON *result_obj /* takes ownership */)
{
    assert(result_obj != NULL);
    cJSON *env = make_envelope(id);
    if (env == NULL) {
        cJSON_Delete(result_obj);
        return NULL;
    }
    if (!cJSON_AddItemToObject(env, "result", result_obj)) {
        cJSON_Delete(result_obj);
        cJSON_Delete(env);
        return NULL;
    }
    return env;
}

/* ------------------------------------------------------------------ */
/*  Method handlers                                                   */
/* ------------------------------------------------------------------ */

static cJSON *handle_initialize(mcp_server_t *srv, const cJSON *id, const cJSON *params)
{
    (void)params; /* we don't enforce client protocolVersion matching yet */
    assert(srv != NULL);

    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }

    if (cJSON_AddStringToObject(result, "protocolVersion", MCP_PROTOCOL_VERSION) == NULL) {
        cJSON_Delete(result);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }

    cJSON *caps = cJSON_AddObjectToObject(result, "capabilities");
    if (caps == NULL) {
        cJSON_Delete(result);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }
    /* We always advertise tools; listChanged=false until we implement notifications.
     * NOTE: cJSON_AddBoolToObject expects 0/1, NOT cJSON_True/cJSON_False (those are
     * bit flags for item->type and equal 2 and 1 respectively — both truthy). */
    cJSON *tools_cap = cJSON_AddObjectToObject(caps, "tools");
    if (tools_cap == NULL
        || cJSON_AddBoolToObject(tools_cap, "listChanged", 0) == NULL) {
        cJSON_Delete(result);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }

    cJSON *info = cJSON_AddObjectToObject(result, "serverInfo");
    if (info == NULL
        || cJSON_AddStringToObject(info, "name", srv->name) == NULL
        || cJSON_AddStringToObject(info, "version", srv->version) == NULL) {
        cJSON_Delete(result);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }

    srv->initialized = true;
    return make_result(id, result);
}

static cJSON *handle_ping(const cJSON *id)
{
    cJSON *empty = cJSON_CreateObject();
    if (empty == NULL) {
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }
    return make_result(id, empty);
}

static cJSON *handle_tools_list(mcp_server_t *srv, const cJSON *id)
{
    assert(srv != NULL);

    cJSON *result = cJSON_CreateObject();
    cJSON *arr    = result != NULL ? cJSON_AddArrayToObject(result, "tools") : NULL;
    if (arr == NULL) {
        cJSON_Delete(result);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }

    for (size_t i = 0; i < srv->tools_count; ++i) {
        const mcp_tool_t *t = &srv->tools[i];
        cJSON *obj = cJSON_CreateObject();
        if (obj == NULL) {
            cJSON_Delete(result);
            return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
        }
        bool ok = cJSON_AddStringToObject(obj, "name", t->name) != NULL;
        if (ok && t->title != NULL) {
            ok = cJSON_AddStringToObject(obj, "title", t->title) != NULL;
        }
        if (ok && t->description != NULL) {
            ok = cJSON_AddStringToObject(obj, "description", t->description) != NULL;
        }
        if (ok) {
            cJSON *schema_copy = cJSON_Duplicate(t->input_schema, cJSON_True);
            ok = schema_copy != NULL && cJSON_AddItemToObject(obj, "inputSchema", schema_copy);
            if (!ok) {
                cJSON_Delete(schema_copy);
            }
        }
        if (!ok || !cJSON_AddItemToArray(arr, obj)) {
            cJSON_Delete(obj);
            cJSON_Delete(result);
            return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
        }
    }
    return make_result(id, result);
}

static cJSON *handle_tools_call(mcp_server_t *srv, const cJSON *id, const cJSON *params)
{
    assert(srv != NULL);

    if (!cJSON_IsObject(params)) {
        return make_error(id, JSONRPC_INVALID_PARAMS, "params must be an object");
    }
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(params, "name");
    if (!cJSON_IsString(jname) || jname->valuestring == NULL) {
        return make_error(id, JSONRPC_INVALID_PARAMS, "params.name must be a string");
    }
    const mcp_tool_t *tool = mcp_server_find_tool(srv, jname->valuestring);
    if (tool == NULL) {
        return make_error(id, JSONRPC_METHOD_NOT_FOUND, "unknown tool");
    }

    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "arguments");
    /* `arguments` is optional; a missing value is treated as an empty object. */
    cJSON *args_empty = NULL;
    if (args == NULL) {
        args_empty = cJSON_CreateObject();
        if (args_empty == NULL) {
            return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
        }
        args = args_empty;
    } else if (!cJSON_IsObject(args)) {
        return make_error(id, JSONRPC_INVALID_PARAMS, "params.arguments must be an object");
    }

    mcp_tool_response_t resp = {
        .content   = cJSON_CreateArray(),
        .error_msg = NULL,
        .is_error  = false,
    };
    if (resp.content == NULL) {
        cJSON_Delete(args_empty);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }

    int rc = tool->handler(args, &resp, tool->userdata);
    cJSON_Delete(args_empty);

    if (rc != 0 && !resp.is_error) {
        /* Handler returned error but did not set a message — synthesize one. */
        mcp_tool_response_set_error(&resp, "tool handler failed");
    }

    /* Assemble the JSON-RPC result. A tool error is represented as a normal
     * RPC result with `isError: true` and the error message prepended to
     * content (per MCP spec). */
    cJSON *result = cJSON_CreateObject();
    if (result == NULL) {
        cJSON_Delete(resp.content);
        cJSON_Delete(resp.error_msg);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }

    if (resp.is_error) {
        const char *msg = "tool error";
        if (resp.error_msg != NULL && cJSON_IsString(resp.error_msg)) {
            const char *s = cJSON_GetStringValue(resp.error_msg);
            if (s != NULL && s[0] != '\0') {
                msg = s;
            }
        }
        cJSON *err_item = cJSON_CreateObject();
        if (err_item == NULL
            || cJSON_AddStringToObject(err_item, "type", "text") == NULL
            || cJSON_AddStringToObject(err_item, "text", msg) == NULL
            || !cJSON_InsertItemInArray(resp.content, 0, err_item)) {
            cJSON_Delete(err_item);
            cJSON_Delete(result);
            cJSON_Delete(resp.content);
            cJSON_Delete(resp.error_msg);
            return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
        }
    }

    if (!cJSON_AddItemToObject(result, "content", resp.content)) {
        cJSON_Delete(result);
        cJSON_Delete(resp.content);
        cJSON_Delete(resp.error_msg);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }
    /* resp.content ownership transferred */
    cJSON_Delete(resp.error_msg);

    if (cJSON_AddBoolToObject(result, "isError", resp.is_error ? 1 : 0) == NULL) {
        cJSON_Delete(result);
        return make_error(id, JSONRPC_INTERNAL_ERROR, "oom");
    }

    return make_result(id, result);
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                       */
/* ------------------------------------------------------------------ */

cJSON *mcp_handle_message(mcp_server_t *srv, const cJSON *msg)
{
    assert(srv != NULL);
    assert(msg != NULL);

    if (!cJSON_IsObject(msg)) {
        return make_error(NULL, JSONRPC_INVALID_REQUEST, "message must be an object");
    }

    const cJSON *jrpc   = cJSON_GetObjectItemCaseSensitive(msg, "jsonrpc");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(msg, "method");
    const cJSON *id     = cJSON_GetObjectItemCaseSensitive(msg, "id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(msg, "params");

    if (!cJSON_IsString(jrpc) || jrpc->valuestring == NULL
        || strcmp(jrpc->valuestring, "2.0") != 0) {
        return make_error(id, JSONRPC_INVALID_REQUEST, "jsonrpc must be \"2.0\"");
    }
    if (!cJSON_IsString(method) || method->valuestring == NULL) {
        return make_error(id, JSONRPC_INVALID_REQUEST, "method must be a string");
    }

    const char *m = method->valuestring;
    const bool is_notification = (id == NULL);

    /* Notifications: no response regardless of success/failure. */
    if (is_notification) {
        if (strcmp(m, "notifications/initialized") == 0) {
            /* no-op: initialize handler already flipped the flag */
        } else if (strcmp(m, "notifications/cancelled") == 0) {
            /* Accept and drop; we don't run long tasks yet. */
        } else {
            MCP_LOG_WARN("unknown notification: %s", m);
        }
        return NULL;
    }

    /* Requests: must be initialized for anything other than initialize/ping. */
    if (strcmp(m, "initialize") == 0) {
        return handle_initialize(srv, id, params);
    }
    if (strcmp(m, "ping") == 0) {
        return handle_ping(id);
    }
    if (!srv->initialized) {
        return make_error(id, JSONRPC_INVALID_REQUEST, "server not initialized");
    }
    if (strcmp(m, "tools/list") == 0) {
        return handle_tools_list(srv, id);
    }
    if (strcmp(m, "tools/call") == 0) {
        return handle_tools_call(srv, id, params);
    }
    return make_error(id, JSONRPC_METHOD_NOT_FOUND, m);
}
