/*
 * Does health checks of servers in an upstream
 *
 * Author: Jack Lindamood <jack facebook com>
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_healthcheck_module.h>
#ifdef NGX_SUPERVISORD_MODULE
#include <ngx_supervisord.h>
#if (NGX_SUPERVISORD_API_VERSION != 2)
  #error "ngx_http_upstream_fair_module requires NGX_SUPERVISORD_API v2"
#endif
#endif

#if (!NGX_HAVE_ATOMIC_OPS)
#error "Healthcheck module only works with atomic ops"
#endif

typedef enum {
    // In progress states
    NGX_HEALTH_UNINIT_STATE = 0,
    NGX_HEALTH_WAITING,
    NGX_HEALTH_SENDING_CHECK,
    NGX_HEALTH_READING_STAT_LINE,
    NGX_HEALTH_READING_STAT_CODE,
    NGX_HEALTH_READING_HEADER,
    NGX_HEALTH_HEADER_ALMOST_DONE,
    NGX_HEALTH_READING_BODY,
    // Good + final states
    NGX_HEALTH_OK = 100,
    // bad + final states
    NGX_HEALTH_BAD_HEADER = 200,
    NGX_HEALTH_BAD_STATUS,
    NGX_HEALTH_BAD_BODY,
    NGX_HEALTH_BAD_STATE,
    NGX_HEALTH_BAD_CONN,
    NGX_HEALTH_BAD_CODE,
    NGX_HEALTH_TIMEOUT,
    NGX_HEALTH_FULL_BUFFER,
    NGX_HEALTH_EARLY_CLOSE
} ngx_http_health_state;

typedef struct {
    // Worker pid processing this healthcheck
    ngx_pid_t                         owner;
    // matches the non shared memory index
    ngx_uint_t                        index;
    // Last time any action (read/write/timeout) was taken on this structure
    ngx_msec_t                        action_time;
    // Number of concurrent bad or good responses
    ngx_int_t concurrent;
    // How long this server's been concurrently bad or good
    ngx_msec_t                        since;
    // If true, the server's last response was bad
    unsigned last_down:1;
    // Code (above ngx_http_health_state) of last finished check
    ngx_http_health_state             down_code;
    // Used so multiple processes don't try to healthcheck the same peer
    ngx_atomic_t lock;
    /**
     * If true, the server is actually down.  This is
     * different than last_down because a server needs
     * X concurrent good or bad connections to actually
     * be down
     */
    ngx_atomic_t down;
} ngx_http_healthcheck_status_shm_t;


typedef struct {
    // Upstream this peer belongs to
    ngx_http_upstream_srv_conf_t    *conf;
    // The peer to check
#if defined(nginx_version) && nginx_version >= 8022
    ngx_addr_t                      *peer;
#else
    ngx_peer_addr_t                 *peer;
#endif
    // Index of the peer.  Matches shm segment and is used for 'down' checking
    //  by external clients
    ngx_uint_t                       index;
    // Current state of the healthcheck.  Different than shm->down_state
    // because this is an active state and that is a finisehd state.
    ngx_http_health_state            state;
    // Connection to the peer.  We reuse this memory each healthcheck, but
    // memset zero it
    ngx_peer_connection_t           *pc;
    // When the check began so we can diff it with action_time and time the
    // check out
    ngx_msec_t                       check_start_time;
    // Event that triggers a health check
    ngx_event_t                       health_ev;
    // Event that triggers an attempt at ownership of this healthcheck
    ngx_event_t                       ownership_ev;
    ngx_buf_t                        *read_buffer;
    // Where I am reading the entire connection, headers + body
    ssize_t                           read_pos;
    // Where I am in conf->health_expected (the body only)
    ssize_t                           body_read_pos;
    // Where I am in conf->health_send
    ssize_t                           send_pos;
    // HTTP status code returned (200, 404, etc)
    ngx_uint_t                        stat_code;
    ngx_http_healthcheck_status_shm_t *shm;
} ngx_http_healthcheck_status_t;

// This one is not shared. Created when the config is parsed
static ngx_array_t                   *ngx_http_healthchecks_arr;
// This is the same as the above data ->elts.  For ease of use
#define ngx_http_healthchecks \
  ((ngx_http_healthcheck_status_t*) ngx_http_healthchecks_arr->elts)
static ngx_http_healthcheck_status_shm_t *ngx_http_healthchecks_shm;

