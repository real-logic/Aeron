/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(__linux__)
#define _BSD_SOURCE
#define _GNU_SOURCE
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "protocol/aeron_udp_protocol.h"
#include "util/aeron_error.h"
#include "util/aeron_strutil.h"
#include "util/aeron_arrayutil.h"
#include "aeron_name_resolver.h"
#include "aeron_driver_context.h"
#include "media/aeron_udp_channel_transport_bindings.h"
#include "media/aeron_udp_channel_transport.h"
#include "media/aeron_udp_transport_poller.h"
#include "util/aeron_netutil.h"
#include "aeron_name_resolver_driver_cache.h"
#include "aeron_name_resolver_driver.h"


// Cater for windows.
#define AERON_MAX_HOSTNAME_LEN (256)
#define AERON_NAME_RESOLVER_DRIVER_DUTY_CYCLE_MS (10)
#define AERON_NAME_RESOLVER_DRIVER_NUM_RECV_BUFFERS (1)


static inline size_t aeron_res_header_entry_length_ipv4(aeron_resolution_header_ipv4_t *header)
{
    return AERON_ALIGN(sizeof(aeron_resolution_header_ipv4_t) + header->name_length, sizeof(int64_t));
}

static inline size_t aeron_res_header_entry_length_ipv6(aeron_resolution_header_ipv6_t *header)
{
    return AERON_ALIGN(sizeof(aeron_resolution_header_ipv6_t) + header->name_length, sizeof(int64_t));
}

typedef struct aeron_name_resolver_driver_neighbor_stct
{
    uint8_t address[AERON_RES_HEADER_ADDRESS_LENGTH_IP6];
    int64_t time_of_last_activity_ms;
    uint16_t port;
    int8_t res_type;
}
aeron_name_resolver_driver_neighbor_t;

typedef struct aeron_name_resolver_driver_stct
{
    const char *name;
    struct sockaddr_storage local_socket_addr;
    const char *bootstrap_neighbor;
    struct sockaddr_storage bootstrap_neighbor_addr;
    unsigned int interface_index;
    aeron_udp_channel_transport_bindings_t *transport_bindings;
    aeron_name_resolver_t bootstrap_resolver;
    aeron_udp_channel_data_paths_t data_paths;
    aeron_udp_channel_transport_t transport;
    aeron_udp_transport_poller_t poller;
    aeron_name_resolver_driver_cache_t cache;
    struct neighbour_stct
    {
        size_t length;
        size_t capacity;
        aeron_name_resolver_driver_neighbor_t *array;
    }
    neighbors;

    int64_t time_of_last_work_ms;
    int64_t self_resolution_interval_ms;
    int64_t neighbor_resolution_interval_ms;

    int64_t dead_line_self_resolutions_ms;
    int64_t dead_line_neighbor_resolutions_ms;

    struct sockaddr_storage received_address;
    uint8_t buffer[AERON_MAX_UDP_PAYLOAD_LENGTH];  // TODO: Cache alignment??
}
aeron_name_resolver_driver_t;

static const char *host_string(struct sockaddr_storage *addr)
{
    static char buf[NI_MAXHOST + NI_MAXSERV + 32];
    char host[NI_MAXHOST];
    char port[NI_MAXSERV];

    getnameinfo(
        (struct sockaddr *)addr, sizeof(struct sockaddr_storage),
        host, sizeof(host),
        port, sizeof(port),
        NI_NUMERICSERV | NI_NUMERICHOST);

    snprintf(buf, sizeof(buf), "(host=%s,port=%s)", host, port);
    return buf;
}

void aeron_name_resolver_driver_receive(
    aeron_udp_channel_data_paths_t *data_paths,
    void *receiver_clientd,
    void *endpoint_clientd,
    uint8_t *buffer,
    size_t length,
    struct sockaddr_storage *addr);

