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

/** @file proxy_tcp.c
    @brief xfrp proxy tcp implemented
    @author Copyright (C) 2016 Dengfeng Liu <liu_df@qq.com>
*/

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <syslog.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <event2/bufferevent.h>
#include <event2/buffer.h>
#include <event2/listener.h>
#include <event2/event.h>

#include "debug.h"
#include "uthash.h"
#include "common.h"
#include "proxy.h"
#include "config.h"
#include "tcpmux.h"
#include "control.h"

#define	BUF_LEN	2*1024

static int is_socks5(uint8_t *buf, int len)
{
	if (len < 3)
		return 0;
	if (buf[0] != 0x05)
		return 0;
	if (buf[1] != 0x01)
		return 0;
	if (buf[2] != 0x00)
		return 0;
	return 1;
}

/**
 * @brief Parse SOCKS5 address structure from ring buffer
 *
 * Parses a SOCKS5 address structure which can be one of:
 * - IPv4 (type 0x01): 4 bytes address + 2 bytes port
 * - IPv6 (type 0x04): 16 bytes address + 2 bytes port  
 * - Domain (type 0x03): 1 byte length + domain + 2 bytes port
 *
 * @param rb Ring buffer containing the SOCKS5 address data
 * @param len Total length of data in ring buffer
 * @param offset Returns number of bytes processed
 * @param addr Output parameter for parsed address structure
 * @return 1 on success, 0 on failure/invalid format
 */
static int parse_socks5_addr(struct ring_buffer *rb, int len, int *offset, 
							struct socks5_addr *addr)
{
	assert(addr && rb && offset);
	assert(len > 0);

	// Initialize
	memset(addr, 0, sizeof(struct socks5_addr));
	uint8_t buf[256] = {0};  // Increased buffer size to handle domains
	
	// Read address type
	rx_ring_buffer_pop(rb, buf, 1);
	*offset = 1;
	addr->type = buf[0];

	// Parse based on address type
	switch(addr->type) {
		case 0x01:  // IPv4
			if (len < 7) return 0;
			rx_ring_buffer_pop(rb, buf+1, 6);
			memcpy(addr->addr, buf+1, 4);     // IPv4 address
			memcpy(&addr->port, buf+5, 2);    // Port
			*offset = 7;
			break;

		case 0x04:  // IPv6
			if (len < 19) return 0;
			rx_ring_buffer_pop(rb, buf+1, 18);
			memcpy(addr->addr, buf+1, 16);    // IPv6 address
			memcpy(&addr->port, buf+17, 2);   // Port
			*offset = 19;
			break;

		case 0x03:  // Domain name
			if (len < 2) return 0;
			rx_ring_buffer_pop(rb, buf+1, 1); // Domain length
			uint8_t domain_len = buf[1];
			
			if (len < domain_len + 4) return 0;
			rx_ring_buffer_pop(rb, buf+2, domain_len + 2);
			memcpy(addr->addr, buf+2, domain_len);  // Domain
			memcpy(&addr->port, buf+2+domain_len, 2); // Port
			*offset = domain_len + 4;
			break;

		default:
			return 0;
	}

	return 1;
}

/**
 * @brief Establish a SOCKS5 proxy connection based on destination address
 *
 * This function creates and establishes a proxy connection to the target address
 * specified in the SOCKS5 address structure. It supports:
 * - IPv4 addresses (type 0x01)
 * - Domain names (type 0x03) 
 * - IPv6 addresses (type 0x04)
 *
 * @param client Proxy client structure containing event base
 * @param addr SOCKS5 address structure with connection details
 * @return Configured bufferevent on success, NULL on failure
 */
static struct bufferevent *socks5_proxy_connect(struct proxy_client *client, struct socks5_addr *addr)
{
	struct bufferevent *bev = NULL;

	// Create new socket bufferevent
	bev = bufferevent_socket_new(client->base, -1, BEV_OPT_CLOSE_ON_FREE);
	if (!bev) {
		debug(LOG_ERR, "Failed to create bufferevent for SOCKS5 proxy");
		return NULL;
	}

	int connect_result = -1;
	switch(addr->type) {
		case 0x01: { // IPv4
			struct sockaddr_in sin = {
				.sin_family = AF_INET,
				.sin_port = addr->port
			};
			memcpy(&sin.sin_addr, addr->addr, 4);

			// Log connection details
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, addr->addr, ip, INET_ADDRSTRLEN);
			debug(LOG_DEBUG, "SOCKS5 connecting to IPv4: %s:%d", ip, ntohs(addr->port));

			connect_result = bufferevent_socket_connect(bev, 
				(struct sockaddr *)&sin, sizeof(sin));
			break;
		}