static ngx_int_t ngx_http_healthcheck_init(ngx_conf_t *cf);
static char* ngx_http_healthcheck_enabled(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char* ngx_http_healthcheck_delay(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char* ngx_http_healthcheck_timeout(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char* ngx_http_healthcheck_failcount(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char* ngx_http_healthcheck_send(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char* ngx_http_healthcheck_expected(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char* ngx_http_healthcheck_buffer(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf);
static char* ngx_http_set_healthcheck_status(ngx_conf_t *cf, ngx_command_t *cmd,
      void*conf);
static ngx_int_t ngx_http_healthcheck_procinit(ngx_cycle_t *cycle);
static ngx_int_t ngx_http_healthcheck_preconfig(ngx_conf_t *cf);
static ngx_int_t ngx_http_healthcheck_init_zone(ngx_shm_zone_t *shm_zone,
        void *data);
static ngx_int_t ngx_http_healthcheck_process_recv(
        ngx_http_healthcheck_status_t *stat);
static char* ngx_http_healthcheck_statestr(
        ngx_http_health_state state);

// I really wish there was a way to make nginx call this when you HUP the
// master
void ngx_http_healthcheck_clear_events(ngx_log_t *log);

static ngx_command_t  ngx_http_healthcheck_commands[] = {
    /**
     * If mentioned, enable healthchecks for this upstream
     */
    { ngx_string("healthcheck_enabled"),
        NGX_HTTP_UPS_CONF|NGX_CONF_NOARGS,
        ngx_http_healthcheck_enabled,
        0,
        0,
        NULL },
    /**
     * Delay in msec between healthchecks for a single peer
     */
    { ngx_string("healthcheck_delay"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_healthcheck_delay,
        0,
        0,
        NULL } ,
    /**
     * How long in msec a healthcheck is allowed to take place
     */
    { ngx_string("healthcheck_timeout"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_healthcheck_timeout,
        0,
        0,
        NULL },
    /**
     * Number of healthchecks good or bad in a row it takes to switch from
     * down to up and back.  Good to prevent flapping
     */
    { ngx_string("healthcheck_failcount"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_healthcheck_failcount,
        0,
        0,
        NULL } ,
    /**
     * What to send for the healthcheck.  Each argument is appended by \r\n
     * and the entire thing is suffixed with another \r\n.  For example,
     *
     *     healthcheck_send 'GET /health HTTP/1.1'
     *       'Host: www.facebook.com' 'Connection: close';
     *
     * Note that you probably want to end your health check with some directive
     * that closes the connection, like Connection: close.
     *
     */
    { ngx_string("healthcheck_send"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_http_healthcheck_send,
        0,
        0,
        NULL },
    /**
     * What to expect in the HTTP BODY, (meaning not the headers), in a correct
     * response
     */
    { ngx_string("healthcheck_expected"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_healthcheck_expected,
        0,
        0,
        NULL },
    /**
     * How big a buffer to use for the health check.  Remember to include
     * headers PLUS body, not just body.
     */
    { ngx_string("healthcheck_buffer"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_healthcheck_buffer,
        0,
        0,
        NULL },
    /**
     * When inside a /location block, replaced the HTTP body with backend
     * health status.  Use similarly to the stub_status module
     */
    { ngx_string("healthcheck_status"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_set_healthcheck_status,
      0,
      0,
      NULL },
    ngx_null_command
};


// Note: I tried using the "create server configuration" section rather than
// patching the nginx code, but it didn't work.  When you set the options
// you're in a different config context than when you use them in the upstream.
// It's very strange and unintuitive, but it's nginx

static ngx_http_module_t  ngx_http_healthcheck_module_ctx = {
    ngx_http_healthcheck_preconfig,        /* preconfiguration */
    ngx_http_healthcheck_init,             /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};

ngx_module_t  ngx_http_healthcheck_module = {
    NGX_MODULE_V1,
    &ngx_http_healthcheck_module_ctx,      /* module context */
    ngx_http_healthcheck_commands,         /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_http_healthcheck_procinit,         /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


void ngx_http_healthcheck_mark_finished(ngx_http_healthcheck_status_t *stat) {
#ifdef NGX_SUPERVISORD_MODULE
    ngx_http_upstream_rr_peers_t  *peers = stat->conf->peer.data;
#endif
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, stat->health_ev.log, 0,
            "healthcheck: Finished %V, state %d", &stat->peer->name,
            stat->state);
    if (stat->state == NGX_HEALTH_OK) {
        if (stat->shm->last_down) {
            stat->shm->last_down = 0;
            stat->shm->concurrent = 1;
            stat->shm->since = ngx_current_msec;
#ifdef NGX_SUPERVISORD_MODULE
            (void) ngx_supervisord_execute(stat->conf,
                                          NGX_SUPERVISORD_CMD_START,
                                          peers->peer[stat->index].onumber,
                                          NULL);
#endif
        } else {
            stat->shm->concurrent++;
        }
    } else {
        if (stat->shm->last_down) {
            stat->shm->concurrent++;
        } else {
            stat->shm->last_down = 1;
            stat->shm->concurrent = 1;
            stat->shm->since = ngx_current_msec;
#ifdef NGX_SUPERVISORD_MODULE
            (void) ngx_supervisord_execute(stat->conf,
                                          NGX_SUPERVISORD_CMD_STOP,
                                          peers->peer[stat->index].onumber,
                                          NULL);
#endif
        }
    }
    if (stat->shm->concurrent >= stat->conf->health_failcount) {
        stat->shm->down = stat->shm->last_down;
    }
    stat->shm->down_code = stat->state;
    ngx_close_connection(stat->pc->connection);
    stat->pc->connection = NULL;
    stat->state = NGX_HEALTH_WAITING;
    if (!ngx_terminate && !ngx_exiting && !ngx_quit) {
      ngx_add_timer(&stat->health_ev, stat->conf->health_delay);
    } else {
      ngx_http_healthcheck_clear_events(stat->health_ev.log);
    }
    stat->shm->action_time = ngx_current_msec;
}

void ngx_http_healthcheck_send_request(ngx_connection_t *);

void ngx_http_healthcheck_write_handler(ngx_event_t *wev) {
    ngx_connection_t        *c;

    c = wev->data;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, wev->log, 0,
            "healthcheck: Write handler called");

    ngx_http_healthcheck_send_request(c);
}

void ngx_http_healthcheck_send_request(ngx_connection_t *c) {
    ngx_http_healthcheck_status_t *stat = c->data;
    ssize_t size;

    if (stat->state != NGX_HEALTH_SENDING_CHECK) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                "healthcheck: Ignoring a write.  Not in writing state");
        return;
    }

    do {
        size =
            c->send(c, stat->conf->health_send.data + stat->send_pos,
                    stat->conf->health_send.len - stat->send_pos);
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, c->log, 0,
                "healthcheck: Send size %z", size);
        if (size == NGX_ERROR || size == 0) {
            // If the send fails, the connection is bad.  Close it out
            stat->state = NGX_HEALTH_BAD_CONN;
            ngx_http_healthcheck_mark_finished(stat);
            stat->shm->action_time = ngx_current_msec;
            break;
        } else if (size == NGX_AGAIN) {
            // I guess this means return and try again later
            break;
        } else {
            stat->shm->action_time = ngx_current_msec;
            stat->send_pos += size;
        }
    } while (stat->send_pos < (ssize_t)stat->conf->health_send.len);

    if (stat->send_pos > (ssize_t)stat->conf->health_send.len) {
        ngx_log_error(NGX_LOG_WARN, c->log, 0,
            "healthcheck: Logic error.  %z send pos bigger than buffer len %i",
                stat->send_pos, stat->conf->health_send.len);
    } else if (stat->send_pos == (ssize_t)stat->conf->health_send.len) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, c->log, 0,
                "healthcheck: Finished sending request");
        stat->state = NGX_HEALTH_READING_STAT_LINE;
    }
}

void ngx_http_healthcheck_read_handler(ngx_event_t *rev) {
    ngx_connection_t        *c;
    ngx_buf_t               *rb;
    ngx_int_t                rc;
    ssize_t size;
    ngx_http_healthcheck_status_t *stat;
    ngx_int_t                expect_finished;

    c = rev->data;
    stat = c->data;
    rb = stat->read_buffer;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0,
            "healthcheck: Read handler called");

    stat->shm->action_time = ngx_current_msec;
    if (ngx_current_msec - stat->check_start_time >=
            stat->conf->health_timeout) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0,
                "healthcheck: timeout!");
        stat->state = NGX_HEALTH_TIMEOUT;
        ngx_http_healthcheck_mark_finished(stat);
        return;
    }
    expect_finished = 0;
    do {
        size = c->recv(c, rb->pos, rb->end - rb->pos);
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, rev->log, 0,
                "healthcheck: Recv size %z when I wanted %O", size,
                rb->end - rb->pos);
        if (size == NGX_ERROR) {
            // If the send fails, the connection is bad.  Close it out
            stat->state = NGX_HEALTH_BAD_CONN;
            break;
        } else if (size == NGX_AGAIN) {
            break;
        } else if (size == 0) {
            expect_finished = 1;
            break;
        } else {
            rb->pos += size;
        }
    } while (rb->pos < rb->end);

    if (stat->state != NGX_HEALTH_BAD_CONN) {
        rc = ngx_http_healthcheck_process_recv(stat);
        switch (rc) {
            case NGX_AGAIN:
                if (expect_finished) {
                    stat->state = NGX_HEALTH_EARLY_CLOSE;
                    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, rev->log, 0,
                      "healthcheck: prematurely closed connection");
                } else if (rb->end == rb->pos) {
                    // We used up our read buffer and STILL can't verify
                    stat->state = NGX_HEALTH_FULL_BUFFER;
                    ngx_http_healthcheck_mark_finished(stat);
                }
                // We want more data to see if the body is OK or not
                break;
            case NGX_ERROR:
                ngx_http_healthcheck_mark_finished(stat);
                break;
            case NGX_OK:
                ngx_http_healthcheck_mark_finished(stat);
                break;
            default:
                ngx_log_error(NGX_LOG_WARN, rev->log, 0,
                        "healthcheck: Unknown process_recv code %i", rc);
                break;
        }
    } else {
        ngx_http_healthcheck_mark_finished(stat);
    }
}

static ngx_int_t ngx_http_healthcheck_process_recv(
        ngx_http_healthcheck_status_t *stat) {

    ngx_buf_t               *rb;
    u_char                   ch;
    ngx_str_t               *health_expected;

    rb = stat->read_buffer;
    health_expected = &stat->conf->health_expected;
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, stat->health_ev.log, 0,
            "healthcheck: Process recv");

    while (rb->start + stat->read_pos < rb->pos) {
        ch = *(rb->start+stat->read_pos);
        stat->read_pos++;
#if 0
        // Useful for debugging
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, stat->health_ev.log, 0,
                "healthcheck: CH %c state %d", ch, stat->state);
#endif
        switch (stat->state) {
            case NGX_HEALTH_READING_STAT_LINE:
                // Look for regex '/ \d+/'
                if (ch == ' ') {
                    stat->state = NGX_HEALTH_READING_STAT_CODE;
                    stat->stat_code = 0;
                } else if (ch == '\r' || ch == '\n') {
                    stat->state = NGX_HEALTH_BAD_STATUS;
                    return NGX_ERROR;
                }
                break;
            case NGX_HEALTH_READING_STAT_CODE:
                if (ch == ' ') {
                    if (stat->stat_code != NGX_HTTP_OK /*200*/) {
                        stat->state = NGX_HEALTH_BAD_CODE;
                        return NGX_ERROR;
                    } else {
                        stat->state = NGX_HEALTH_READING_HEADER;
                    }
                } else if (ch < '0' || ch > '9') {
                    stat->state = NGX_HEALTH_BAD_STATUS;
                    return NGX_ERROR;
                } else {
                    stat->stat_code = stat->stat_code * 10 + (ch - '0');
                }
                break;
            case NGX_HEALTH_READING_HEADER:
                if (ch == '\n') {
                    stat->state = NGX_HEALTH_HEADER_ALMOST_DONE;
                }
                break;
            case NGX_HEALTH_HEADER_ALMOST_DONE:
                if (ch == '\n') {
                    if (health_expected->len == NGX_CONF_UNSET_SIZE) {
                        stat->state = NGX_HEALTH_OK;
                        return NGX_OK;
                    } else {
                        stat->state = NGX_HEALTH_READING_BODY;
                    }
                } else if (ch != '\r') {
                    stat->state = NGX_HEALTH_READING_HEADER;
                }
                break;
            case NGX_HEALTH_READING_BODY:
                if (stat->body_read_pos == (ssize_t)health_expected->len) {
                    // Body was ok, but is now too long
                    stat->state = NGX_HEALTH_BAD_BODY;
                    return NGX_ERROR;
                } else if (ch != health_expected->data[stat->body_read_pos]) {
                    // Body was actually bad
                    stat->state = NGX_HEALTH_BAD_BODY;
                    return NGX_ERROR;
                } else {
                    stat->body_read_pos++;
                }
                break;
            default:
                ngx_log_error(NGX_LOG_CRIT, stat->health_ev.log, 0,
                  "healthcheck: Logic error.  Invalid state: %d",
                  stat->state);
                stat->state = NGX_HEALTH_BAD_STATE;
                return NGX_ERROR;
        }
    }
    if (stat->state == NGX_HEALTH_READING_BODY &&
          stat->body_read_pos == (ssize_t)health_expected->len) {
        stat->state = NGX_HEALTH_OK;
        return NGX_OK;
    } else if (stat->state == NGX_HEALTH_OK) {
        return NGX_OK;
    } else {
        return NGX_AGAIN;
    }
}