int aeron_name_resolver_driver_init(
    aeron_name_resolver_driver_t **driver_resolver,
    aeron_driver_context_t *context,
    const char *name,
    const char *interface_name,
    const char *bootstrap_neighbor)
{
    aeron_name_resolver_driver_t *_driver_resolver = NULL;
    char *local_hostname = NULL;

    if (aeron_alloc((void **)&_driver_resolver, sizeof(aeron_name_resolver_driver_t)) < 0)
    {
        aeron_set_err_from_last_err_code("%s:%d", __FILE__, __LINE__);
        goto error_cleanup;
    }

    _driver_resolver->name = name;
    if (NULL == _driver_resolver->name)
    {
        if (aeron_alloc((void **)&local_hostname, AERON_MAX_HOSTNAME_LEN) < 0)
        {
            goto error_cleanup;
        }

        if (gethostname(local_hostname, AERON_MAX_HOSTNAME_LEN) < 0)
        {
            aeron_set_err(errno, "Failed to lookup: %s", local_hostname);
            goto error_cleanup;
        }

        _driver_resolver->name = local_hostname;
    }

    if (aeron_find_unicast_interface(
        AF_INET, interface_name, &_driver_resolver->local_socket_addr, &_driver_resolver->interface_index) < 0)
    {
        goto error_cleanup;
    }

    if (aeron_name_resolver_default_supplier(context, &_driver_resolver->bootstrap_resolver, NULL) < 0)
    {
        goto error_cleanup;
    }

    _driver_resolver->bootstrap_neighbor = bootstrap_neighbor;
    if (NULL != _driver_resolver->bootstrap_neighbor)
    {
        if (aeron_name_resolver_resolve_host_and_port(
            &_driver_resolver->bootstrap_resolver,
            _driver_resolver->bootstrap_neighbor,
            "bootstrap_neighbor",
            false,
            &_driver_resolver->bootstrap_neighbor_addr) < 0)
        {
            goto error_cleanup;
        }
    }

    _driver_resolver->time_of_last_work_ms = 0;

    _driver_resolver->transport_bindings = context->udp_channel_transport_bindings;
    if (aeron_udp_channel_data_paths_init(
        &_driver_resolver->data_paths,
        context->udp_channel_outgoing_interceptor_bindings,
        context->udp_channel_incoming_interceptor_bindings,
        _driver_resolver->transport_bindings,
        aeron_name_resolver_driver_receive,
        AERON_UDP_CHANNEL_TRANSPORT_AFFINITY_CONDUCTOR) < 0)
    {
        goto error_cleanup;
    }

    if (_driver_resolver->transport_bindings->init_func(
        &_driver_resolver->transport,
        &_driver_resolver->local_socket_addr,
        NULL, // Unicast only.
        _driver_resolver->interface_index,
        0,
        context->socket_rcvbuf,
        context->socket_sndbuf,
        context,
        AERON_UDP_CHANNEL_TRANSPORT_AFFINITY_CONDUCTOR) < 0)
    {
        goto error_cleanup;
    }

    if (_driver_resolver->transport_bindings->poller_init_func(
        &_driver_resolver->poller, context, AERON_UDP_CHANNEL_TRANSPORT_AFFINITY_CONDUCTOR) < 0)
    {
        goto error_cleanup;
    }

    if (_driver_resolver->transport_bindings->poller_add_func(
        &_driver_resolver->poller, &_driver_resolver->transport) < 0)
    {
        goto error_cleanup;
    }

    aeron_name_resolver_driver_cache_init(&_driver_resolver->cache);

    _driver_resolver->self_resolution_interval_ms = 1000;
    _driver_resolver->dead_line_self_resolutions_ms = 0;
    _driver_resolver->neighbor_resolution_interval_ms = 2000;
    _driver_resolver->dead_line_neighbor_resolutions_ms = aeron_clock_cached_epoch_time(context->cached_clock);

    *driver_resolver = _driver_resolver;
    return 0;

error_cleanup:
    aeron_free((void *)local_hostname);
    aeron_free((void *)_driver_resolver);
    return -1;
}

int aeron_name_resolver_driver_close(aeron_name_resolver_t *resolver)
{
    aeron_name_resolver_driver_t *driver_resolver = (aeron_name_resolver_driver_t *)resolver->state;
    driver_resolver->transport_bindings->poller_close_func(&driver_resolver->poller);
    driver_resolver->transport_bindings->close_func(&driver_resolver->transport);
    aeron_free(driver_resolver->neighbors.array);
    aeron_free(driver_resolver);
    return 0;
}

