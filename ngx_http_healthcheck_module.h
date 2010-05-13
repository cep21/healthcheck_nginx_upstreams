#ifndef _NGX_HEALTHCHECK_MODULE_H_
#define _NGX_HEALTHCHECK_MODULE_H_

#include <ngx_core.h>
#include <ngx_http.h>
#include <nginx.h>

// I don't define everything here, just the stuff external users will
// want to call

/**
 * Add a peer for healthchecking
 *
 * @param uscf The upstream the peer belongs to
 * @param peer The peer to check
 * @param pool Pool of memory to create peer checking data from
 *
 * @return Integer identifier for this healthcheck or NGX_ERROR if stuff
 * went bad.
 */
#if defined(nginx_version) && nginx_version >= 8022
ngx_int_t ngx_http_healthcheck_add_peer(ngx_http_upstream_srv_conf_t *uscf,
    ngx_addr_t *peer, ngx_pool_t *pool);
#else
ngx_int_t ngx_http_healthcheck_add_peer(ngx_http_upstream_srv_conf_t *uscf,
    ngx_peer_addr_t *peer, ngx_pool_t *pool);
#endif

/**
 * Check the health of a peer
 *
 * @param index Integer identifier index to check
 * @param log Gets warning and error messages
 * @return True if the given peer has failed its healthcheck
 */
ngx_int_t ngx_http_healthcheck_is_down(ngx_uint_t index, ngx_log_t *log);

#endif