static void ngx_http_healthcheck_begin_healthcheck(ngx_event_t *event) {
    ngx_http_healthcheck_status_t * stat;
    ngx_connection_t        *c;
    ngx_int_t rc;

    stat = event->data;
    if (stat->state != NGX_HEALTH_WAITING) {
        ngx_log_error(NGX_LOG_WARN, event->log, 0,
                "healthcheck: State not waiting, is %d", stat->state);
    }
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, event->log, 0,
            "healthcheck: begun healthcheck of index %i", stat->index);

    ngx_memzero(stat->pc, sizeof(ngx_peer_connection_t));
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, event->log, 0,
            "healthcheck: Memzero done", stat->index);

    stat->pc->get = ngx_event_get_peer;

    stat->pc->sockaddr = stat->peer->sockaddr;
    stat->pc->socklen = stat->peer->socklen;
    stat->pc->name = &stat->peer->name;

    stat->pc->log = event->log;
    stat->pc->log_error = NGX_ERROR_ERR; // Um I guess (???)

    stat->pc->cached = 0;
    stat->pc->connection = NULL;
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, event->log, 0,
            "healthcheck: Connecting peer", stat->index);

    rc = ngx_event_connect_peer(stat->pc);
    if (rc == NGX_ERROR || rc == NGX_BUSY || rc == NGX_DECLINED) {
      ngx_log_error(NGX_LOG_CRIT, event->log, 0,
        "healthcheck: Could not connect to peer.  This is"
        " pretty bad and probably means your health checks won't"
        " work anymore: %i", rc);
      if (stat->pc->connection) {
          ngx_close_connection(stat->pc->connection);
      }
      // Try to do it again later, but if you're getting errors when you
      // try to connect to a peer, this probably won't work
      ngx_add_timer(&stat->health_ev, stat->conf->health_delay);
      return;
    }
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, event->log, 0,
      "healthcheck: connected so far");


    c = stat->pc->connection;
    c->data = stat;
    c->log = stat->pc->log;
    c->write->handler = ngx_http_healthcheck_write_handler;
    c->read->handler = ngx_http_healthcheck_read_handler;
    c->sendfile = 0;
    c->read->log = c->log;
    c->write->log = c->log;

    stat->state = NGX_HEALTH_SENDING_CHECK;
    stat->shm->action_time = ngx_current_msec;
    stat->read_pos = 0;
    stat->send_pos = 0;
    stat->body_read_pos = 0;
    stat->read_buffer->pos = stat->read_buffer->start;
    stat->read_buffer->last = stat->read_buffer->start;
    stat->check_start_time = ngx_current_msec;
    ngx_add_timer(c->read, stat->conf->health_timeout);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, event->log, 0,
            "healthcheck: Peer connected", stat->index);

    ngx_http_healthcheck_send_request(c);
}

