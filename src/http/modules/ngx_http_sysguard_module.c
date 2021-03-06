
/*
 * Copyright (C) 2010-2012 Alibaba Group Holding Limited
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


typedef struct {
    ngx_flag_t  enable;
    ngx_int_t   load;
    ngx_str_t   load_action;
    ngx_int_t   swap;
    ngx_str_t   swap_action;
    time_t      interval;

    ngx_uint_t  log_level;
} ngx_http_sysguard_conf_t;


static void *ngx_http_sysguard_create_conf(ngx_conf_t *cf);
static char *ngx_http_sysguard_merge_conf(ngx_conf_t *cf, void *parent,
    void *child);
static char *ngx_http_sysguard_load(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static char *ngx_http_sysguard_mem(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);
static ngx_int_t ngx_http_sysguard_init(ngx_conf_t *cf);


static ngx_conf_enum_t  ngx_http_sysguard_log_levels[] = {
    { ngx_string("info"), NGX_LOG_INFO },
    { ngx_string("notice"), NGX_LOG_NOTICE },
    { ngx_string("warn"), NGX_LOG_WARN },
    { ngx_string("error"), NGX_LOG_ERR },
    { ngx_null_string, 0 }
};


static ngx_command_t  ngx_http_sysguard_commands[] = {

    { ngx_string("sysguard"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_FLAG,
      ngx_conf_set_flag_slot,
      NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(ngx_http_sysguard_conf_t, enable),
      NULL },

    { ngx_string("sysguard_load"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE12,
      ngx_http_sysguard_load,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("sysguard_mem"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE12,
      ngx_http_sysguard_mem,
      NGX_HTTP_SRV_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("sysguard_interval"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_sec_slot,
      NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(ngx_http_sysguard_conf_t, interval),
      NULL },

    { ngx_string("sysguard_log_level"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_enum_slot,
      NGX_HTTP_SRV_CONF_OFFSET,
      offsetof(ngx_http_sysguard_conf_t, log_level),
      &ngx_http_sysguard_log_levels },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_sysguard_module_ctx = {
    NULL,                                   /* preconfiguration */
    ngx_http_sysguard_init,                 /* postconfiguration */

    NULL,                                   /* create main configuration */
    NULL,                                   /* init main configuration */

    ngx_http_sysguard_create_conf,          /* create server configuration */
    ngx_http_sysguard_merge_conf,           /* merge server configuration */

    NULL,                                   /* create location configuration */
    NULL                                    /* merge location configuration */
};


ngx_module_t  ngx_http_sysguard_module = {
    NGX_MODULE_V1,
    &ngx_http_sysguard_module_ctx,          /* module context */
    ngx_http_sysguard_commands,             /* module directives */
    NGX_HTTP_MODULE,                        /* module type */
    NULL,                                   /* init master */
    NULL,                                   /* init module */
    NULL,                                   /* init process */
    NULL,                                   /* init thread */
    NULL,                                   /* exit thread */
    NULL,                                   /* exit process */
    NULL,                                   /* exit master */
    NGX_MODULE_V1_PADDING
};


static time_t    ngx_http_sysguard_cached_exptime;
static ngx_int_t ngx_http_sysguard_cached_load;
static ngx_int_t ngx_http_sysguard_cached_swapstat;


static ngx_int_t
ngx_http_sysguard_update(ngx_http_request_t *r, time_t exptime)
{
    ngx_int_t       load, rc;
    ngx_meminfo_t   m;

    ngx_http_sysguard_cached_exptime = ngx_time() + exptime;

    rc = ngx_getloadavg(&load, 1, r->connection->log);
    if (rc == NGX_ERROR) {
        goto error;
    }

    rc = ngx_getmeminfo(&m, r->connection->log);
    if (rc == NGX_ERROR) {
        goto error;
    }

    ngx_http_sysguard_cached_load = load;

    ngx_http_sysguard_cached_swapstat = m.totalswap == 0
        ? 0 : (m.totalswap- m.freeswap) * 100 / m.totalswap;

    return NGX_OK;

error:

    ngx_http_sysguard_cached_load = 0;
    ngx_http_sysguard_cached_swapstat = 0;

    return NGX_ERROR;

}


static ngx_int_t
ngx_http_sysguard_do_redirect(ngx_http_request_t *r, ngx_str_t *path)
{
    if (path->len == 0) {
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    } else if (path->data[0] == '@') {
        (void) ngx_http_named_location(r, path);
    } else {
        (void) ngx_http_internal_redirect(r, path, &r->args);
    }

    ngx_http_finalize_request(r, NGX_DONE);

    return NGX_DONE;
}