		case 0x03: // Domain name
			debug(LOG_DEBUG, "SOCKS5 connecting to domain: %s:%d", 
				addr->addr, ntohs(addr->port));
			connect_result = bufferevent_socket_connect_hostname(bev,
				get_main_control()->dnsbase, AF_INET, 
				(char *)addr->addr, ntohs(addr->port));
			break;

		case 0x04: { // IPv6
			struct sockaddr_in6 sin6 = {
				.sin6_family = AF_INET6,
				.sin6_port = addr->port
			};
			memcpy(&sin6.sin6_addr, addr->addr, 16);

			connect_result = bufferevent_socket_connect(bev,
				(struct sockaddr *)&sin6, sizeof(sin6));
			break;
		}

		default:
			debug(LOG_ERR, "Invalid SOCKS5 address type: %d", addr->type);
			bufferevent_free(bev);
			return NULL;
	}

	if (connect_result < 0) {
		debug(LOG_ERR, "Failed to connect SOCKS5 proxy (type %d)", addr->type);
		bufferevent_free(bev);
		return NULL;
	}

	// Setup callbacks and enable bufferevent
	bufferevent_setcb(bev, tcp_proxy_c2s_cb, NULL, xfrp_proxy_event_cb, client);
	bufferevent_enable(bev, EV_READ | EV_WRITE);

	return bev;
}

/**
 * @brief Legacy SOCKS5 protocol handler 
 * 
 * This function implements a simplified SOCKS5 protocol handler that supports:
 * - Initial direct connection request (SOCKS5_INIT)
 * - Data forwarding in established state (SOCKS5_ESTABLISHED)
 *
 * @param client The proxy client structure
 * @param rb Ring buffer containing incoming data
 * @param len Length of data in ring buffer
 * @return Number of bytes processed, 0 on error
 * 
 * @deprecated Use handle_socks5() instead which implements full SOCKS5 protocol
 */
uint32_t handle_ss5(struct proxy_client *client, struct ring_buffer *rb, int len) 
{
	uint32_t bytes_processed = 0;

	// Handle established connection state
	if (client->state == SOCKS5_ESTABLISHED) {
		assert(client->local_proxy_bev);
		tx_ring_buffer_write(client->local_proxy_bev, rb, len);
		return len;
	}

	// Handle initial connection request
	if (client->state == SOCKS5_INIT && len >= 7) {
		debug(LOG_DEBUG, "Processing initial SOCKS5 connection request, len: %d", len);

		// Parse destination address
		int addr_len = 0;
		if (!parse_socks5_addr(rb, len, &addr_len, &client->remote_addr)) {
			debug(LOG_ERR, "Failed to parse SOCKS5 address");
			return bytes_processed;
		}

		// Establish proxy connection
		client->local_proxy_bev = socks5_proxy_connect(client, &client->remote_addr);
		if (!client->local_proxy_bev) {
			debug(LOG_ERR, "Failed to establish proxy connection");
			return bytes_processed;
		}

		debug(LOG_DEBUG, "SOCKS5 proxy connection established (parsed %d of %d bytes)", 
			  addr_len, len);
		
		return addr_len;
	}

	return bytes_processed;
}

/**
 * @brief Handles SOCKS5 protocol states and data forwarding
 * 
 * This function implements the SOCKS5 protocol state machine and handles:
 * - Initial handshake (SOCKS5_INIT)
 * - Authentication negotiation (SOCKS5_HANDSHAKE)
 * - Connection establishment (SOCKS5_CONNECT)
 * 
 * @param client The proxy client structure
 * @param rb Ring buffer containing incoming data
 * @param len Length of data in ring buffer
 * @return Number of bytes processed, 0 on error
 */