static void ngx_http_healthcheck_try_for_ownership(ngx_event_t *event) {
    ngx_http_healthcheck_status_t * stat;
    ngx_int_t                       i_own_it;

    stat = event->data;
    if (ngx_terminate || ngx_exiting || ngx_quit) {
      ngx_http_healthcheck_clear_events(stat->health_ev.log);
      return;
    }

    i_own_it = 0;
    //  nxg_time_update(0, 0);
    //  Spinlock.  So don't own for a long time!
    //  Use spinlock so two worker processes don't try to healthcheck the same
    //  peer
    ngx_spinlock(&stat->shm->lock, ngx_pid, 1024);
    if (stat->shm->owner == ngx_pid) {
        i_own_it = 1;
    } else if (ngx_current_msec - stat->shm->action_time >=
               (stat->conf->health_delay + stat->conf->health_timeout) * 3) {
        stat->shm->owner = ngx_pid;
        stat->shm->action_time = ngx_current_msec;
        stat->state = NGX_HEALTH_WAITING;
        ngx_http_healthcheck_begin_healthcheck(&stat->health_ev);
        i_own_it = 1;
    }
    if (!ngx_atomic_cmp_set(&stat->shm->lock, ngx_pid, 0)) {
        ngx_log_error(NGX_LOG_CRIT, event->log, 0,
          "healthcheck: spinlock didn't work.  Should be %P, but isn't",
          ngx_pid);

        stat->shm->lock = 0;
    }
    if (!i_own_it) {
      // Try again for ownership later in case the guy that DOES own it dies or
      // something
        ngx_add_timer(&stat->ownership_ev, 5000);
    }
}

