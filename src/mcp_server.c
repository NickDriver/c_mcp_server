#include "mcp_internal.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

static char *xstrdup(const char *s)
{
    assert(s != NULL);
    size_t n = strlen(s) + 1u;
    char *out = (char *)malloc(n);
    if (out == NULL) {
        return NULL;
    }
    (void)memcpy(out, s, n);
    return out;
}

static char *xstrdup_or_null(const char *s)
{
    return s == NULL ? NULL : xstrdup(s);
}

mcp_server_t *mcp_server_create(const char *name, const char *version)
{
    assert(name != NULL && name[0] != '\0');
    assert(version != NULL && version[0] != '\0');

    mcp_server_t *srv = (mcp_server_t *)calloc(1, sizeof *srv);
    if (srv == NULL) {
        return NULL;
    }
    srv->name = xstrdup(name);
    srv->version = xstrdup(version);
    if (srv->name == NULL || srv->version == NULL) {
        mcp_server_destroy(srv);
        return NULL;
    }
    return srv;
}

static void tool_free(mcp_tool_t *t)
{
    assert(t != NULL);
    free(t->name);
    free(t->title);
    free(t->description);
    cJSON_Delete(t->input_schema);
    *t = (mcp_tool_t){0};
}

void mcp_server_destroy(mcp_server_t *srv)
{
    if (srv == NULL) {
        return;
    }
    for (size_t i = 0; i < srv->tools_count; ++i) {
        tool_free(&srv->tools[i]);
    }
    free(srv->tools);
    free(srv->name);
    free(srv->version);
    free(srv);
}

const mcp_tool_t *mcp_server_find_tool(const mcp_server_t *srv, const char *name)
{
    assert(srv != NULL);
    assert(name != NULL);
    for (size_t i = 0; i < srv->tools_count; ++i) {
        if (strcmp(srv->tools[i].name, name) == 0) {
            return &srv->tools[i];
        }
    }
    return NULL;
}

static int tools_reserve(mcp_server_t *srv, size_t want)
{
    assert(srv != NULL);
    if (want <= srv->tools_cap) {
        return MCP_OK;
    }
    size_t newcap = srv->tools_cap == 0 ? 4 : srv->tools_cap * 2;
    while (newcap < want) {
        newcap *= 2;
    }
    mcp_tool_t *nt = (mcp_tool_t *)realloc(srv->tools, newcap * sizeof *nt);
    if (nt == NULL) {
        return MCP_ERR_OOM;
    }
    srv->tools = nt;
    srv->tools_cap = newcap;
    return MCP_OK;
}

int mcp_server_add_tool(mcp_server_t       *srv,
                        const char         *name,
                        const char         *title,
                        const char         *description,
                        const char         *input_schema_json,
                        mcp_tool_handler_fn handler,
                        void               *userdata)
{
    assert(srv != NULL);
    assert(name != NULL && name[0] != '\0');
    assert(handler != NULL);

    if (srv == NULL || name == NULL || handler == NULL) {
        return MCP_ERR_INVALID_ARG;
    }
    if (mcp_server_find_tool(srv, name) != NULL) {
        return MCP_ERR_DUPLICATE;
    }

    cJSON *schema = NULL;
    if (input_schema_json != NULL && input_schema_json[0] != '\0') {
        schema = cJSON_Parse(input_schema_json);
        if (schema == NULL) {
            return MCP_ERR_PARSE;
        }
    } else {
        schema = cJSON_CreateObject();
        if (schema == NULL) {
            return MCP_ERR_OOM;
        }
        if (cJSON_AddStringToObject(schema, "type", "object") == NULL) {
            cJSON_Delete(schema);
            return MCP_ERR_OOM;
        }
    }

    if (tools_reserve(srv, srv->tools_count + 1u) != MCP_OK) {
        cJSON_Delete(schema);
        return MCP_ERR_OOM;
    }

    mcp_tool_t *t = &srv->tools[srv->tools_count];
    *t = (mcp_tool_t){0};
    t->name         = xstrdup(name);
    t->title        = xstrdup_or_null(title);
    t->description  = xstrdup_or_null(description);
    t->input_schema = schema;
    t->handler      = handler;
    t->userdata     = userdata;

    if (t->name == NULL
        || (title != NULL && t->title == NULL)
        || (description != NULL && t->description == NULL)) {
        tool_free(t);
        return MCP_ERR_OOM;
    }

    srv->tools_count += 1u;
    return MCP_OK;
}

/* ------------------------------------------------------------------ */
/*  Tool response builder                                             */
/* ------------------------------------------------------------------ */

int mcp_tool_response_add_text(mcp_tool_response_t *resp, const char *text)
{
    assert(resp != NULL);
    assert(resp->content != NULL);
    assert(text != NULL);

    cJSON *item = cJSON_CreateObject();
    if (item == NULL) {
        return MCP_ERR_OOM;
    }
    if (cJSON_AddStringToObject(item, "type", "text") == NULL
        || cJSON_AddStringToObject(item, "text", text) == NULL) {
        cJSON_Delete(item);
        return MCP_ERR_OOM;
    }
    if (!cJSON_AddItemToArray(resp->content, item)) {
        cJSON_Delete(item);
        return MCP_ERR_OOM;
    }
    return MCP_OK;
}

void mcp_tool_response_set_error(mcp_tool_response_t *resp, const char *message)
{
    assert(resp != NULL);
    assert(message != NULL);

    resp->is_error = true;
    cJSON_Delete(resp->error_msg);
    resp->error_msg = cJSON_CreateString(message);
    /* best-effort; if OOM, is_error still flags it upstream */
}
