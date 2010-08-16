
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_upstream.h>

static char *ngx_http_upstream_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char * ngx_http_upstream_check_status_set_status(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);

static ngx_conf_bitmask_t  ngx_check_http_expect_alive_masks[] = {
    { ngx_string("http_2xx"), NGX_CHECK_HTTP_2XX },
    { ngx_string("http_3xx"), NGX_CHECK_HTTP_3XX },
    { ngx_string("http_4xx"), NGX_CHECK_HTTP_4XX },
    { ngx_string("http_5xx"), NGX_CHECK_HTTP_5XX },
    { ngx_null_string, 0 }
};

static ngx_conf_bitmask_t  ngx_check_smtp_expect_alive_masks[] = {
    { ngx_string("smtp_2xx"), NGX_CHECK_SMTP_2XX },
    { ngx_string("smtp_3xx"), NGX_CHECK_SMTP_3XX },
    { ngx_string("smtp_4xx"), NGX_CHECK_SMTP_4XX },
    { ngx_string("smtp_5xx"), NGX_CHECK_SMTP_5XX },
    { ngx_null_string, 0 }
};

static ngx_command_t  ngx_http_upstream_check_commands[] = {

    { ngx_string("check"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_http_upstream_check,
        NGX_HTTP_SRV_CONF_OFFSET,
        0,
        NULL },

    { ngx_string("check_http_send"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_SRV_CONF_OFFSET,
        offsetof(ngx_http_upstream_srv_conf_t, send),
        NULL },

    { ngx_string("check_smtp_send"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_str_slot,
        NGX_HTTP_SRV_CONF_OFFSET,
        offsetof(ngx_http_upstream_srv_conf_t, send),
        NULL },

    { ngx_string("check_http_expect_alive"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_conf_set_bitmask_slot,
        NGX_HTTP_SRV_CONF_OFFSET,
        offsetof(ngx_http_upstream_srv_conf_t, status_alive),
        &ngx_check_http_expect_alive_masks },

    { ngx_string("check_smtp_expect_alive"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_conf_set_bitmask_slot,
        NGX_HTTP_SRV_CONF_OFFSET,
        offsetof(ngx_http_upstream_srv_conf_t, status_alive),
        &ngx_check_smtp_expect_alive_masks },

    { ngx_string("check_shm_size"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_conf_set_size_slot,
        NGX_HTTP_MAIN_CONF_OFFSET,
        offsetof(ngx_http_upstream_main_conf_t, check_shm_size),
        NULL },

    { ngx_string("check_status"),
        NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
        ngx_http_upstream_check_status_set_status,
        0,
        0,
        NULL },

    ngx_null_command
};

static ngx_http_module_t  ngx_http_upstream_check_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};

ngx_module_t  ngx_http_upstream_check_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_check_module_ctx,   /* module context */
    ngx_http_upstream_check_commands,      /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    ngx_http_check_init_process,           /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static char *
ngx_http_upstream_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) 
{
    ngx_str_t                     *value, s;
    ngx_uint_t                     i, rise, fall;
    ngx_msec_t                     interval, timeout;
    ngx_http_upstream_srv_conf_t  *uscf = conf;

    /*set default*/
    rise = 2;
    fall = 5;
    interval = 30000;
    timeout = 1000;

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {

        if (ngx_strncmp(value[i].data, "type=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            uscf->check_type_conf = ngx_http_get_check_type_conf(&s);

            if ( uscf->check_type_conf == NULL) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "interval=", 9) == 0) {
            s.len = value[i].len - 9;
            s.data = value[i].data + 9;

            interval = ngx_atoi(s.data, s.len);
            if (interval == (ngx_msec_t) NGX_ERROR) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "timeout=", 8) == 0) {
            s.len = value[i].len - 8;
            s.data = value[i].data + 8;

            timeout = ngx_atoi(s.data, s.len);
            if (timeout == (ngx_msec_t) NGX_ERROR) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "rise=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            rise = ngx_atoi(s.data, s.len);
            if (rise == (ngx_uint_t) NGX_ERROR) {
                goto invalid_check_parameter;
            }

            continue;
        }

        if (ngx_strncmp(value[i].data, "fall=", 5) == 0) {
            s.len = value[i].len - 5;
            s.data = value[i].data + 5;

            fall = ngx_atoi(s.data, s.len);
            if (fall == (ngx_uint_t) NGX_ERROR) {
                goto invalid_check_parameter;
            }

            continue;
        }

        goto invalid_check_parameter;
    }

    uscf->check_interval = interval;
    uscf->check_timeout = timeout;
    uscf->fall_count = fall;
    uscf->rise_count = rise;

    if (uscf->check_type_conf == NULL) {
        s.len = sizeof("http") - 1;
        s.data =(u_char *) "http";

        uscf->check_type_conf = ngx_http_get_check_type_conf(&s);
    }

    return NGX_CONF_OK;

invalid_check_parameter:
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
            "invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}

static char *
ngx_http_upstream_check_status_set_status(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf) {

    ngx_http_core_loc_conf_t                *clcf;
    ngx_str_t                               *value;

    value = cf->args->elts;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_upstream_check_status_handler;

    return NGX_CONF_OK;
}
