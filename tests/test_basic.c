/* In-process dispatcher tests: exercise mcp_handle_message without transports. */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mcp/mcp.h"

/* Pull in the internal entry point. We intentionally redeclare it here
 * rather than including the internal header — the test links against
 * the static library. */
cJSON *mcp_handle_message(mcp_server_t *srv, const cJSON *msg);

static int echo_tool(const cJSON *args, mcp_tool_response_t *resp, void *userdata)
{
    (void)userdata;
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(args, "text");
    if (!cJSON_IsString(t)) {
        mcp_tool_response_set_error(resp, "need text");
        return 1;
    }
    return mcp_tool_response_add_text(resp, t->valuestring);
}

/* Returns non-zero without calling set_error — dispatcher must synthesize
 * a generic tool error. */
static int silent_fail_tool(const cJSON *args, mcp_tool_response_t *resp, void *userdata)
{
    (void)args;
    (void)resp;
    (void)userdata;
    return 1;
}

static cJSON *parse(const char *s)
{
    cJSON *j = cJSON_Parse(s);
    assert(j != NULL);
    return j;
}

static void init_server(mcp_server_t *srv)
{
    cJSON *init = parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}");
    cJSON_Delete(mcp_handle_message(srv, init));
    cJSON_Delete(init);
}

static int get_err_code(const cJSON *resp)
{
    const cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
    assert(cJSON_IsObject(err));
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(err, "code");
    assert(cJSON_IsNumber(code));
    return (int)code->valuedouble;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                             */
/* ------------------------------------------------------------------ */

static void test_initialize_flow(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    assert(srv != NULL);

    cJSON *req = parse(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
        "\"params\":{\"protocolVersion\":\"2025-06-18\"}}");
    cJSON *resp = mcp_handle_message(srv, req);
    assert(resp != NULL);

    const cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    assert(cJSON_IsObject(result));
    const cJSON *pv = cJSON_GetObjectItemCaseSensitive(result, "protocolVersion");
    assert(cJSON_IsString(pv));
    assert(strcmp(pv->valuestring, MCP_PROTOCOL_VERSION) == 0);

    const cJSON *info = cJSON_GetObjectItemCaseSensitive(result, "serverInfo");
    assert(cJSON_IsObject(info));
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(info, "name")->valuestring, "t") == 0);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(info, "version")->valuestring, "0.0.1") == 0);

    const cJSON *caps = cJSON_GetObjectItemCaseSensitive(result, "capabilities");
    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(caps, "tools");
    assert(cJSON_IsObject(tools));
    const cJSON *lc = cJSON_GetObjectItemCaseSensitive(tools, "listChanged");
    assert(cJSON_IsBool(lc) && !cJSON_IsTrue(lc));

    cJSON_Delete(req);
    cJSON_Delete(resp);
    mcp_server_destroy(srv);
    (void)puts("ok initialize_flow");
}

static void test_ping(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    /* Ping must work before initialize. */
    cJSON *req = parse("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}");
    cJSON *resp = mcp_handle_message(srv, req);
    assert(resp != NULL);
    assert(cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(resp, "result")));
    cJSON_Delete(req);
    cJSON_Delete(resp);
    mcp_server_destroy(srv);
    (void)puts("ok ping");
}

