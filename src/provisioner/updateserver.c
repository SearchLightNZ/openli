/*
 *
 * Copyright (c) 2018 The University of Waikato, Hamilton, New Zealand.
 * All rights reserved.
 *
 * This file is part of OpenLI.
 *
 * This code has been developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * OpenLI is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenLI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#define _GNU_SOURCE

#include <string.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <microhttpd.h>
#include <json-c/json.h>
#include <pthread.h>

#include "provisioner.h"
#include "logger.h"
#include "util.h"
#include "updateserver.h"

#define MICRO_POST 0
#define MICRO_GET 1
#define MICRO_DELETE 2

static int send_http_page(struct MHD_Connection *connection, const char *page,
        int status_code) {

    int ret;
    struct MHD_Response *resp;

    resp = MHD_create_response_from_buffer(strlen(page), (void *)page,
            MHD_RESPMEM_MUST_COPY);
    if (!resp) {
        return MHD_NO;
    }
    MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE, "text/html");
    ret = MHD_queue_response(connection, status_code, resp);
    MHD_destroy_response(resp);
    return ret;
}

static int send_json_object(struct MHD_Connection *connection,
        json_object *jobj) {

    int ret;
    struct MHD_Response *resp;
    const char *jsonstr;

    if (jobj) {
        jsonstr = json_object_to_json_string(jobj);
        if (!jsonstr) {
            return MHD_NO;
        }

        resp = MHD_create_response_from_buffer(strlen(jsonstr), (void *)jsonstr,
                MHD_RESPMEM_MUST_COPY);
        if (!resp) {
            return MHD_NO;
        }

        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                "application/json");
        ret = MHD_queue_response(connection, MHD_HTTP_OK, resp);
    } else {

        resp = MHD_create_response_from_buffer(strlen(get404), (void *)get404,
                MHD_RESPMEM_MUST_COPY);
        if (!resp) {
            return MHD_NO;
        }

        MHD_add_response_header(resp, MHD_HTTP_HEADER_CONTENT_TYPE,
                "text/html");
        ret = MHD_queue_response(connection, MHD_HTTP_NOT_FOUND, resp);
    }
    MHD_destroy_response(resp);
    return ret;
}

static inline int extract_target_from_url(update_con_info_t *cinfo,
        char *url, char *targetspace, int spacelen, char *methodtype) {

    char *targetstart, *targetend, *urlstart;

    if (*url != '/') {
        logger(LOG_INFO, "OpenLI: invalid %s url from update socket: %s",
                methodtype, url);
        snprintf(cinfo->answerstring, 4096,
                "%s <p>OpenLI provisioner was unable to parse %s instruction from update socket. %s",
                update_failure_page_start, methodtype, update_failure_page_end);
        return -1;
    }

    urlstart = url;
    while (*urlstart == '/') {
        urlstart ++;
    }

    targetstart = strchr(urlstart, '/');
    if (targetstart == NULL) {
        return 0;
    }

    while (*(targetstart + 1) == '/') {
        targetstart ++;
    }

    targetend = strchrnul(targetstart + 1, '/');

    if (targetend - targetstart >= spacelen) {
        logger(LOG_INFO, "OpenLI: invalid %s url from update socket: %s",
                methodtype, url);
        snprintf(cinfo->answerstring, 4096,
                "%s <p>OpenLI provisioner was unable to parse %s instruction from update socket. %s",
                update_failure_page_start, methodtype, update_failure_page_end);
        return -1;
    }

    memcpy(targetspace, targetstart + 1, targetend - targetstart - 1);
    targetspace[targetend - (targetstart + 1)] = '\0';
    return 1;
}

static int update_configuration_delete(update_con_info_t *cinfo,
        provision_state_t *state, const char *url) {

    int ret = 0;
    char *urlcopy = strdup(url);
    char target[4096];

    if ((ret = extract_target_from_url(cinfo, urlcopy, target, 4096, "DELETE"))
             < 0) {
        free(urlcopy);
        return -1;
    }

    if (ret == 0) {
        /* no target specified, just return quietly? */
        free(urlcopy);
        return ret;
    }
    ret = 0;

    pthread_mutex_lock(&(state->interceptconf.safelock));
    switch(cinfo->target) {
        case TARGET_AGENCY:
            ret = remove_agency(cinfo, state, target);
            break;
        case TARGET_SIPSERVER:
            ret = remove_coreserver(cinfo, state, target,
                    OPENLI_CORE_SERVER_SIP);
            break;
        case TARGET_RADIUSSERVER:
            ret = remove_coreserver(cinfo, state, target,
                    OPENLI_CORE_SERVER_RADIUS);
            break;
        case TARGET_GTPSERVER:
            ret = remove_coreserver(cinfo, state, target,
                    OPENLI_CORE_SERVER_GTP);
            break;
        case TARGET_IPINTERCEPT:
            ret = remove_ip_intercept(cinfo, state, target);
            break;
        case TARGET_VOIPINTERCEPT:
            ret = remove_voip_intercept(cinfo, state, target);
            break;
        case TARGET_DEFAULTRADIUS:
            ret = remove_defaultradius(cinfo, state, target);
            break;
    }

    /* Safe to unlock before emitting, since all accesses should be reads
     * anyway... */
    pthread_mutex_unlock(&(state->interceptconf.safelock));
    emit_intercept_config(state->interceptconffile, &(state->interceptconf));
    free(urlcopy);
    return ret;
}