void ngx_http_healthcheck_clear_events(ngx_log_t *log) {
    ngx_uint_t i;
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, log, 0,
            "healthcheck: Clearing events");

    //  Note: From what I can tell it is safe to ngx_del_timer events
    //  that are not in the event tree
    for (i=0; i<ngx_http_healthchecks_arr->nelts; i++) {
        ngx_del_timer(&ngx_http_healthchecks[i].health_ev);
        ngx_del_timer(&ngx_http_healthchecks[i].ownership_ev);
    }
}

static ngx_int_t ngx_http_healthcheck_procinit(ngx_cycle_t *cycle) {
    ngx_uint_t i;
    ngx_msec_t t;

    if (ngx_http_healthchecks_arr->nelts == 0) {
      return NGX_OK;
    }

     // Otherwise, the distribution isn't very random because each process
     // is a fork, so they all have the same seed
    srand(ngx_pid);
    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, cycle->log, 0,
            "healthcheck: Adding events to worker process %P", ngx_pid);
    for (i=0; i<ngx_http_healthchecks_arr->nelts; i++) {
        ngx_http_healthchecks[i].shm = &ngx_http_healthchecks_shm[i];

        if (ngx_http_healthchecks[i].conf->healthcheck_enabled) {

            ngx_http_healthchecks[i].ownership_ev.handler =
                ngx_http_healthcheck_try_for_ownership;
            ngx_http_healthchecks[i].ownership_ev.log = cycle->log;
            ngx_http_healthchecks[i].ownership_ev.data =
                &ngx_http_healthchecks[i];
            // I'm not sure why the timer_set needs to be reset to zero.
            // It shouldn't (??), but it does when you HUP the process
            ngx_http_healthchecks[i].ownership_ev.timer_set = 0;

            ngx_http_healthchecks[i].health_ev.handler =
                ngx_http_healthcheck_begin_healthcheck;
            ngx_http_healthchecks[i].health_ev.log = cycle->log;
            ngx_http_healthchecks[i].health_ev.data =
                &ngx_http_healthchecks[i];
            ngx_http_healthchecks[i].health_ev.timer_set = 0;

            t = abs(ngx_random() % ngx_http_healthchecks[i].conf->health_delay);
            ngx_add_timer(&ngx_http_healthchecks[i].ownership_ev, t);
        }
    }
    return NGX_OK;
}