static int aeron_name_resolver_driver_to_sockaddr(
    int8_t res_type, uint8_t *address, uint16_t port, struct sockaddr_storage *addr)
{
    int result = -1;
    if (res_type == AERON_RES_HEADER_TYPE_NAME_TO_IP6_MD)
    {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)addr;
        addr_in6->sin6_family = AF_INET6;
        addr_in6->sin6_port = htons(port);
        memcpy(&addr_in6->sin6_addr, address, sizeof(addr_in6->sin6_addr));
        result = 0;
    }
    else if (res_type == AERON_RES_HEADER_TYPE_NAME_TO_IP4_MD)
    {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
        addr_in->sin_family = AF_INET;
        addr_in->sin_port = htons(port);
        memcpy(&addr_in->sin_addr, address, sizeof(addr_in->sin_addr));
        result = 0;
    }
    else
    {
        assert(false && "Invalid res_type");
    }

    return result;
}

static int aeron_name_resolver_driver_from_sockaddr(
    struct sockaddr_storage *addr, int8_t *res_type, uint8_t **address, uint16_t *port)
{
    int result = -1;
    if (addr->ss_family == AF_INET6)
    {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)addr;
        *res_type = AERON_RES_HEADER_TYPE_NAME_TO_IP6_MD;
        *port = ntohs(addr_in6->sin6_port);
        *address = (uint8_t *)&addr_in6->sin6_addr;
        result = 0;
    }
    else if (addr->ss_family == AF_INET)
    {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)addr;
        *res_type = AERON_RES_HEADER_TYPE_NAME_TO_IP4_MD;
        *port = ntohs(addr_in->sin_port);
        *address = (uint8_t *)&addr_in->sin_addr;
        result = 0;
    }
    else
    {
        aeron_set_err(EINVAL, "Invalid address family: %d", addr->ss_family);
    }

    return result;
}

static uint16_t aeron_name_resolver_driver_get_port(aeron_name_resolver_driver_t *resolver)
{
    in_port_t port = resolver->local_socket_addr.ss_family == AF_INET6 ?
        ((struct sockaddr_in6 *)&resolver->local_socket_addr)->sin6_port :
        ((struct sockaddr_in *)&resolver->local_socket_addr)->sin_port;
    return ntohs(port);
}

int aeron_name_resolver_driver_resolve(
    aeron_name_resolver_t *resolver,
    const char *name,
    const char *uri_param_name,
    bool is_re_resolution,
    struct sockaddr_storage *address)
{
    aeron_name_resolver_driver_t *driver_resolver = resolver->state;

    const int8_t res_type = address->ss_family == AF_INET6 ?
        AERON_RES_HEADER_TYPE_NAME_TO_IP6_MD : AERON_RES_HEADER_TYPE_NAME_TO_IP4_MD;
    
    aeron_name_resolver_driver_cache_entry_t *cache_entry;
    if (aeron_name_resolver_driver_cache_lookup_by_name(
        &driver_resolver->cache, name, strlen(name), res_type, &cache_entry) < 0)
    {
        return driver_resolver->bootstrap_resolver.resolve_func(
            &driver_resolver->bootstrap_resolver, name, uri_param_name, is_re_resolution, address);
    }

    return aeron_name_resolver_driver_to_sockaddr(res_type, cache_entry->address, cache_entry->port, address);
}

int aeron_name_resolver_driver_find_neighbor_by_addr(
    aeron_name_resolver_driver_t *resolver,
    int8_t res_type,
    uint8_t *address,
    uint16_t port)
{
    for (size_t i = 0; i < resolver->neighbors.length; i++)
    {
        aeron_name_resolver_driver_neighbor_t *neighbor = &resolver->neighbors.array[i];

        if (res_type == neighbor->res_type &&
            port == neighbor->port &&
            0 == memcmp(address, neighbor->address, aeron_res_header_address_length(res_type)))
        {
            return i;
        }
    }

    return -1;
}

