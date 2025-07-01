#include "mosquitto_internal.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <stdlib.h>

#include "net_mosq.h" // For MOSQ_ERR_PROXY if defined there, or needs mosquitto.h if defined there
#include "logging_mosq.h" // For log__printf
#include "memory_mosq.h" // For mosquitto__malloc, mosquitto__free

/* For poll() */
#ifndef WIN32
#include <poll.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

/* Function declaration for net__read and net__write, if not in an included header */
ssize_t net__read(struct mosquitto *mosq, void *buf, size_t count);
ssize_t net__write(struct mosquitto *mosq, const void *buf, size_t count);


int net__proxy_connect(struct mosquitto *mosq, const char *dest_host, int dest_port)
{
    char request[1024]; /* Increased size for potentially longer auth headers */
    char *response;
    int len;
    ssize_t n;
    size_t total_read = 0; /* Renamed from total to avoid conflict if mosq has a field named total */
    struct pollfd pfd;
    const char *terminator = "\r\n\r\n";
    size_t response_size = 2048;
    int connect_timeout_ms = 10000; /* 10 seconds timeout for proxy connect */

    response = (char *)mosquitto__malloc(response_size);
    if(!response){
        return MOSQ_ERR_NOMEM;
    }

    if (mosq->proxy.auth_header) {
        len = snprintf(request, sizeof(request),
            "CONNECT %s:%d HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "%s" /* Proxy-Authorization is already formatted with \r\n in mosquitto_proxy_set */
            "\r\n",
            dest_host, dest_port, dest_host, dest_port,
            mosq->proxy.auth_header);
    } else {
        len = snprintf(request, sizeof(request),
            "CONNECT %s:%d HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "\r\n",
            dest_host, dest_port, dest_host, dest_port);
    }

    if(len <= 0 || (size_t)len >= sizeof(request)){
        mosquitto__free(response);
        log__printf(mosq, MOSQ_LOG_ERR, "Proxy CONNECT request too long.");
        return MOSQ_ERR_PROXY; /* Or MOSQ_ERR_INVAL if more appropriate */
    }

    n = net__write(mosq, request, (size_t)len);
    if(n != len){
        mosquitto__free(response);
        log__printf(mosq, MOSQ_LOG_ERR, "Error writing to proxy: %s.", strerror(errno));
        return MOSQ_ERR_PROXY; /* Or MOSQ_ERR_ERRNO */
    }

    pfd.fd = mosq->sock;
    pfd.events = POLLIN;

    while(total_read < response_size-1){
#ifndef WIN32
        n = poll(&pfd, 1, connect_timeout_ms);
#else
        n = WSAPoll(&pfd, 1, connect_timeout_ms);
#endif
        if(n < 0){ /* Error */
            mosquitto__free(response);
            log__printf(mosq, MOSQ_LOG_ERR, "Proxy poll error: %s.", strerror(errno));
            return MOSQ_ERR_PROXY; /* Or MOSQ_ERR_ERRNO */
        }
        if(n == 0){ /* Timeout */
            mosquitto__free(response);
            log__printf(mosq, MOSQ_LOG_ERR, "Proxy connect timed out.");
            return MOSQ_ERR_PROXY; /* Or MOSQ_ERR_TIMEOUT */
        }

        if (pfd.revents & POLLIN) {
            n = net__read(mosq, response + total_read, response_size - 1 - total_read);
            if(n <= 0){
                mosquitto__free(response);
                if (n == 0) {
                    log__printf(mosq, MOSQ_LOG_ERR, "Proxy connection closed prematurely.");
                } else {
                    log__printf(mosq, MOSQ_LOG_ERR, "Error reading from proxy: %s.", strerror(errno));
                }
                return MOSQ_ERR_PROXY; /* Or MOSQ_ERR_ERRNO */
            }
            total_read += (size_t)n;
            response[total_read] = '\0';

            if(strstr(response, terminator)){
                break; /* Full headers received */
            }
        } else { /* Some other event or error on the socket */
             mosquitto__free(response);
             log__printf(mosq, MOSQ_LOG_ERR, "Proxy socket error during connect.");
             return MOSQ_ERR_PROXY;
        }
    }

    if (!strstr(response, terminator)) {
        mosquitto__free(response);
        log__printf(mosq, MOSQ_LOG_ERR, "Proxy response headers too long or incomplete.");
        return MOSQ_ERR_PROXY;
    }

    /* Check for HTTP 200 response */
    if(strncmp(response, "HTTP/1.1 200", 12) != 0 && strncmp(response, "HTTP/1.0 200", 12) != 0){
        /* Log the actual response for debugging */
        char *end_of_status_line = strstr(response, "\r\n");
        if (end_of_status_line) {
            *end_of_status_line = '\0';
        }
        log__printf(mosq, MOSQ_LOG_ERR, "Proxy CONNECT failed: %s", response);
        if (end_of_status_line) {
            *end_of_status_line = '\r'; /* Restore for full response if needed elsewhere */
        }
        mosquitto__free(response);
        return MOSQ_ERR_PROXY;
    }

    log__printf(mosq, MOSQ_LOG_INFO, "Successfully connected to MQTT broker via proxy.");
    mosquitto__free(response);
    return MOSQ_ERR_SUCCESS;
}
