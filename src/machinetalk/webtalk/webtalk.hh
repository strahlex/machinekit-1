/*
 * Copyright (C) 2013-2014 Michael Haberler <license@mah.priv.at>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"


#define PROXY_PORT 7681  // serves both http and ws

#ifndef SYSLOG_FACILITY
#define SYSLOG_FACILITY LOG_LOCAL1  // where all rtapi/ulapi logging goes
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <assert.h>
#include <errno.h>
#include <getopt.h>
#include <uuid/uuid.h>
#include <czmq.h>
#include <libwebsockets.h>

#define LWS_INITIAL_TXBUFFER 4096  // transmit buffer grows as needed
#define LWS_TXBUFFER_EXTRA 256   // add to current required size if growing tx buffer

#ifdef LWS_MAX_HEADER_LEN
#define MAX_HEADER_LEN LWS_MAX_HEADER_LEN
#else
#define MAX_HEADER_LEN 1024
#endif

// this extends enum lws_log_levels in libwebsockets.h
enum wt_log_levels {
    LLL_URI    = 1 << 10,
    LLL_TOWS   = 1 << 11,
    LLL_FROMWS = 1 << 12,
    LLL_LOOP   = 1 << 13,
    LLL_CONFIG = 1 << 14,
    LLL_ZWS    = 1 << 15,
    LLL_LAST = 15
};

#define lwsl_uri(...)    _lws_log(LLL_URI, __VA_ARGS__)
#define lwsl_tows(...)   _lws_log(LLL_TOWS, __VA_ARGS__)
#define lwsl_fromws(...) _lws_log(LLL_FROMWS, __VA_ARGS__)
#define lwsl_loop(...)   _lws_log(LLL_LOOP, __VA_ARGS__)
#define lwsl_cfg(...)    _lws_log(LLL_CONFIG, __VA_ARGS__)
#define lwsl_zws(...)    _lws_log(LLL_ZWS, __VA_ARGS__)


#include <inifile.h>
#include <syslog_async.h>

#include "mk-zeroconf.hh"
#include "select_interface.h"

#include <machinetalk/generated/message.pb.h>
namespace gpb = google::protobuf;

#include <json2pb.hh>
#include <jansson.h>
#include <uriparser/Uri.h>

typedef struct wtself wtself_t;
typedef struct zws_session_data zws_session_t;

// policy callback phases
typedef enum zwscvt_type {
    ZWS_CONNECTING,
    ZWS_ESTABLISHED,
    ZWS_CLOSE,
    ZWS_FROM_WS,
    ZWS_TO_WS,
} zwscb_type;

// return values:
// 0: success, -1: error; closes connection
// >0: invoke default policy callback
typedef  int (*zwscvt_cb)(wtself_t *self,         // server instance
			  zws_session_t *s,       // session
			  zwscb_type type);       // which callback

// per-session data
typedef struct zws_session_data {
    void *socket; // zmq destination
    zmq_pollitem_t pollitem;
    int socket_type;
    libwebsocket_write_protocol txmode;

    void *wsq_in;
    void *wsq_out;
    zmq_pollitem_t wsqin_pollitem;
    bool wsqin_poller_active; // false - disabled while send pipe choked

    // adapt to largest frame as we go
    unsigned char  *txbuffer;
    size_t txbufsize;

    void *user_data; // if any: allocate in ZWS_CONNECT, freed in ZWS_CLOSE

    // the current frame received from WS, for the ZWS_FROM_WS callback
    void *buffer;
    size_t length;

    zframe_t *current;   // partially sent frame (to ws)
    size_t already_sent; // how much of current was sent already

    // needed for websocket writable callback
    struct libwebsocket *wsiref;
    struct libwebsocket_context *ctxref;

    // URI/args state
    UriUriA u;
    UriQueryListA *queryList;

    // the policy applied to this session
    zwscvt_cb  policy;

    // stats counters:
    int wsin_bytes, wsin_msgs;
    int wsout_bytes, wsout_msgs;
    int zmq_bytes, zmq_msgs;

    int partial;
    int partial_retry;
    int completed;
} zws_session_t;

typedef struct zwspolicy {
    const char *name;
    zwscvt_cb  callback;
} zwspolicy_t;

typedef struct wtconf {
    const char *progname;
    const char *inifile;
    const char *section;
    const char *interfaces;
    const char *interface;
    const char *ipaddr;
    int debug;
    char *service_uuid;
    bool foreground;
    bool log_stderr;
    bool use_ssl;
    int service_timer;
    struct lws_context_creation_info info;
    char *index_html; // path to announce
    char *www_dir;
    unsigned ifIndex;
    int remote;
} wtconf_t;

typedef struct wtself {
    wtconf_t *cfg;
    uuid_t process_uuid;      // server instance (this process)
    int signal_fd;
    bool interrupted;
    pid_t pid;

    pb::Container rx; // any ParseFrom.. function does a Clear() first
    pb::Container tx; // tx must be Clear()'d after or before use

    zctx_t *ctx;
    zloop_t *loop;

    zlist_t *policies;
    struct libwebsocket_context *wsctx;
    int service_timer;

    AvahiCzmqPoll *av_loop;
    register_context_t *www_publisher;
    zservice_t zswww;

} wtself_t;



// wt_zeroconf.cc:
int wt_zeroconf_announce(wtself_t *self);
int wt_zeroconf_withdraw(wtself_t *self);

// webtalk_echo.cc:
void echo_thread(void *args, zctx_t *ctx, void *pipe);

// webtalk_proxy.cc:
int wt_proxy_new(wtself_t *self);
int wt_proxy_add_policy(wtself_t *self, const char *name, zwscvt_cb cb);
int service_timer_callback(zloop_t *loop, int  timer_id, void *context);
const char *zwsmimetype(const char *ext);

// webtalk_jsonpolicy.cc:
int json_policy(wtself_t *self, zws_session_t *wss, zwscb_type type);

// webtalk_defaultpolicy.cc:
int default_policy(wtself_t *self, zws_session_t *wss, zwscb_type type);


// webtalk_plugin.cc:
int wt_add_plugin(wtself_t *self, const char *sopath);