int aeron_name_resolver_driver_add_neighbor(
    aeron_name_resolver_driver_t *resolver,
    int8_t res_type,
    uint8_t *address,
    uint16_t port)
{
    if (aeron_name_resolver_driver_find_neighbor_by_addr(resolver, res_type, address, port) < 0)
    {
        int ensure_capacity_result = 0;
        AERON_ARRAY_ENSURE_CAPACITY(ensure_capacity_result, resolver->neighbors, aeron_name_resolver_driver_neighbor_t)
        if (ensure_capacity_result < 0)
        {
            aeron_set_err_from_last_err_code(
                "Failed to allocate rows for neighbors table (%zu,%zu) - %s:%d",
                resolver->neighbors.length, resolver->neighbors.capacity, __FILE__, __LINE__);

            return ensure_capacity_result;
        }

        aeron_name_resolver_driver_neighbor_t *new_neighbor = &resolver->neighbors.array[resolver->neighbors.length];
        new_neighbor->res_type = res_type;
        new_neighbor->port = port;
        memcpy(&new_neighbor->address, address, aeron_res_header_address_length(res_type));
        resolver->neighbors.length++;

        return 1;
    }

    return 0;
}

void aeron_name_resolver_driver_receive(
    aeron_udp_channel_data_paths_t *data_paths,
    void *receiver_clientd,
    void *endpoint_clientd,
    uint8_t *buffer,
    size_t length,
    struct sockaddr_storage *addr)
{
    aeron_name_resolver_driver_t *resolver = receiver_clientd;
    aeron_frame_header_t *frame_header = (aeron_frame_header_t *)buffer;
    size_t remaining = length;

    if ((remaining < sizeof(aeron_frame_header_t)) || (frame_header->version != AERON_FRAME_HEADER_VERSION))
    {
        // Resolution frames error counter...
//        aeron_counter_increment(receiver->invalid_frames_counter, 1);
        return;
    }

    remaining -= sizeof(aeron_frame_header_t);

    while (0 < remaining)
    {
        const size_t offset = length - remaining;
        if (AERON_HDR_TYPE_RES != frame_header->type || remaining < sizeof(aeron_resolution_header_t))
        {
            // Invalid message counter...
            return;
        }

        aeron_resolution_header_t *resolution_header = (aeron_resolution_header_t *)&buffer[offset];
        uint8_t *address;
        const char *name;
        size_t name_length;
        size_t entry_length;

        if (AERON_RES_HEADER_TYPE_NAME_TO_IP4_MD == resolution_header->res_type)
        {
            aeron_resolution_header_ipv4_t *ip4_hdr = (aeron_resolution_header_ipv4_t *)resolution_header;
            if (length < sizeof(aeron_resolution_header_ipv4_t) ||
                length < (entry_length = aeron_res_header_entry_length_ipv4(ip4_hdr)))
            {
                // Message not long enough...
                return;
            }

            address = ip4_hdr->addr;
            name_length = ip4_hdr->name_length;
            name = (const char *)(ip4_hdr + 1);
        }
        else if (AERON_RES_HEADER_TYPE_NAME_TO_IP6_MD == resolution_header->res_type)
        {
            aeron_resolution_header_ipv6_t *ip6_hdr = (aeron_resolution_header_ipv6_t *)resolution_header;
            if (length < sizeof(aeron_resolution_header_ipv6_t) ||
                length < (entry_length = aeron_res_header_entry_length_ipv6(ip6_hdr)))
            {
                // Message not long enough...
                return;
            }

            address = ip6_hdr->addr;
            name_length = ip6_hdr->name_length;
            name = (const char *)(ip6_hdr + 1);
        }
        else
        {
            // Log error invalid res_type...
            return;
        }


        int8_t res_type = resolution_header->res_type;
        uint16_t port = resolution_header->udp_port;

        printf("Host: %s, received: %s (%d)\n", resolver->name, name, port);

        if (AERON_RES_HEADER_SELF_FLAG == (resolution_header->res_flags & AERON_RES_HEADER_SELF_FLAG) &&
            resolution_header->res_type == AERON_RES_HEADER_TYPE_NAME_TO_IP4_MD &&
            *((in_addr_t *)address) == INADDR_ANY)
        {
            aeron_name_resolver_driver_from_sockaddr(addr, &res_type, &address, &port);
        }

        // Ignore self...
        if (port != aeron_name_resolver_driver_get_port(resolver) ||
            name_length != strlen(resolver->name) ||
            0 != strncmp(resolver->name, name, name_length))
        {
            aeron_name_resolver_driver_cache_add_or_update(&resolver->cache, name, name_length, res_type, address, port);

            aeron_name_resolver_driver_add_neighbor(
                resolver, resolution_header->res_type, address, resolution_header->udp_port);
        }

        remaining -= entry_length;
    }