static json_object *create_get_response(update_con_info_t *cinfo,
        provision_state_t *state, const char *url) {

    json_object *jobj = NULL;
    int ret = 0;
    char *tgtptr = NULL;
    char *urlcopy = strdup(url);
    char target[4096];

    if ((ret = extract_target_from_url(cinfo, urlcopy, target, 4096, "GET"))
            < 0) {
        free(urlcopy);
        return NULL;
    }

    if (ret > 0) {
        tgtptr = target;
    }
    ret = 0;

    pthread_mutex_lock(&(state->interceptconf.safelock));
    switch(cinfo->target) {
        case TARGET_AGENCY:
            jobj = get_agency(cinfo, state, tgtptr);
            break;
        case TARGET_SIPSERVER:
            jobj = get_coreservers(cinfo, state, OPENLI_CORE_SERVER_SIP);
            break;
        case TARGET_RADIUSSERVER:
            jobj = get_coreservers(cinfo, state, OPENLI_CORE_SERVER_RADIUS);
            break;
        case TARGET_DEFAULTRADIUS:
            jobj = get_default_radius(cinfo, state);
            break;
        case TARGET_GTPSERVER:
            jobj = get_coreservers(cinfo, state, OPENLI_CORE_SERVER_GTP);
            break;
        case TARGET_IPINTERCEPT:
            jobj = get_ip_intercept(cinfo, state, tgtptr);
            break;
        case TARGET_VOIPINTERCEPT:
            jobj = get_voip_intercept(cinfo, state, tgtptr);
            break;
    }


    /* Safe to unlock before emitting, since all accesses should be reads
     * anyway... */
    pthread_mutex_unlock(&(state->interceptconf.safelock));
    free(urlcopy);
    return jobj;
}


static int update_configuration_post(update_con_info_t *cinfo,
        provision_state_t *state, const char *method) {

    int ret = 0;

    if (cinfo->content_type == NULL || strcasecmp(cinfo->content_type,
                "application/json") != 0) {
        return -1;
    }

    if (!cinfo->jsonbuffer) {
        return -1;
    }

    pthread_mutex_lock(&(state->interceptconf.safelock));
    switch(cinfo->target) {
        case TARGET_AGENCY:
            if (strcmp(method, "POST") == 0) {
                ret = add_new_agency(cinfo, state);
            }
            else {
                ret = modify_agency(cinfo, state);
            }
            break;
        case TARGET_SIPSERVER:
            ret = add_new_coreserver(cinfo, state, OPENLI_CORE_SERVER_SIP);
            break;
        case TARGET_RADIUSSERVER:
            ret = add_new_coreserver(cinfo, state, OPENLI_CORE_SERVER_RADIUS);
            break;
        case TARGET_DEFAULTRADIUS:
            ret = add_new_defaultradius(cinfo, state);
            break;
        case TARGET_GTPSERVER:
            ret = add_new_coreserver(cinfo, state, OPENLI_CORE_SERVER_GTP);
            break;
        case TARGET_IPINTERCEPT:
            if (strcmp(method, "POST") == 0) {
                ret = add_new_ipintercept(cinfo, state);
            } else {
                ret = modify_ipintercept(cinfo, state);
            }
            break;
        case TARGET_VOIPINTERCEPT:
            if (strcmp(method, "POST") == 0) {
                ret = add_new_voipintercept(cinfo, state);
            } else {
                ret = modify_voipintercept(cinfo, state);
            }
            break;
    }


    /* Safe to unlock before emitting, since all accesses should be reads
     * anyway... */
    pthread_mutex_unlock(&(state->interceptconf.safelock));

    emit_intercept_config(state->interceptconffile, &(state->interceptconf));
    return ret;
}

