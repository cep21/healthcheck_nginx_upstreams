
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_upstream.h>

static char *ngx_http_upstream_check(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);
static char * ngx_http_upstream_check_http_send(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);
static char * ngx_http_upstream_check_smtp_send(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);
static char * ngx_http_upstream_check_http_expect_alive(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf); 
static char * ngx_http_upstream_check_smtp_expect_alive(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf); 

static char * ngx_http_upstream_check_shm_size(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);
static char * ngx_http_upstream_check_status_set_status(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf);

static void *ngx_http_upstream_check_create_main_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf);

static void * ngx_http_upstream_check_create_srv_conf(ngx_conf_t *cf);
static char * ngx_http_upstream_check_merge_srv_conf(ngx_conf_t *cf, 
        void *prev, void *conf);

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
        0,
        0,
        NULL },

    { ngx_string("check_http_send"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_upstream_check_http_send,
        0,
        0,
        NULL },

    { ngx_string("check_smtp_send"),
        NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1,
        ngx_http_upstream_check_smtp_send,
        0,
        0,
        NULL },

    { ngx_string("check_http_expect_alive"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_http_upstream_check_http_expect_alive,
        0,
        0,
        NULL },

    { ngx_string("check_smtp_expect_alive"),
        NGX_HTTP_UPS_CONF|NGX_CONF_1MORE,
        ngx_http_upstream_check_smtp_expect_alive,
        0,
        0,
        NULL },

    { ngx_string("check_shm_size"),
        NGX_HTTP_MAIN_CONF|NGX_CONF_TAKE1,
        ngx_http_upstream_check_shm_size,
        0,
        0,
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

    ngx_http_upstream_check_create_main_conf,    /* create main configuration */
    ngx_http_upstream_check_init_main_conf,    /* init main configuration */

    ngx_http_upstream_check_create_srv_conf, /* create server configuration */
    ngx_http_upstream_check_merge_srv_conf,  /* merge server configuration */

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

ngx_conf_t upstream_cf;

static char *
ngx_http_upstream_check(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) 
{
    ngx_str_t                     *value, s;
    ngx_uint_t                     i, rise, fall;
    ngx_msec_t                     interval, timeout;
    ngx_http_upstream_check_srv_conf_t  *uscf;

    /*set default*/
    rise = 2;
    fall = 5;
    interval = 30000;
    timeout = 1000;

    value = cf->args->elts;

    uscf = ngx_http_conf_get_module_srv_conf((&upstream_cf), ngx_http_upstream_check_module);
    if (uscf == NULL) {
        return NGX_CONF_ERROR;
    } 

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
ngx_http_upstream_check_http_send(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) 
{
    ngx_str_t                           *value;
    ngx_http_upstream_check_srv_conf_t  *uscf;

    value = cf->args->elts;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_check_module);

    uscf->send = *value;

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_smtp_send(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) 
{
    ngx_str_t                           *value;
    ngx_http_upstream_check_srv_conf_t  *uscf;

    value = cf->args->elts;

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_check_module);

    uscf->send = *value;

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_http_expect_alive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) 
{
    ngx_str_t                           *value;
    ngx_uint_t                           bit, i, m;
    ngx_conf_bitmask_t                  *mask;
    ngx_http_upstream_check_srv_conf_t  *uscf;

    value = cf->args->elts;
    mask = ngx_check_http_expect_alive_masks; 

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_check_module);
    bit = uscf->status_alive;

    for (i = 1; i < cf->args->nelts; i++) {
        for (m = 0; mask[m].name.len != 0; m++) {

            if (mask[m].name.len != value[i].len
                || ngx_strcasecmp(mask[m].name.data, value[i].data) != 0)
            {
                continue;
            }

            if (bit & mask[m].mask) {
                ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                                   "duplicate value \"%s\"", value[i].data);

            } else {
                bit |= mask[m].mask;
            }

            break;
        }

        if (mask[m].name.len == 0) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "invalid value \"%s\"", value[i].data);

            return NGX_CONF_ERROR;
        }
    }

    uscf->status_alive = bit;

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_smtp_expect_alive(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) 
{
    ngx_str_t                           *value;
    ngx_uint_t                           bit, i, m;
    ngx_conf_bitmask_t                  *mask;
    ngx_http_upstream_check_srv_conf_t  *uscf;

    value = cf->args->elts;
    mask = ngx_check_smtp_expect_alive_masks; 

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_check_module);
    bit = uscf->status_alive;

    for (i = 1; i < cf->args->nelts; i++) {
        for (m = 0; mask[m].name.len != 0; m++) {

            if (mask[m].name.len != value[i].len
                || ngx_strcasecmp(mask[m].name.data, value[i].data) != 0)
            {
                continue;
            }

            if (bit & mask[m].mask) {
                ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                                   "duplicate value \"%s\"", value[i].data);

            } else {
                bit |= mask[m].mask;
            }

            break;
        }

        if (mask[m].name.len == 0) {
            ngx_conf_log_error(NGX_LOG_WARN, cf, 0,
                               "invalid value \"%s\"", value[i].data);

            return NGX_CONF_ERROR;
        }
    }

    uscf->status_alive = bit;

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_shm_size(ngx_conf_t *cf, ngx_command_t *cmd, void *conf) 
{
    ngx_str_t                            *value;
    ngx_http_upstream_check_main_conf_t  *ucmcf; 

    ucmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_upstream_check_module);

    if (ucmcf->check_shm_size) {
        return "is duplicate";
    }

    value = cf->args->elts;

    ucmcf->check_shm_size = ngx_parse_size(&value[1]);
    if (ucmcf->check_shm_size == (size_t) NGX_ERROR) {
        return "invalid value";
    }

    return NGX_CONF_OK;
}