static void test_tools_list_and_call(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    assert(srv != NULL);
    assert(mcp_server_add_tool(srv, "echo", "Echo", "desc",
        "{\"type\":\"object\",\"properties\":{\"text\":{\"type\":\"string\"}},\"required\":[\"text\"]}",
        echo_tool, NULL) == MCP_OK);

    init_server(srv);

    cJSON *list_req = parse("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}");
    cJSON *list_resp = mcp_handle_message(srv, list_req);
    assert(list_resp != NULL);
    const cJSON *tools = cJSON_GetObjectItemCaseSensitive(
        cJSON_GetObjectItemCaseSensitive(list_resp, "result"), "tools");
    assert(cJSON_IsArray(tools));
    assert(cJSON_GetArraySize(tools) == 1);
    cJSON_Delete(list_req);
    cJSON_Delete(list_resp);

    /* success */
    cJSON *call_req = parse(
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo\",\"arguments\":{\"text\":\"hello\"}}}");
    cJSON *call_resp = mcp_handle_message(srv, call_req);
    assert(call_resp != NULL);
    const cJSON *cres = cJSON_GetObjectItemCaseSensitive(call_resp, "result");
    const cJSON *is_err = cJSON_GetObjectItemCaseSensitive(cres, "isError");
    assert(cJSON_IsBool(is_err) && !cJSON_IsTrue(is_err));
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(cres, "content");
    assert(cJSON_IsArray(content) && cJSON_GetArraySize(content) == 1);
    const cJSON *item0 = cJSON_GetArrayItem(content, 0);
    const cJSON *txt = cJSON_GetObjectItemCaseSensitive(item0, "text");
    assert(cJSON_IsString(txt) && strcmp(txt->valuestring, "hello") == 0);
    cJSON_Delete(call_req);
    cJSON_Delete(call_resp);

    /* unknown tool → RPC error */
    cJSON *bad_req = parse(
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"nope\"}}");
    cJSON *bad_resp = mcp_handle_message(srv, bad_req);
    assert(bad_resp != NULL);
    assert(get_err_code(bad_resp) == -32601);
    cJSON_Delete(bad_req);
    cJSON_Delete(bad_resp);

    /* handler signalled tool-level error (isError=true) */
    cJSON *ebad_req = parse(
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"echo\",\"arguments\":{}}}");
    cJSON *ebad_resp = mcp_handle_message(srv, ebad_req);
    assert(ebad_resp != NULL);
    const cJSON *eres = cJSON_GetObjectItemCaseSensitive(ebad_resp, "result");
    assert(cJSON_IsObject(eres));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(eres, "isError")));
    cJSON_Delete(ebad_req);
    cJSON_Delete(ebad_resp);

    mcp_server_destroy(srv);
    (void)puts("ok tools_list_and_call");
}

static void test_tool_silent_fail_synthesises_error(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    assert(mcp_server_add_tool(srv, "boom", NULL, NULL, NULL,
                               silent_fail_tool, NULL) == MCP_OK);
    init_server(srv);

    cJSON *req = parse(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"boom\"}}");
    cJSON *resp = mcp_handle_message(srv, req);
    assert(resp != NULL);
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(resp, "result");
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(res, "isError")));
    const cJSON *content = cJSON_GetObjectItemCaseSensitive(res, "content");
    assert(cJSON_IsArray(content) && cJSON_GetArraySize(content) >= 1);
    const cJSON *first = cJSON_GetArrayItem(content, 0);
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(first, "text");
    assert(cJSON_IsString(text));
    assert(strstr(text->valuestring, "tool") != NULL);

    cJSON_Delete(req);
    cJSON_Delete(resp);
    mcp_server_destroy(srv);
    (void)puts("ok tool_silent_fail_synthesises_error");
}

static void test_tools_call_bad_params(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    init_server(srv);

    /* Missing params entirely */
    cJSON *a = parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\"}");
    cJSON *ra = mcp_handle_message(srv, a);
    assert(get_err_code(ra) == -32602);
    cJSON_Delete(a);
    cJSON_Delete(ra);

    /* Non-string name */
    cJSON *b = parse(
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\","
        "\"params\":{\"name\":123}}");
    cJSON *rb = mcp_handle_message(srv, b);
    assert(get_err_code(rb) == -32602);
    cJSON_Delete(b);
    cJSON_Delete(rb);

    mcp_server_destroy(srv);
    (void)puts("ok tools_call_bad_params");
}

static void test_notification_no_response(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    cJSON *n = parse("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    cJSON *r = mcp_handle_message(srv, n);
    assert(r == NULL);
    cJSON_Delete(n);

    /* Unknown notifications are also dropped silently. */
    cJSON *n2 = parse("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/whatever\"}");
    assert(mcp_handle_message(srv, n2) == NULL);
    cJSON_Delete(n2);

    mcp_server_destroy(srv);
    (void)puts("ok notification_no_response");
}

static void test_uninitialized_rejected(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    cJSON *q = parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}");
    cJSON *r = mcp_handle_message(srv, q);
    assert(r != NULL);
    assert(get_err_code(r) == -32600);
    cJSON_Delete(q);
    cJSON_Delete(r);
    mcp_server_destroy(srv);
    (void)puts("ok uninitialized_rejected");
}

static void test_invalid_jsonrpc_version(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    cJSON *q = parse("{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}");
    cJSON *r = mcp_handle_message(srv, q);
    assert(get_err_code(r) == -32600);
    cJSON_Delete(q);
    cJSON_Delete(r);

    /* Non-object message */
    cJSON *arr = parse("[1,2,3]");
    cJSON *ra  = mcp_handle_message(srv, arr);
    assert(get_err_code(ra) == -32600);
    cJSON_Delete(arr);
    cJSON_Delete(ra);

    /* Missing method */
    cJSON *nm = parse("{\"jsonrpc\":\"2.0\",\"id\":1}");
    cJSON *rnm = mcp_handle_message(srv, nm);
    assert(get_err_code(rnm) == -32600);
    cJSON_Delete(nm);
    cJSON_Delete(rnm);

    mcp_server_destroy(srv);
    (void)puts("ok invalid_jsonrpc_version");
}

