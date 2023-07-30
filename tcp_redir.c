/* vim: set et ts=4 sts=4 sw=4 : */
/********************************************************************\
 * This program is free software; you can redistribute it and/or    *
 * modify it under the terms of the GNU General Public License as   *
 * published by the Free Software Foundation; either version 2 of   *
 * the License, or (at your option) any later version.              *
 *                                                                  *
 * This program is distributed in the hope that it will be useful,  *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of   *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the    *
 * GNU General Public License for more details.                     *
 *                                                                  *
 * You should have received a copy of the GNU General Public License*
 * along with this program; if not, contact:                        *
 *                                                                  *
 * Free Software Foundation           Voice:  +1-617-542-5942       *
 * 59 Temple Place - Suite 330        Fax:    +1-617-542-2652       *
 * Boston, MA  02111-1307,  USA       gnu@gnu.org                   *
 *                                                                  *
\********************************************************************/

/** @file tcp_redir.c
    @brief xfrp tcp redirect service implemented
    @author Copyright (C) 2023 Dengfeng Liu <liu_df@qq.com>
*/

#include <pthread.h>
#include <arpa/inet.h>

#include "common.h"
#include "debug.h"
#include "config.h"
#include "tcp_redir.h"


// define a struct for tcp_redir which include proxy_service and event_base
struct tcp_redir_service {
    struct event_base *base;
    struct proxy_service *ps;
    struct sockaddr_in server_addr;
    struct bufferevent *bev_xfrps;
};             

// define a callback function for read event
static void read_cb(struct bufferevent *bev, void *arg)
{
    struct bufferevent *bev_out = (struct bufferevent *)arg;
    struct evbuffer *input = bufferevent_get_input(bev);
    struct evbuffer *output = bufferevent_get_output(bev_out);
    evbuffer_add_buffer(output, input);
}

// define a callback function for event event
static void event_cb(struct bufferevent *bev, short events, void *arg)
{
    struct bufferevent *partner = (struct bufferevent *)arg;
    if (events & BEV_EVENT_EOF) {
        debug(LOG_INFO, "connection closed");
        bufferevent_free(bev);
        bufferevent_free(partner);
    } else if (events & BEV_EVENT_ERROR) {
        debug(LOG_ERR, "some other error");
        bufferevent_free(bev);
        bufferevent_free(partner);
    }
    
}

// define a callback function for accept event
static void accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
    struct sockaddr *address, int socklen, void *arg)
{
    // the argument is the proxy_service
    struct tcp_redir_service *trs = (struct tcp_redir_service *)arg;
    struct event_base *base = trs->base;

    // read the data from the local port
    struct bufferevent *bev_in = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
    if (!bev_in) {
        debug(LOG_ERR, "create bufferevent for local port failed!");
        return;
    }

    struct bufferevent *bev_out = trs->bev_xfrps;
    
    bufferevent_setcb(bev_in, read_cb, NULL, event_cb, (void *)bev_out);
    bufferevent_setcb(bev_out, read_cb, NULL, event_cb, (void *)bev_in);
    bufferevent_enable(bev_in, EV_READ|EV_WRITE);
    bufferevent_enable(bev_out, EV_READ|EV_WRITE);

    debug(LOG_INFO, "connect to remote port success!");
    return;
}

// define a thread worker function for tcp_redir
static void *tcp_redir_worker(void *arg)
{
    struct proxy_service *ps = (struct proxy_service *)arg;
    struct common_conf *c_conf = get_common_config();
    // the worker is based on libevent and bufferevent
    // it listens on the local port and forward the data to the remote port
    // the local port and remote port are defined in the proxy_service
    // the proxy_service as argument is passed to the worker function

    // create a event_base
    struct evconnlistener *listener;
    struct event_base *base = event_base_new();
    if (!base) {
        debug(LOG_ERR, "create event base failed!");
        exit(1);
    }

    // define listen address and port
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(ps->local_port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    
    struct tcp_redir_service trs;
    trs.base = base;
    trs.ps = ps;
    trs.server_addr.sin_family = AF_INET;
    trs.server_addr.sin_port = htons(ps->remote_port);
    trs.server_addr.sin_addr.s_addr = inet_addr(c_conf->server_addr);
    // connect to the remote xfrpc service
    trs.bev_xfrps = bufferevent_socket_new(base, -1, BEV_OPT_CLOSE_ON_FREE|BEV_OPT_DEFER_CALLBACKS);
    if (!trs.bev_xfrps) {
        debug(LOG_ERR, "create bufferevent for remote xfrps service failed!");
        exit(1);
    }
    if (bufferevent_socket_connect(trs.bev_xfrps, (struct sockaddr *)&trs.server_addr, sizeof(trs.server_addr)) < 0) {
        debug(LOG_ERR, "connect to remote xfrps service [%s:%d] failed! error [%s]", 
            c_conf->server_addr, ps->remote_port, (errno));
        exit(1);
    }
    
    // create a listener
    listener = evconnlistener_new_bind(base, accept_cb, (void *)&trs,
        LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE, -1, (struct sockaddr *)&sin, sizeof(sin));
    if (!listener) {
        debug(LOG_ERR, "create listener failed!");
        exit(1);
    }

    // start the event loop
    event_base_dispatch(base);

    // free the listener
    evconnlistener_free(listener);
    // free the event base
    event_base_free(base);

    return NULL;
}

void start_tcp_redir_service(struct proxy_service *ps)
{
    // create a thread 
    pthread_t tid;
    if (pthread_create(&tid, NULL, tcp_redir_worker, (void *)ps) != 0) {
        debug(LOG_ERR, "create tcp_redir worker thread failed!");
        exit(1);
    }
    debug(LOG_INFO, "create tcp_redir worker thread success!");

    // detach the thread
    if (pthread_detach(tid) != 0) {
        debug(LOG_ERR, "detach tcp_redir worker thread failed!");
        exit(1);
    }
    debug(LOG_INFO, "detach tcp_redir worker thread success!");

    return;
}