static char *
ngx_http_upstream_check_status_set_status(ngx_conf_t *cf, 
        ngx_command_t *cmd, void *conf) 
{
    ngx_str_t                               *value;
    ngx_http_core_loc_conf_t                *clcf;

    value = cf->args->elts;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);

    clcf->handler = ngx_http_upstream_check_status_handler;

    return NGX_CONF_OK;
}


static void *
ngx_http_upstream_check_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_check_main_conf_t  *ucmcf;

    ucmcf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_check_main_conf_t));
    if (ucmcf == NULL) {
        return NULL;
    }

    ucmcf->peers = ngx_pcalloc(cf->pool, sizeof(ngx_http_check_peers_t));
    if (ucmcf->peers == NULL) {
        return NULL;
    }

    if (ngx_array_init(&ucmcf->peers->peers, cf->pool, 16,
                sizeof(ngx_http_check_peer_t)) != NGX_OK)
    {
        return NULL;
    }

    return ucmcf;
}


static char *
ngx_http_upstream_check_init_main_conf(ngx_conf_t *cf, void *conf)
{

    if (ngx_http_upstream_init_main_check_conf(cf, conf) != NGX_OK) {
            return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}


static void *
ngx_http_upstream_check_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_check_srv_conf_t  *ucscf;

    ucscf = ngx_pcalloc(cf->pool, sizeof(ngx_http_upstream_check_srv_conf_t));
    if (ucscf == NULL) {
        return NULL;
    }

    ucscf->fall_count = NGX_CONF_UNSET_UINT;
    ucscf->rise_count = NGX_CONF_UNSET_UINT;
    ucscf->check_timeout = NGX_CONF_UNSET_MSEC;
    ucscf->check_type_conf = NGX_CONF_UNSET_PTR;

    upstream_cf = *cf;

    return ucscf;
}


static char *
ngx_http_upstream_check_merge_srv_conf(ngx_conf_t *cf, void *prev, void *conf)
{
    check_conf_t                        *check;
    ngx_http_upstream_check_srv_conf_t  *pcf = prev;

    if (pcf->fall_count == NGX_CONF_UNSET_UINT) {
        pcf->fall_count = 2;
    }

    if (pcf->rise_count == NGX_CONF_UNSET_UINT) {
        pcf->rise_count = 5;
    }

    if (pcf->check_interval == NGX_CONF_UNSET_MSEC) {
        pcf->check_interval = 0;
    }

    if (pcf->check_timeout == NGX_CONF_UNSET_MSEC) {
        pcf->check_timeout = 1000;
    }

    if (pcf->check_type_conf == NGX_CONF_UNSET_PTR) {
        pcf->check_type_conf = NULL;
    }

    check = pcf->check_type_conf;
    if (check) {
        if (pcf->send.len == 0) {
            pcf->send.data = check->default_send.data;
            pcf->send.len = check->default_send.len;
        }

        if (pcf->status_alive == 0) { 
            pcf->status_alive = check->default_status_alive;
        }
    }

    return NGX_CONF_OK;
}