    printf("Host: %s, from: %s\n", resolver->name, host_string(addr));
}

static int aeron_name_resolver_driver_poll(aeron_name_resolver_driver_t *resolver)
{
    struct mmsghdr mmsghdr[AERON_NAME_RESOLVER_DRIVER_NUM_RECV_BUFFERS];
    struct iovec iov[AERON_NAME_RESOLVER_DRIVER_NUM_RECV_BUFFERS];
    iov[0].iov_base = resolver->buffer;
    iov[0].iov_len = AERON_MAX_UDP_PAYLOAD_LENGTH;

    for (size_t i = 0; i < AERON_NAME_RESOLVER_DRIVER_NUM_RECV_BUFFERS; i++)
    {
        mmsghdr[i].msg_hdr.msg_name = &resolver->received_address;
        mmsghdr[i].msg_hdr.msg_namelen = sizeof(resolver->received_address);
        mmsghdr[i].msg_hdr.msg_iov = &iov[i];
        mmsghdr[i].msg_hdr.msg_iovlen = 1;
        mmsghdr[i].msg_hdr.msg_flags = 0;
        mmsghdr[i].msg_hdr.msg_control = NULL;
        mmsghdr[i].msg_hdr.msg_controllen = 0;
        mmsghdr[i].msg_len = 0;
    }

    int64_t bytes_received = 0;
    int poll_result = resolver->transport_bindings->poller_poll_func(
        &resolver->poller,
        mmsghdr,
        AERON_NAME_RESOLVER_DRIVER_NUM_RECV_BUFFERS,
        &bytes_received,
        resolver->data_paths.recv_func,
        resolver->transport_bindings->recvmmsg_func,
        resolver);

    if (poll_result < 0)
    {
        fprintf(stderr, "Failed to poll: %d\n", poll_result);
        // Distinct error log...
    }

    return bytes_received > 0 ? (int)bytes_received : 0;
}

int aeron_name_resolver_driver_send_self_resolutions(aeron_name_resolver_driver_t *resolver, int64_t now_ms)
{
    if (NULL == resolver->bootstrap_neighbor && 0 == resolver->neighbors.length)
    {
        return 0;
    }

    const size_t entry_offset = sizeof(aeron_frame_header_t);

    aeron_frame_header_t *frame_header = (aeron_frame_header_t *) &resolver->buffer[0];
    aeron_resolution_header_t *resolution_header = (aeron_resolution_header_t *)&resolver->buffer[entry_offset];

    const size_t name_length = strlen(resolver->name); // TODO: cache name length

    int entry_length = aeron_name_resolver_driver_set_resolution_header_from_sockaddr(
        resolution_header,
        sizeof(resolver->buffer) - entry_offset,
        AERON_RES_HEADER_SELF_FLAG,
        &resolver->local_socket_addr,
        resolver->name,
        name_length);

    assert(entry_length > 0 && "Bug! Single message should always fit in buffer.");

    frame_header->type = AERON_HDR_TYPE_RES;
    frame_header->flags = UINT8_C(0);
    frame_header->version = AERON_FRAME_HEADER_VERSION;
    frame_header->frame_length = sizeof(frame_header) + entry_length;

    struct sockaddr_storage neighbor_address;

    struct iovec iov[1];
    iov[0].iov_base = frame_header;
    iov[0].iov_len = frame_header->frame_length;
    struct msghdr msghdr;
    msghdr.msg_iov = iov;
    msghdr.msg_iovlen = 1;
    msghdr.msg_flags = 0;
    msghdr.msg_name = &neighbor_address;
    msghdr.msg_namelen = sizeof(neighbor_address);
    msghdr.msg_control = NULL;
    msghdr.msg_controllen = 0;

    int send_result = 0;
    if (0 == resolver->neighbors.length && NULL != resolver->bootstrap_neighbor)
    {
        msghdr.msg_name = &resolver->bootstrap_neighbor_addr;
        msghdr.msg_namelen = sizeof(resolver->bootstrap_neighbor_addr);

        // TODO: Track resolution send errors/short sends.
        send_result = resolver->transport_bindings->sendmsg_func(&resolver->data_paths, &resolver->transport, &msghdr);
        if (send_result < 0)
        {
            fprintf(stderr, "Error: %s\n", aeron_errmsg());
        }

        printf("From: %s, to: %s, send self (%d)\n", resolver->name, host_string(&resolver->bootstrap_neighbor_addr), resolution_header->udp_port);
    }
    else
    {
        for (size_t k = 0; k < resolver->neighbors.length; k++)
        {
            aeron_name_resolver_driver_neighbor_t *neighbor = &resolver->neighbors.array[k];

            aeron_name_resolver_driver_to_sockaddr(
                neighbor->res_type, neighbor->address, neighbor->port, &neighbor_address);

            send_result += resolver->transport_bindings->sendmsg_func(
                &resolver->data_paths, &resolver->transport, &msghdr);

            if (send_result < 0)
            {
                // TODO: proper error handling...
                fprintf(stderr, "Failed to send");
            }

            printf("From: %s, to: %s, send self (%d)\n", resolver->name, host_string(&neighbor_address), resolution_header->udp_port);
        }
    }

    return send_result;
}