static ngx_int_t
ngx_http_sysguard_handler(ngx_http_request_t *r)
{
    ngx_uint_t                updated;
    ngx_http_sysguard_conf_t *glcf;

    if (r->main->sysguard_set) {
        return NGX_DECLINED;
    }

    glcf = ngx_http_get_module_srv_conf(r, ngx_http_sysguard_module);

    if (!glcf->enable) {
        return NGX_DECLINED;
    }

    r->main->sysguard_set = 1;
    updated = 0;

    if (ngx_http_sysguard_cached_exptime < ngx_time()) {
        ngx_http_sysguard_update(r, glcf->interval);
        updated = 1;
    }

    ngx_log_debug7(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "http sysguard handler %d %d %d %d %V %V %V",
                   ngx_http_sysguard_cached_load,
                   glcf->load,
                   ngx_http_sysguard_cached_swapstat,
                   glcf->swap,
                   &r->uri,
                   &glcf->load_action,
                   &glcf->swap_action);

    if (glcf->load >= 0
        && ngx_http_sysguard_cached_load > glcf->load)
    {
        if (updated) {
            ngx_log_error(glcf->log_level, r->connection->log, 0,
                          "sysguard load limited, current:%d conf:%d",
                          ngx_http_sysguard_cached_load,
                          glcf->load);
        }

        return ngx_http_sysguard_do_redirect(r, &glcf->load_action);
    }

    if (glcf->swap >= 0
        && ngx_http_sysguard_cached_swapstat > glcf->swap)
    {
        if (updated) {
            ngx_log_error(glcf->log_level, r->connection->log, 0,
                          "sysguard swap limited, current:%d conf:%d",
                          ngx_http_sysguard_cached_swapstat,
                          glcf->swap);
        }

        return ngx_http_sysguard_do_redirect(r, &glcf->swap_action);
    }

    return NGX_DECLINED;
}


static void *
ngx_http_sysguard_create_conf(ngx_conf_t *cf)
{
    ngx_http_sysguard_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_sysguard_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->load_action = {0, NULL};
     *     conf->swap_action = {0, NULL};
     */

    conf->enable = NGX_CONF_UNSET;
    conf->load = NGX_CONF_UNSET;
    conf->swap = NGX_CONF_UNSET;
    conf->interval = NGX_CONF_UNSET;
    conf->log_level = NGX_CONF_UNSET_UINT;

    return conf;
}


static char *
ngx_http_sysguard_merge_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_sysguard_conf_t *prev = parent;
    ngx_http_sysguard_conf_t *conf = child;

    ngx_conf_merge_value(conf->enable, prev->enable, 0);
    ngx_conf_merge_str_value(conf->load_action, prev->load_action, "");
    ngx_conf_merge_str_value(conf->swap_action, prev->swap_action, "");
    ngx_conf_merge_value(conf->load, prev->load, -1);
    ngx_conf_merge_value(conf->swap, prev->swap, -1);
    ngx_conf_merge_value(conf->interval, prev->interval, 1);
    ngx_conf_merge_uint_value(conf->log_level, prev->log_level, NGX_LOG_ERR);

    return NGX_CONF_OK;
}

static char *
ngx_http_sysguard_load(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_sysguard_conf_t *glcf = conf;

    ngx_str_t  *value;
    ngx_uint_t  i;

    value = cf->args->elts;
    i = 1;

    if (ngx_strncmp(value[i].data, "load=", 5) == 0) {

        if (glcf->load != NGX_CONF_UNSET) {
            return "is duplicate";
        }

        if (value[i].len == 5) {
            goto invalid;
        }

        glcf->load = ngx_atofp(value[i].data + 5, value[i].len - 5, 3);
        if (glcf->load == NGX_ERROR) {
            goto invalid;
        }

        if (cf->args->nelts == 2) {
            return NGX_CONF_OK;
        }

        i++;

        if (ngx_strncmp(value[i].data, "action=", 7) != 0) {
            goto invalid;
        }

        if (value[i].len == 7) {
            goto invalid;
        }

        if (value[i].data[7] != '/' && value[i].data[7] != '@') {
            goto invalid;
        }

        glcf->load_action.data = value[i].data + 7;
        glcf->load_action.len = value[i].len - 7;

        return NGX_CONF_OK;
    }

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}


static char *
ngx_http_sysguard_mem(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_sysguard_conf_t *glcf = conf;

    ngx_str_t  *value;
    ngx_uint_t  i;

    value = cf->args->elts;
    i = 1;

    if (ngx_strncmp(value[i].data, "swapratio=", 10) == 0) {

        if (glcf->swap != NGX_CONF_UNSET) {
            return "is duplicate";
        }

        if (value[i].data[value[i].len - 1] != '%') {
            goto invalid;
        }

        glcf->swap = ngx_atofp(value[i].data + 10, value[i].len - 11, 2);
        if (glcf->swap == NGX_ERROR) {
            goto invalid;
        }

        if (cf->args->nelts == 2) {
            return NGX_CONF_OK;
        }

        i++;

        if (ngx_strncmp(value[i].data, "action=", 7) != 0) {
            goto invalid;
        }

        if (value[i].len == 7) {
            goto invalid;
        }

        if (value[i].data[7] != '/' && value[i].data[7] != '@') {
            goto invalid;
        }

        glcf->swap_action.data = value[i].data + 7;
        glcf->swap_action.len = value[i].len - 7;

        return NGX_CONF_OK;
    }

invalid:

    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                       "invalid parameter \"%V\"", &value[i]);

    return NGX_CONF_ERROR;
}


static ngx_int_t
ngx_http_sysguard_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt        *h;
    ngx_http_core_main_conf_t  *cmcf;

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_PREACCESS_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_sysguard_handler;

    return NGX_OK;
}