static int consume_upload_data(update_con_info_t *cinfo, const char *data,
        size_t size) {

    cinfo->jsonbuffer = realloc(cinfo->jsonbuffer, cinfo->jsonlen + size + 1);
    if (cinfo->jsonbuffer == NULL) {
        snprintf(cinfo->answerstring, 4096, "%s %s", update_failure_page_start,
                update_failure_page_end);
        cinfo->answercode = MHD_HTTP_INTERNAL_SERVER_ERROR;
        return MHD_NO;
    }

    memcpy(cinfo->jsonbuffer + cinfo->jsonlen, data, size);
    cinfo->jsonlen += size;
    cinfo->jsonbuffer[cinfo->jsonlen] = '\0';
    return MHD_YES;
}

void complete_update_request(void *cls, struct MHD_Connection *conn,
        void **con_cls, enum MHD_RequestTerminationCode toe) {

    update_con_info_t *cinfo = (update_con_info_t *)(*con_cls);

    if (cinfo == NULL) {
        return;
    }

    if (cinfo->connectiontype == MICRO_POST) {
        if (cinfo->jsonbuffer) {
            free(cinfo->jsonbuffer);
        }
    }

    free(cinfo);
    *con_cls = NULL;
}

int handle_update_request(void *cls, struct MHD_Connection *conn,
        const char *url, const char *method, const char *version,
        const char *upload_data, size_t *upload_data_size,
        void **con_cls) {

    update_con_info_t *cinfo;
    provision_state_t *provstate = (provision_state_t *)cls;
    int ret;

    if (*con_cls == NULL) {
        cinfo = calloc(1, sizeof(update_con_info_t));
        if (cinfo == NULL) {
            return MHD_NO;
        }

        if (strncmp(url, "/agency", 7) == 0) {
            cinfo->target = TARGET_AGENCY;
        } else if (strncmp(url, "/sipserver", 10) == 0) {
            cinfo->target = TARGET_SIPSERVER;
        } else if (strncmp(url, "/radiusserver", 13) == 0) {
            cinfo->target = TARGET_RADIUSSERVER;
        } else if (strncmp(url, "/gtpserver", 10) == 0) {
            cinfo->target = TARGET_GTPSERVER;
        } else if (strncmp(url, "/ipintercept", 12) == 0) {
            cinfo->target = TARGET_IPINTERCEPT;
        } else if (strncmp(url, "/voipintercept", 14) == 0) {
            cinfo->target = TARGET_VOIPINTERCEPT;
        } else if (strncmp(url, "/defaultradius", 14) == 0) {
            cinfo->target = TARGET_DEFAULTRADIUS;
        } else {
            free(cinfo);
            return MHD_NO;
        }

        if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
            cinfo->content_type = MHD_lookup_connection_value(conn,
                    MHD_HEADER_KIND, "Content-Type");

            cinfo->connectiontype = MICRO_POST;
            cinfo->answercode = MHD_HTTP_OK;
            snprintf(cinfo->answerstring, 4096, "%s", update_success_page);
        } else if (strcmp(method, "DELETE") == 0) {
            cinfo->connectiontype = MICRO_DELETE;
            cinfo->answercode = MHD_HTTP_OK;
            snprintf(cinfo->answerstring, 4096, "%s", update_success_page);
        } else {
            cinfo->connectiontype = MICRO_GET;
            cinfo->answercode = MHD_HTTP_OK;
        }

        *con_cls = (void *)cinfo;
        return MHD_YES;
    }


    if (strcmp(method, "GET") == 0) {
        json_object *respjson = NULL;
        cinfo = (update_con_info_t *)(*con_cls);

        respjson = create_get_response(cinfo, provstate, url);
        ret = send_json_object(conn, respjson);

        if (respjson) {
            json_object_put(respjson);
        }
        return ret;
    } else if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
        cinfo = (update_con_info_t *)(*con_cls);

        if (*upload_data_size != 0) {
            int ret = consume_upload_data(cinfo, upload_data, *upload_data_size);
            *upload_data_size = 0;
            return ret;
        } else {
            /* POST / PUT is complete */
            if (update_configuration_post(cinfo, provstate, method) < 0) {
                return send_http_page(conn, cinfo->answerstring,
                        MHD_HTTP_BAD_REQUEST);
            }

            return send_http_page(conn, cinfo->answerstring, cinfo->answercode);
        }
    } else if (strcmp(method, "DELETE") == 0) {
        cinfo = (update_con_info_t *)(*con_cls);

        if (update_configuration_delete(cinfo, provstate, url) < 0) {
            return send_http_page(conn, cinfo->answerstring,
                    MHD_HTTP_BAD_REQUEST);
        }
        return send_http_page(conn, cinfo->answerstring, cinfo->answercode);
    }

    return send_http_page(conn, unsupported_operation, MHD_HTTP_BAD_REQUEST);

}
// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