int aeron_name_resolver_driver_send_neighbor_resolutions(aeron_name_resolver_driver_t *resolver, int64_t now_ms)
{
    aeron_frame_header_t *frame_header = (aeron_frame_header_t *) &resolver->buffer[0];
    frame_header->type = AERON_HDR_TYPE_RES;
    frame_header->flags = UINT8_C(0);
    frame_header->version = AERON_FRAME_HEADER_VERSION;

    struct sockaddr_storage neighbor_address;

    struct iovec iov[1];
    iov[0].iov_base = frame_header;
    struct msghdr msghdr;
    msghdr.msg_iov = iov;
    msghdr.msg_iovlen = 1;
    msghdr.msg_flags = 0;
    msghdr.msg_name = &neighbor_address;
    msghdr.msg_namelen = sizeof(neighbor_address);
    msghdr.msg_control = NULL;
    msghdr.msg_controllen = 0;

    size_t i;
    size_t j;
    int work_count = 0;
    for (i = 0; i < resolver->cache.entries.length;)
    {
        size_t entry_offset = sizeof(aeron_frame_header_t);

        for (j = i; j < resolver->cache.entries.length;)
        {
            aeron_resolution_header_t *resolution_header = (aeron_resolution_header_t *)&resolver->buffer[entry_offset];
            aeron_name_resolver_driver_cache_entry_t *cache_entry = &resolver->cache.entries.array[j];

            int entry_length = aeron_name_resolver_driver_set_resolution_header(
                resolution_header,
                sizeof(resolver->buffer) - entry_offset,
                0,
                cache_entry->res_type,
                cache_entry->address,
                cache_entry->port,
                cache_entry->name,
                cache_entry->name_length);

            assert(-1 != entry_length && "Invalid res_type crept in from somewhere");

            if (0 == entry_length)
            {
                break;
            }

            printf("Host: %s, neighbor: %.*s (%d)\n", resolver->name, (int)cache_entry->name_length, cache_entry->name, cache_entry->port);

            entry_offset += entry_length;
            j++;
        }

        frame_header->frame_length = (int32_t)entry_offset;
        iov[0].iov_len = entry_offset;
        
        for (size_t k = 0; k < resolver->neighbors.length; k++)
        {
            aeron_name_resolver_driver_neighbor_t *neighbor = &resolver->neighbors.array[k];

            aeron_name_resolver_driver_to_sockaddr(
                neighbor->res_type, neighbor->address, neighbor->port, &neighbor_address);

            int send_result = resolver->transport_bindings->sendmsg_func(
                &resolver->data_paths, &resolver->transport, &msghdr);

            if (send_result < 0)
            {
                // TODO: proper error handling...
                fprintf(stderr, "Failed to send");
            }

            printf("From: %s, to: %s, send neighbors\n", resolver->name, host_string(&neighbor_address));
        }

        work_count++;
        i = j;
    }

    return work_count;
}

