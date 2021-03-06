/*
START OF LICENSE STUB
    DeDOS: Declarative Dispersion-Oriented Software
    Copyright (C) 2017 University of Pennsylvania, Georgetown University

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
END OF LICENSE STUB
*/
#include "webserver/read_msu.h"
#include "webserver/http_msu.h"
#include "webserver/sslops.h"
#include "socket_msu.h"
#include "webserver/connection-handler.h"
#include "routing_strategies.h"
#include "logging.h"
#include "msu_state.h"
#include "local_msu.h"
#include "msu_message.h"
#include "routing.h"
#include "local_files.h"
#include "msu_calls.h"
#include "unused_def.h"

#include <signal.h>

struct ws_read_state {
    int use_ssl;
};

static int handle_read(struct read_state *read_state,
                       struct ws_read_state *msu_state,
                       struct local_msu *self,
                       struct msu_msg *msg) {
    log(LOG_WEBSERVER_READ, "Attempting read from %p", read_state);
    int rtn = read_request(read_state);
    if (rtn & (WS_INCOMPLETE_WRITE | WS_INCOMPLETE_READ)) {
        read_state->conn.status = CON_READING;
        log(LOG_WEBSERVER_READ, "Read incomplete. Re-enabling (fd: %d)", read_state->conn.fd);
        msu_monitor_fd(read_state->conn.fd, RTN_TO_EVT(rtn), self, &msg->hdr);
        free(msg->data);
        return 0;
    } else if (rtn & WS_ERROR) {
        struct connection conn = read_state->conn;
        msu_free_state(self, &msg->hdr.key);
        log(LOG_WEBSERVER_READ, "Error when reading from socket!");
        msu_error(self, &msg->hdr, 1);
        log(LOG_WEBSERVER_READ, "Error when reading from socket!");
        msu_remove_fd_monitor(conn.fd);
        log(LOG_WEBSERVER_READ, "Error when reading from socket!");
        close_connection(&conn);
        return 0;
    } else {
        log(LOG_WEBSERVER_READ, "Read %s", read_state->req);
    }
    struct read_state *out = malloc(sizeof(*out));
    memcpy(out, read_state, sizeof(*read_state));
    msu_free_state(self, &msg->hdr.key);
    free(msg->data);
    call_msu_type(self, &WEBSERVER_HTTP_MSU_TYPE, &msg->hdr, sizeof(*out), out);
    return 0;
}

static int handle_accept(struct read_state *read_state,
                         struct ws_read_state *msu_state,
                         struct local_msu *self,
                         struct msu_msg *msg) {
    int rtn = accept_connection(&read_state->conn, msu_state->use_ssl);
    if (rtn & WS_COMPLETE) {
        rtn = handle_read(read_state, msu_state, self, msg);
        return rtn;
    } else if (rtn & WS_ERROR) {
        msu_error(self, NULL, 0);
        msu_remove_fd_monitor(read_state->conn.fd);
        close_connection(&read_state->conn);
        msu_free_state(self, &msg->hdr.key);
        free(msg->data);
        return -1;
    } else {
        read_state->conn.status = CON_SSL_CONNECTING;
        return msu_monitor_fd(read_state->conn.fd, RTN_TO_EVT(rtn), self, &msg->hdr);
    }
}

static int read_http_request(struct local_msu *self,
                             struct msu_msg *msg) {
    struct ws_read_state *msu_state = self->msu_state;

    struct connection conn_in;
    int sender_type_id = msu_msg_sender_type(&msg->hdr.provinance);
    switch (sender_type_id) {
        case SOCKET_MSU_TYPE_ID:;
            struct socket_msg *msg_in = msg->data;
            init_connection(&conn_in, msg_in->fd);
            break;
        case WEBSERVER_HTTP_MSU_TYPE_ID:
            log(LOG_WEBSERVER_READ, "Got back read msg %u", msg->hdr.key.id);
            conn_in = *(struct connection*)msg->data;
            break;
        default:
            log_error("Unknown sender MSU type ID: %d", sender_type_id);
            return -1;
    }

    size_t size = -1;
    struct read_state *read_state = msu_get_state(self, &msg->hdr.key, &size);
    if (read_state == NULL) {
        read_state = msu_init_state(self, &msg->hdr.key, sizeof(*read_state));
        init_read_state(read_state, &conn_in);
    } else {
        log(LOG_WEBSERVER_READ, "Retrieved read ptr %p", read_state);
    }
    if (read_state->conn.fd != conn_in.fd) {
        log_error("Got non-matching FDs! state: %d vs input: %d", read_state->conn.fd, conn_in.fd);
        return -1;
    }

    switch (read_state->conn.status) {
        case NIL:
        case NO_CONNECTION:
        case CON_ACCEPTED:
        case CON_SSL_CONNECTING:
            return handle_accept(read_state, msu_state, self, msg);
        case CON_READING:
            // TODO: I think it may never get here...
            return handle_read(read_state, msu_state, self, msg);
        default:
            log_error("Received unknown packet status: %d", read_state->conn.status);
            return -1;
    }
}

#define SSL_INIT_CMD "SSL"

static int ws_read_init(struct local_msu *self, struct msu_init_data *data) {
    struct ws_read_state *ws_state = malloc(sizeof(*ws_state));

    char *init_cmd = data->init_data;

    if (init_cmd == NULL) {
        log_info("Initializing NON-SSL webserver-reading MSU");
        ws_state->use_ssl = 0;
    }
    if (strncasecmp(init_cmd, SSL_INIT_CMD, sizeof(SSL_INIT_CMD)) == 0) {
        log_info("Initializing SSL webserver-reading MSU");
        ws_state->use_ssl = 1;
    } else {
        log_info("Initializing SSL webserver-reading MSU anyway");
        ws_state->use_ssl = 1;
    }

    self->msu_state = (void*)ws_state;
    return 0;
}

static void ws_read_destroy(struct local_msu *self) {
    free(self->msu_state);
}


static int init_ssl_ctx(struct msu_type UNUSED *type) {
    init_ssl_locks();
    if (init_ssl_context() != 0) {
        log_critical("Error intiializing SSL context");
        return -1;
    }

    char pem_file[256];
    get_local_file(pem_file, "mycert.pem");
    if (load_ssl_certificate(pem_file, pem_file) != 0) {
        log_error("Error loading SSL cert %s", pem_file);
        return -1;
    }

    signal(SIGPIPE, SIG_IGN);

    return 0;
}

static void destroy_ssl_ctx(struct msu_type UNUSED *type) {
    kill_ssl_locks();
}

struct msu_type WEBSERVER_READ_MSU_TYPE = {
    .name = "Webserver_read_MSU",
    .id = WEBSERVER_READ_MSU_TYPE_ID,
    .init_type = init_ssl_ctx,
    .destroy_type = destroy_ssl_ctx,
    .init = ws_read_init,
    .destroy = ws_read_destroy,
    .route = route_to_origin_runtime,
    .receive = read_http_request
};