static ngx_int_t ngx_http_healthcheck_preconfig(ngx_conf_t *cf) {
    ngx_http_healthchecks_arr = ngx_array_create(cf->pool, 10,
            sizeof(ngx_http_healthcheck_status_t));
    if (ngx_http_healthchecks_arr == NULL) {
        return NGX_ERROR;
    }
    return NGX_OK;
}

static ngx_int_t ngx_http_healthcheck_init(ngx_conf_t *cf) {
    ngx_str_t        *shm_name;
    ngx_shm_zone_t   *shm_zone;
    ngx_uint_t         i;
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0,
      "healthcheck: healthcheck_init");

    if (ngx_http_healthchecks_arr->nelts == 0) {
      ngx_http_healthchecks_shm = NULL;
      return NGX_OK;
    }

    shm_name = ngx_palloc(cf->pool, sizeof *shm_name);
    shm_name->len = sizeof("http_healthcheck") - 1;
    shm_name->data = (unsigned char *) "http_healthcheck";

    // I guess a page each is good enough (?)
    shm_zone = ngx_shared_memory_add(cf, shm_name,
            ngx_pagesize * (ngx_http_healthchecks_arr->nelts + 1),
            &ngx_http_healthcheck_module);

    if (shm_zone == NULL) {
        return NGX_ERROR;
    }
    shm_zone->init = ngx_http_healthcheck_init_zone;

    for (i=0; i<ngx_http_healthchecks_arr->nelts; i++) {
      // It says 'temp', but it should last forever-ish
      ngx_http_healthchecks[i].read_buffer = ngx_create_temp_buf(cf->pool,
          ngx_http_healthchecks[i].conf->health_buffersize);
      if (ngx_http_healthchecks[i].read_buffer  == NULL) {
          return NGX_ERROR;
      }
    }

    return NGX_OK;
}

static ngx_int_t
ngx_http_healthcheck_init_zone(ngx_shm_zone_t *shm_zone, void *data) {
    ngx_uint_t                       i;
    ngx_slab_pool_t                *shpool;

    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, shm_zone->shm.log, 0,
      "healthcheck: Init zone");

    // If we're being HUP'd, I can't just use the same 'data' segment because
    // the number of servers may of changed.  Instead, I need to recreate a
    // slab

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    ngx_http_healthchecks_shm = ngx_slab_alloc(shpool,
            (sizeof (ngx_http_healthcheck_status_shm_t)) *
            ngx_http_healthchecks_arr->nelts);
    if (ngx_http_healthchecks_shm == NULL) {
        return NGX_ERROR;
    }
    for (i=0; i<ngx_http_healthchecks_arr->nelts; i++) {
        ngx_http_healthchecks_shm[i].index = i;
        ngx_http_healthchecks_shm[i].action_time = 0;
        ngx_http_healthchecks_shm[i].down = 0;
        ngx_http_healthchecks_shm[i].since = ngx_current_msec;
    }
    shm_zone->data = ngx_http_healthchecks_shm;

    return NGX_OK;
}


// --- BEGIN PUBLIC METHODS ---
ngx_int_t
ngx_http_healthcheck_add_peer(ngx_http_upstream_srv_conf_t *uscf,
#if defined(nginx_version) && nginx_version >= 8022
        ngx_addr_t *peer, ngx_pool_t *pool) {
#else
        ngx_peer_addr_t *peer, ngx_pool_t *pool) {
#endif
    ngx_http_healthcheck_status_t *status;
    status = ngx_array_push(ngx_http_healthchecks_arr);
    if (status == NULL) {
        return NGX_ERROR;
    }
    status->conf = uscf;
    status->peer = peer;
    status->index = ngx_http_healthchecks_arr->nelts - 1;
    status->pc = ngx_pcalloc(pool, sizeof(ngx_peer_connection_t));
    if (status->pc == NULL) {
        return NGX_ERROR;
    }
    return ngx_http_healthchecks_arr->nelts - 1;
}

