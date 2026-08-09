#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "master_query.h"
#include "utils.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define CLOSE_SOCKET close
#endif

static bool build_query_packet(uint8_t *buf, int *len, int max_len,
    uint8_t region, const char *last_addr, const char *filter)
{
    int pos = 0;
    buf[pos++] = 0x31;
    buf[pos++] = region;

    int addr_len = (int)strlen(last_addr) + 1;
    if (pos + addr_len > max_len) return false;
    memcpy(buf + pos, last_addr, addr_len);
    pos += addr_len;

    int filt_len = (int)strlen(filter) + 1;
    if (pos + filt_len > max_len) return false;
    memcpy(buf + pos, filter, filt_len);
    pos += filt_len;

    *len = pos;
    return true;
}

static bool parse_response(const uint8_t *data, int len, master_query_result_t *result,
    uint32_t *last_ip, uint16_t *last_port)
{
    if (len < 6) return false;

    if (data[0] != 0xFF || data[1] != 0xFF || data[2] != 0xFF || data[3] != 0xFF)
        return false;

    if (data[4] != 0x66 || data[5] != 0x0A)
        return false;

    int pos = 6;
    while (pos + 6 <= len && result->count < MAX_QUERY_SERVERS)
    {
        uint32_t ip = (data[pos] << 24) | (data[pos + 1] << 16) | (data[pos + 2] << 8) | data[pos + 3];
        uint16_t port = (data[pos + 4] << 8) | data[pos + 5];
        pos += 6;

        if (ip == 0 && port == 0)
        {
            break;
        }

        uint32_t ip_net = htonl(ip);
        uint16_t port_net = htons(port);

        result->servers[result->count].ip = ip_net;
        result->servers[result->count].port = port_net;
        result->count++;

        *last_ip = ip;
        *last_port = port;
    }

    return true;
}

bool master_query_servers(const char *master_addr, master_query_result_t *result)
{
    memset(result, 0, sizeof(*result));

    char hostname[256];
    unsigned short port = 0;
    hostname[0] = 0;

    if (sscanf(master_addr, "%255[-.0-9A-Za-z_]:%hu", hostname, &port) < 1)
    {
        return false;
    }

    if (!port) port = 27010;

    uint32_t ip = host2ip(hostname);
    if (ip == 0 || ip == (uint32_t)-1)
    {
        return false;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = ip;
    dest.sin_port = htons(port);

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET)
        return false;

    char last_addr[32] = "0.0.0.0:0";
    uint32_t last_ip = 0;
    uint16_t last_port_val = 0;

    // revSrvBrowser.dll içindeki tam şablon filtremiz: \gamedir\%s \nap\%u \full\1 \empty\1 \secure\1 \proxy\1[cite: 1]
    char filter[256];
    snprintf(filter, sizeof(filter), "\\gamedir\\cstrike \\nap\\10 \\full\\1 \\empty\\1 \\secure\\1 \\proxy\\1");

    int retries = 0;
    bool done = false;

    while (!done && retries < 100)
    {
        uint8_t pkt[512];
        int pkt_len = 0;

        if (!build_query_packet(pkt, &pkt_len, sizeof(pkt), 0xFF, last_addr, filter))
            break;

        int sr = sendto(sock, (const char *)pkt, pkt_len, 0,
            (struct sockaddr *)&dest, sizeof(dest));
        if (sr == SOCKET_ERROR)
        {
            break;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int sel = select((int)sock + 1, &readfds, NULL, NULL, &tv);
        if (sel <= 0)
        {
            break;
        }

        uint8_t recv_buf[4096];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int recv_len = recvfrom(sock, (char *)recv_buf, sizeof(recv_buf), 0,
            (struct sockaddr *)&from, &fromlen);

        if (recv_len <= 0)
            break;

        uint32_t prev_last_ip = last_ip;
        uint16_t prev_last_port = last_port_val;
        int prev_count = result->count;

        if (!parse_response(recv_buf, recv_len, result, &last_ip, &last_port_val))
            break;

        if (result->count == prev_count || (last_ip == 0 && last_port_val == 0))
        {
            done = true;
            break;
        }

        snprintf(last_addr, sizeof(last_addr), "%u.%u.%u.%u:%u",
            (last_ip >> 24) & 0xFF, (last_ip >> 16) & 0xFF,
            (last_ip >> 8) & 0xFF, last_ip & 0xFF, last_port_val);

        retries++;
    }

    CLOSE_SOCKET(sock);
    return result->count > 0;
}