uint32_t handle_socks5(struct proxy_client *client, struct ring_buffer *rb, int len)
{
	uint32_t nret = 0;

	// Forward data in established connection state
	if (client->state == SOCKS5_CONNECT) {
		assert(client->local_proxy_bev);
		tx_ring_buffer_write(client->local_proxy_bev, rb, len);
		return len;
	}

	// Handle initial SOCKS5 handshake
	if (client->state == SOCKS5_INIT && len >= 3) {
		debug(LOG_DEBUG, "Processing SOCKS5 initial handshake, len: %d", len);
		uint8_t buf[3] = {0};
		rx_ring_buffer_pop(rb, buf, 3);

		if (buf[0] != 0x5 || buf[1] != 0x1 || buf[2] != 0x0) {
			debug(LOG_ERR, "Invalid SOCKS5 handshake");
			return nret;
		}

		// Send handshake response
		buf[1] = 0x0; // No authentication required
		tmux_stream_write(client->ctl_bev, buf, 3, &client->stream);
		client->state = SOCKS5_HANDSHAKE;
		return 3;
	}

	// Handle connection request
	if (client->state == SOCKS5_HANDSHAKE && len >= 10) {
		debug(LOG_DEBUG, "Processing SOCKS5 connection request, len: %d", len);
		uint8_t buf[3] = {0};
		rx_ring_buffer_pop(rb, buf, 3);

		if (!is_socks5(buf, 3)) {
			debug(LOG_ERR, "Invalid SOCKS5 request format");
			return nret;
		}

		int offset = 0;
		if (!parse_socks5_addr(rb, len, &offset, &client->remote_addr)) {
			debug(LOG_ERR, "Failed to parse SOCKS5 address");
			return nret;
		}

		client->local_proxy_bev = socks5_proxy_connect(client, &client->remote_addr);
		if (!client->local_proxy_bev) {
			debug(LOG_ERR, "Failed to establish proxy connection");
			return nret;
		}

		assert(len == offset + 3);
		return len;
	}

	// Handle invalid protocol state
	debug(LOG_ERR, "Invalid SOCKS5 protocol state");
	if (client->local_proxy_bev) {
		bufferevent_free(client->local_proxy_bev);
	}
	return nret;
}

/**
 * @brief Callback function handling data transfer from client to server in TCP proxy
 *
 * This function processes data received from the client-side bufferevent and forwards
 * it to the control connection. It supports both regular TCP proxy mode and TCP
 * multiplexing mode.
 *
 * @param bev The bufferevent structure containing client data
 * @param ctx Context pointer containing proxy client information
 *
 * Operation flow:
 * 1. Validates client and control connection
 * 2. Checks for available data in source buffer
 * 3. If TCP multiplexing is disabled, directly forwards data to control connection
 * 4. If TCP multiplexing is enabled, reads data into temporary buffer and writes
 *    to multiplexed stream
 * 
 * @note In multiplexing mode, if partial write occurs, the read event is disabled
 *       to prevent buffer overflow
 */
void tcp_proxy_c2s_cb(struct bufferevent *bev, void *ctx)
{
	struct proxy_client *client = (struct proxy_client *)ctx;
	if (!client || !client->ctl_bev) {
		debug(LOG_ERR, "Invalid client or control connection");
		return;
	}

	struct evbuffer *src = bufferevent_get_input(bev);
	size_t len = evbuffer_get_length(src);
	if (len == 0) {
		debug(LOG_DEBUG, "No data to read from client");
		return;
	}

	struct common_conf *c_conf = get_common_config();
	if (!c_conf->tcp_mux) {
		struct evbuffer *dst = bufferevent_get_output(client->ctl_bev);
		evbuffer_add_buffer(dst, src);
		return;
	}

	uint8_t *buf = calloc(1, len);
	if (!buf) {
		debug(LOG_ERR, "Failed to allocate memory for buffer");
		return;
	}

	size_t nr = bufferevent_read(bev, buf, len);
	if (nr != len) {
		debug(LOG_ERR, "Failed to read complete data: expected %zu, got %zu", len, nr);
		free(buf);
		return;
	}

	uint32_t written = tmux_stream_write(client->ctl_bev, buf, len, &client->stream);
	if (written < len) {
		debug(LOG_DEBUG, "Stream %d: Partial write %u/%zu bytes, disabling read",
			  client->stream.id, written, len);
		bufferevent_disable(bev, EV_READ);
	}

	free(buf);
}

/**
 * @brief Callback function for handling data transfer from server to client in TCP proxy
 *
 * This function is called when data is available to be read from the server's bufferevent
 * and needs to be forwarded to the client.
 *
 * @param bev The bufferevent structure containing the server's buffer
 * @param ctx The context pointer containing user-defined data (typically proxy session information)
 */
void tcp_proxy_s2c_cb(struct bufferevent *bev, void *ctx)
{
	struct common_conf *c_conf = get_common_config();
	struct proxy_client *client = (struct proxy_client *)ctx;
	
	if (!client || !client->local_proxy_bev) {
		debug(LOG_ERR, "Invalid client or local proxy connection");
		return;
	}

	struct evbuffer *src = bufferevent_get_input(bev);
	size_t len = evbuffer_get_length(src);
	if (len == 0) {
		debug(LOG_ERR, "No data to read from local service");
		return;
	}

	if (!c_conf->tcp_mux) {
		struct evbuffer *dst = bufferevent_get_output(client->local_proxy_bev);
		evbuffer_add_buffer(dst, src);
		return;
	}

	debug(LOG_ERR, "impossible to reach here");
}