ngx_int_t ngx_http_healthcheck_is_down(ngx_uint_t index, ngx_log_t *log) {
    if (index >= ngx_http_healthchecks_arr->nelts) {
        ngx_log_error(NGX_LOG_CRIT, log, 0,
            "healthcheck: Invalid index to is_down: %i", index);
        return 0;
    } else {
        return ngx_http_healthchecks[index].conf->healthcheck_enabled &&
          ngx_http_healthchecks[index].shm->down;
    }
}
// --- END PUBLIC METHODS ---

// Health status page
static char* ngx_http_healthcheck_statestr(
    ngx_http_health_state state) {
    switch (state) {
        case NGX_HEALTH_OK:
            return  "OK";
        case NGX_HEALTH_BAD_HEADER:
            return "Malformed header";
        case NGX_HEALTH_BAD_STATUS:
            return "Bad status line.  Maybe not HTTP";
        case NGX_HEALTH_BAD_BODY:
            return "Bad HTTP body contents";
        case NGX_HEALTH_BAD_STATE:
            return "Internal error.  Bad healthcheck state";
        case NGX_HEALTH_BAD_CONN:
            return "Error reading contents.  Bad connection";
        case NGX_HEALTH_BAD_CODE:
            return "Non 200 HTTP status code";
        case NGX_HEALTH_TIMEOUT:
            return "Healthcheck timed out";
        case NGX_HEALTH_FULL_BUFFER:
            return "Contents could not fit read buffer";
        case NGX_HEALTH_EARLY_CLOSE:
            return "Connection closed early";
        default:
            return "Unknown state";
  }
}

ngx_buf_t* ngx_http_healthcheck_buf_append(ngx_buf_t *dst, ngx_buf_t *src,
    ngx_pool_t *pool) {
  //TODO: Consider using a buffer chain
  ngx_buf_t *new_buf;
    if (dst->last + (src->last - src->pos) > dst->end) {
        new_buf = ngx_create_temp_buf(pool, ((dst->last - dst->pos) + (src->last - src->pos)) * 2 + 1);
        if (new_buf == NULL) {
            return NULL;
        }
        ngx_memcpy(new_buf->last, dst->pos, (dst->last - dst->pos));
        new_buf->last += (dst->last - dst->pos);
        // TODO: I don't think there's a way to uncreate the dst buffer (??)
        // Should be ok because these are small and cleared at the end of
        // the status request
        dst = new_buf;
    }
    ngx_memcpy(dst->last, src->pos, (src->last - src->pos));
    dst->last += (src->last - src->pos);
    return dst;
}

#define NGX_HEALTH_APPEND_CHECK(dst, src, pool) \
    do { \
        dst = ngx_http_healthcheck_buf_append(b, tmp, pool); \
        if (dst == NULL) { \
            return NGX_HTTP_INTERNAL_SERVER_ERROR; \
        } \
    } while (0);

