#define _POSIX_C_SOURCE 200809L

#include "mcp_internal.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int write_message(FILE *out, cJSON *msg)
{
    assert(out != NULL);
    assert(msg != NULL);

    /* cJSON_PrintUnformatted produces no embedded newlines — required by
     * the MCP stdio transport (messages are newline-delimited). */
    char *text = cJSON_PrintUnformatted(msg);
    if (text == NULL) {
        return MCP_ERR_OOM;
    }
    /* Sanity: paranoia assertion — compact output must not contain \n. */
    assert(strchr(text, '\n') == NULL);

    int rc = MCP_OK;
    if (fputs(text, out) == EOF || fputc('\n', out) == EOF || fflush(out) != 0) {
        rc = MCP_ERR_IO;
    }
    free(text);
    return rc;
}

int mcp_server_run_stdio(mcp_server_t *srv)
{
    assert(srv != NULL);

    /* Unbuffer stdout: each response must be delivered without additional
     * buffering between the server and the client. */
    (void)setvbuf(stdout, NULL, _IONBF, 0);

    char   *line = NULL;
    size_t  cap  = 0;
    int     rc   = MCP_OK;

    while (true) {
        errno = 0;
        ssize_t n = getline(&line, &cap, stdin);
        if (n < 0) {
            if (feof(stdin)) {
                MCP_LOG_INFO("stdin EOF, exiting");
                rc = MCP_OK;
            } else {
                MCP_LOG_ERROR("stdin read failed: %s", strerror(errno));
                rc = MCP_ERR_IO;
            }
            break;
        }

        /* Trim trailing CR/LF; blank lines are skipped. */
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0) {
            continue;
        }

        cJSON *msg = cJSON_ParseWithLength(line, (size_t)n);
        if (msg == NULL) {
            MCP_LOG_WARN("parse error on input");
            cJSON *resp = cJSON_CreateObject();
            if (resp != NULL) {
                (void)cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
                cJSON *id = cJSON_CreateNull();
                if (id != NULL) {
                    (void)cJSON_AddItemToObject(resp, "id", id);
                }
                cJSON *err = cJSON_AddObjectToObject(resp, "error");
                if (err != NULL) {
                    (void)cJSON_AddNumberToObject(err, "code", JSONRPC_PARSE_ERROR);
                    (void)cJSON_AddStringToObject(err, "message", "parse error");
                }
                (void)write_message(stdout, resp);
                cJSON_Delete(resp);
            }
            continue;
        }

        cJSON *response = mcp_handle_message(srv, msg);
        cJSON_Delete(msg);

        if (response != NULL) {
            if (write_message(stdout, response) != MCP_OK) {
                MCP_LOG_ERROR("stdout write failed; exiting");
                cJSON_Delete(response);
                rc = MCP_ERR_IO;
                break;
            }
            cJSON_Delete(response);
        }
    }

    free(line);
    return rc;
}