static void test_method_not_found(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    init_server(srv);
    cJSON *q = parse("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"does/not/exist\"}");
    cJSON *r = mcp_handle_message(srv, q);
    assert(get_err_code(r) == -32601);
    cJSON_Delete(q);
    cJSON_Delete(r);
    mcp_server_destroy(srv);
    (void)puts("ok method_not_found");
}

static void test_duplicate_tool(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    assert(mcp_server_add_tool(srv, "echo", NULL, NULL, NULL, echo_tool, NULL) == MCP_OK);
    assert(mcp_server_add_tool(srv, "echo", NULL, NULL, NULL, echo_tool, NULL) == MCP_ERR_DUPLICATE);
    /* Different name must still succeed → tools array growth path. */
    for (int i = 0; i < 10; ++i) {
        char name[32];
        (void)snprintf(name, sizeof name, "tool_%d", i);
        assert(mcp_server_add_tool(srv, name, NULL, NULL, NULL, echo_tool, NULL) == MCP_OK);
    }
    mcp_server_destroy(srv);
    (void)puts("ok duplicate_tool");
}

static void test_id_preservation(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");

    /* String id */
    cJSON *q1 = parse("{\"jsonrpc\":\"2.0\",\"id\":\"abc-123\",\"method\":\"ping\"}");
    cJSON *r1 = mcp_handle_message(srv, q1);
    const cJSON *id1 = cJSON_GetObjectItemCaseSensitive(r1, "id");
    assert(cJSON_IsString(id1) && strcmp(id1->valuestring, "abc-123") == 0);
    cJSON_Delete(q1);
    cJSON_Delete(r1);

    /* Numeric id */
    cJSON *q2 = parse("{\"jsonrpc\":\"2.0\",\"id\":42,\"method\":\"ping\"}");
    cJSON *r2 = mcp_handle_message(srv, q2);
    const cJSON *id2 = cJSON_GetObjectItemCaseSensitive(r2, "id");
    assert(cJSON_IsNumber(id2) && (int)id2->valuedouble == 42);
    cJSON_Delete(q2);
    cJSON_Delete(r2);

    /* Null id */
    cJSON *q3 = parse("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}");
    cJSON *r3 = mcp_handle_message(srv, q3);
    assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(r3, "id")));
    cJSON_Delete(q3);
    cJSON_Delete(r3);

    mcp_server_destroy(srv);
    (void)puts("ok id_preservation");
}

static int three_text_tool(const cJSON *a, mcp_tool_response_t *r, void *u)
{
    (void)a; (void)u;
    assert(mcp_tool_response_add_text(r, "one")   == MCP_OK);
    assert(mcp_tool_response_add_text(r, "two")   == MCP_OK);
    assert(mcp_tool_response_add_text(r, "three") == MCP_OK);
    return 0;
}

static void test_multiple_text_items(void)
{
    mcp_server_t *srv = mcp_server_create("t", "0.0.1");
    assert(mcp_server_add_tool(srv, "multi", NULL, NULL, NULL, three_text_tool, NULL) == MCP_OK);
    init_server(srv);

    cJSON *q = parse(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"multi\"}}");
    cJSON *r = mcp_handle_message(srv, q);
    const cJSON *res = cJSON_GetObjectItemCaseSensitive(r, "result");
    const cJSON *c   = cJSON_GetObjectItemCaseSensitive(res, "content");
    assert(cJSON_IsArray(c) && cJSON_GetArraySize(c) == 3);
    cJSON_Delete(q);
    cJSON_Delete(r);
    mcp_server_destroy(srv);
    (void)puts("ok multiple_text_items");
}

int main(void)
{
    test_initialize_flow();
    test_ping();
    test_tools_list_and_call();
    test_tool_silent_fail_synthesises_error();
    test_tools_call_bad_params();
    test_notification_no_response();
    test_uninitialized_rejected();
    test_invalid_jsonrpc_version();
    test_method_not_found();
    test_duplicate_tool();
    test_id_preservation();
    test_multiple_text_items();
    (void)puts("all basic tests passed");
    return 0;
}