static ngx_int_t ngx_http_healthcheck_status_handler(ngx_http_request_t *r) {
    ngx_int_t          rc;
    ngx_buf_t         *b, *tmp;
    ngx_chain_t        out;
    ngx_uint_t i;
    ngx_http_healthcheck_status_t *stat;
    ngx_http_healthcheck_status_shm_t *shm;
    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    ngx_str_t str_tmp = ngx_string("text/html; charset=utf-8");
    r->headers_out.content_type = str_tmp;

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    b = ngx_create_temp_buf(r->pool, 10);
    tmp = ngx_create_temp_buf(r->pool, 1000);
    if (b == NULL || tmp == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    tmp->last = ngx_snprintf(tmp->pos, tmp->end - tmp->pos,
        "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\n"
        "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
        "<head>\n"
        "  <title>NGINX Healthcheck status</title>\n"
        "</head>\n"
        "<body>\n"
        "<table border=\"1\">\n"
        "  <tr>\n"
        "    <th>Index</th>\n"
        "    <th>Name</th>\n"
        "    <th>Owner PID</th>\n"
        "    <th>Last action time</th>\n"
        "    <th>Concurrent status values</th>\n"
        "    <th>Time of concurrent values</th>\n"
        "    <th>Last response down</th>\n"
        "    <th>Last health status</th>\n"
        "    <th>Is down?</th>\n"
        "  </tr>\n");

    NGX_HEALTH_APPEND_CHECK(b, tmp, (r->pool));

    for (i=0; i<ngx_http_healthchecks_arr->nelts; i++) {
      stat = &ngx_http_healthchecks[i];
      shm  = stat->shm;

      tmp->last = ngx_snprintf(tmp->pos, tmp->end - tmp->pos,
        "  <tr>\n"
        "    <td>%i</td>\n" // Index
        "    <td>%V</td>\n" // Name
        "    <td>%P</td>\n" // PID
        "    <td>%M</td>\n" // action time
        "    <td>%i</td>\n" // concurrent status values
        "    <td>%M</td>\n" // Time concurrent
        "    <td>%d</td>\n" // Last response down?
        "    <td>%s</td>\n" // Code of last response
        "    <td>%A</td>\n" // Is down?
        "  </tr>\n", stat->index, &stat->peer->name, shm->owner,
                     shm->action_time, shm->concurrent,
                     shm->since, (int)shm->last_down,
                     ngx_http_healthcheck_statestr(shm->down_code),
                     shm->down);
      NGX_HEALTH_APPEND_CHECK(b, tmp, r->pool);
    }

    tmp->last = ngx_snprintf(tmp->pos, tmp->end - tmp->pos,
        "</table>\n"
        "</body>\n"
        "</html>\n");
    NGX_HEALTH_APPEND_CHECK(b, tmp, r->pool);

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf = 1;
    out.buf = b;
    out.next = NULL;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}
#undef NGX_HEALTH_APPEND_CHECK
// end health status page

//
//
// BEGIN THE BORING PART: Setting config variables
//
//

static char* ngx_http_healthcheck_enabled(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf) {
    ngx_http_upstream_srv_conf_t  *uscf;
    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscf->healthcheck_enabled = 1;
    return NGX_CONF_OK;
}

static char* ngx_http_healthcheck_delay(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf) {
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_str_t *value;
    value = cf->args->elts;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscf->health_delay = (ngx_uint_t)ngx_atoi(value[1].data, value[1].len);
    if (uscf->health_delay == NGX_ERROR) {
        return "Invalid healthcheck delay";
    }
    return NGX_CONF_OK;
}
static char* ngx_http_healthcheck_timeout(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf) {
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_str_t *value;
    value = cf->args->elts;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscf->health_timeout = ngx_atoi(value[1].data, value[1].len);
    if (uscf->health_timeout == (ngx_msec_t)NGX_ERROR) {
        return "Invalid healthcheck timeout ";
    }
    return NGX_CONF_OK;
}
static char* ngx_http_healthcheck_failcount(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf) {
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_str_t *value;
    value = cf->args->elts;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscf->health_failcount = ngx_atoi(value[1].data, value[1].len);
    if (uscf->health_failcount == NGX_ERROR) {
        return "Invalid healthcheck failcount";
    }
    return NGX_CONF_OK;
}
static char* ngx_http_healthcheck_send(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf) {
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_str_t *value;
    ngx_int_t  num;
    int i;
    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    value = cf->args->elts;
    num = cf->args->nelts;
    uscf->health_send.len  = 0;
    size_t at;
    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    for (i = 1; i<num; i++) {
        if (i !=1) {
            uscf->health_send.len += 2; // \r\n
        }
        uscf->health_send.len += value[i].len;
    }
    uscf->health_send.len += (sizeof(CRLF) - 1) * 2;
    uscf->health_send.data = ngx_pnalloc(cf->pool, uscf->health_send.len + 1);
    if (uscf->health_send.data == NULL) {
        return "Unable to alloc data to send";
    }
    at = 0;
    for (i = 1; i<num; i++) {
        if (i !=1) {
            ngx_memcpy(uscf->health_send.data + at, CRLF, sizeof(CRLF) - 1);
            at += sizeof(CRLF) - 1;
        }
        ngx_memcpy(uscf->health_send.data + at, value[i].data, value[i].len);
        at += value[i].len;
    }
    ngx_memcpy(uscf->health_send.data + at, CRLF CRLF, (sizeof(CRLF) - 1) * 2);
    at += (sizeof(CRLF) - 1) * 2;
    uscf->health_send.data[at] = 0;
    if (at != uscf->health_send.len) {
        return "healthcheck: Logic error.  Length doesn't match";
    }

    return NGX_CONF_OK;
}

static char* ngx_http_healthcheck_expected(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf) {
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_str_t *value;
    value = cf->args->elts;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscf->health_expected.data = value[1].data;
    uscf->health_expected.len  = value[1].len;

    return NGX_CONF_OK;
}

static char* ngx_http_healthcheck_buffer(ngx_conf_t *cf, ngx_command_t *cmd,
        void *conf) {
    ngx_http_upstream_srv_conf_t  *uscf;
    ngx_str_t *value;
    value = cf->args->elts;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    uscf->health_buffersize = ngx_atoi(value[1].data, value[1].len);
    if (uscf->health_buffersize == NGX_ERROR) {
        return "Invalid healthcheck buffer size";
    }
    return NGX_CONF_OK;
}


static char* ngx_http_set_healthcheck_status(ngx_conf_t *cf, ngx_command_t *cmd,
      void*conf) {

    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_healthcheck_status_handler;

    return NGX_CONF_OK;
}

#undef ngx_http_healthchecks