int aeron_name_resolver_driver_do_work(aeron_name_resolver_t *resolver, int64_t now_ms)
{
    aeron_name_resolver_driver_t *driver_resolver = resolver->state;
    int work_count = 0;

    if ((driver_resolver->time_of_last_work_ms + AERON_NAME_RESOLVER_DRIVER_DUTY_CYCLE_MS) <= now_ms)
    {
        work_count += aeron_name_resolver_driver_poll(driver_resolver);

        if (driver_resolver->dead_line_self_resolutions_ms <= now_ms)
        {
            work_count += aeron_name_resolver_driver_send_self_resolutions(driver_resolver, now_ms);

            driver_resolver->dead_line_self_resolutions_ms += driver_resolver->self_resolution_interval_ms;
        }

        if (driver_resolver->dead_line_neighbor_resolutions_ms <= now_ms)
        {
            work_count += aeron_name_resolver_driver_send_neighbor_resolutions(driver_resolver, now_ms);

            driver_resolver->dead_line_neighbor_resolutions_ms += driver_resolver->neighbor_resolution_interval_ms;
        }
    }

    return work_count;
}

int aeron_name_resolver_driver_supplier(
    aeron_driver_context_t *context,
    aeron_name_resolver_t *resolver,
    const char *args)
{
    aeron_name_resolver_driver_t *name_resolver = NULL;

    resolver->state = NULL;
    if (aeron_name_resolver_driver_init(
        &name_resolver, context,
        context->resolver_name,
        context->resolver_interface,
        context->resolver_bootstrap_neighbor) < 0)
    {
        return -1;
    }

    resolver->lookup_func = aeron_name_resolver_default_lookup;
    resolver->resolve_func = aeron_name_resolver_driver_resolve;
    resolver->do_work_func = aeron_name_resolver_driver_do_work;
    resolver->close_func = aeron_name_resolver_driver_close;

    resolver->state = name_resolver;

    return 0;
}

int aeron_name_resolver_driver_set_resolution_header_from_sockaddr(
    aeron_resolution_header_t *resolution_header,
    size_t capacity,
    uint8_t flags,
    struct sockaddr_storage *addr,
    const char *name,
    size_t name_length)
{
    int8_t res_type;
    uint8_t *address;
    uint16_t port;

    if (aeron_name_resolver_driver_from_sockaddr(addr, &res_type, &address, &port) < 0)
    {
        return -1;
    }

    return aeron_name_resolver_driver_set_resolution_header(
        resolution_header,
        capacity,
        flags,
        res_type,
        address,
        port,
        name,
        name_length);
}

int aeron_name_resolver_driver_set_resolution_header(
    aeron_resolution_header_t *resolution_header,
    size_t capacity,
    uint8_t flags,
    int8_t res_type,
    uint8_t *address,
    uint16_t port,
    const char *name,
    size_t name_length)
{
    size_t name_offset;
    size_t entry_length;

    switch (res_type)
    {
        case AERON_RES_HEADER_TYPE_NAME_TO_IP4_MD:
            entry_length = AERON_ALIGN(sizeof(aeron_resolution_header_ipv4_t) + name_length, sizeof(int64_t));
            if (capacity < entry_length)
            {
                return 0;
            }

            aeron_resolution_header_ipv4_t *hdr_ipv4 = (aeron_resolution_header_ipv4_t *) resolution_header;
            memcpy(&hdr_ipv4->addr, address, sizeof(hdr_ipv4->addr));
            hdr_ipv4->name_length = name_length;
            name_offset = sizeof(aeron_resolution_header_ipv4_t);

            break;

        case AERON_RES_HEADER_TYPE_NAME_TO_IP6_MD:
            entry_length = AERON_ALIGN(sizeof(aeron_resolution_header_ipv6_t) + name_length, sizeof(int64_t));
            if (capacity < entry_length)
            {
                return 0;
            }

            aeron_resolution_header_ipv6_t *hdr_ipv6 = (aeron_resolution_header_ipv6_t *) resolution_header;

            memcpy(&hdr_ipv6->addr, address, sizeof(hdr_ipv6->addr));
            hdr_ipv6->name_length = name_length;
            name_offset = sizeof(aeron_resolution_header_ipv6_t);

            break;

        default:
            return -1;
    }

    resolution_header->res_type = res_type;
    resolution_header->udp_port = port;
    resolution_header->res_flags = flags;

    uint8_t *buffer = (uint8_t *)resolution_header;
    memcpy(&buffer[name_offset], name, name_length);

    return entry_length;
}

