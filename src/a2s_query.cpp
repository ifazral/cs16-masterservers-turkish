#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "a2s_query.h"

// OyunYöneticisi ve ReHLDS uyumlu A2S_INFO istek paketi
static const uint8_t A2S_INFO_REQUEST[] = {
    0xFF, 0xFF, 0xFF, 0xFF,
    0x54,
    'S','o','u','r','c','e',' ','E','n','g','i','n','e',' ','Q','u','e','r','y', 0x00,
    '\\', 'g', 'a', 'm', 'e', 'd', 'i', 'r', '\\', 'c', 's', 't', 'r', 'i', 'k', 'e', 0x00
};

static const char *read_string(const uint8_t *data, int len, int *pos, char *out, int out_size)
{
    int i = 0;
    while (*pos < len && data[*pos] != 0 && i < out_size - 1)
    {
        out[i++] = (char)data[*pos];
        (*pos)++;
    }
    out[i] = '\0';
    if (*pos < len && data[*pos] == 0) (*pos)++;
    return out;
}

bool parse_a2s_response(const uint8_t *data, int len, a2s_server_info_t *out)
{
    if (len < 6) return false;

    if (data[0] != 0xFF || data[1] != 0xFF || data[2] != 0xFF || data[3] != 0xFF)
        return false;

    if (data[4] != 0x49) return false;

    int pos = 5;
    uint8_t protocol = data[pos++];

    read_string(data, len, &pos, out->name, sizeof(out->name));
    read_string(data, len, &pos, out->map, sizeof(out->map));
    read_string(data, len, &pos, out->gamedir, sizeof(out->gamedir));
    read_string(data, len, &pos, out->gamedesc, sizeof(out->gamedesc));

    if (pos + 7 > len) return false;

    out->appid = (uint16_t)(data[pos] | (data[pos + 1] << 8));
    pos += 2;
    out->players = data[pos++];
    out->max_players = data[pos++];
    out->bots = data[pos++];
    out->type = (char)data[pos++];
    out->os = (char)data[pos++];

    if (pos + 2 > len) return false;
    out->password = data[pos++];
    out->secure = data[pos++];

    read_string(data, len, &pos, out->version, sizeof(out->version));

    if (pos < len)
    {
        uint8_t edf = data[pos++];
        if ((edf & 0x80) && pos + 2 <= len) pos += 2;
        if ((edf & 0x10) && pos + 8 <= len) pos += 8;
        if (edf & 0x40)
        {
            if (pos + 2 <= len) pos += 2;
            char spec_name[128];
            read_string(data, len, &pos, spec_name, sizeof(spec_name));
        }
        if (edf & 0x20)
        {
            char keywords[256];
            read_string(data, len, &pos, keywords, sizeof(keywords));
        }
        if ((edf & 0x01) && pos + 8 <= len) pos += 8;
    }

    out->valid = true;
    return true;
}

bool a2s_query_server(uint32_t ip_net, uint16_t port_net, a2s_server_info_t *out, int timeout_ms)
{
    memset(out, 0, sizeof(*out));

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = ip_net;
    dest.sin_port = port_net;

    uint8_t req_buf[64];
    size_t req_len = sizeof(A2S_INFO_REQUEST);
    memcpy(req_buf, A2S_INFO_REQUEST, req_len);

    DWORD start = GetTickCount();

    for (int attempt = 0; attempt < 2; attempt++)
    {
        if (sendto(sock, (const char *)req_buf, (int)req_len, 0,
                   (struct sockaddr *)&dest, sizeof(dest)) == SOCKET_ERROR)
        {
            closesocket(sock);
            return false;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        DWORD elapsed = GetTickCount() - start;
        if (elapsed >= (DWORD)timeout_ms) break;

        DWORD remain = (DWORD)timeout_ms - elapsed;
        struct timeval tv;
        tv.tv_sec = remain / 1000;
        tv.tv_usec = (remain % 1000) * 1000;

        if (select(0, &readfds, NULL, NULL, &tv) <= 0)
        {
            closesocket(sock);
            return false;
        }

        uint8_t buf[2048];
        struct sockaddr_in from;
        int fromlen = sizeof(from);
        int recv_len = recvfrom(sock, (char *)buf, sizeof(buf), 0,
                                (struct sockaddr *)&from, &fromlen);

        if (recv_len < 5)
        {
            closesocket(sock);
            return false;
        }

        if (buf[0] == 0xFF && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0x41)
        {
            if (recv_len >= 9)
            {
                memcpy(req_buf + sizeof(A2S_INFO_REQUEST), buf + 5, 4);
                req_len = sizeof(A2S_INFO_REQUEST) + 4;
                continue;
            }
        }

        DWORD total_elapsed = GetTickCount() - start;
        closesocket(sock);

        if (parse_a2s_response(buf, recv_len, out))
        {
            out->ip = ip_net;
            out->port = port_net;
            out->ping_ms = (int)total_elapsed;
            return true;
        }

        return false;
    }

    closesocket(sock);
    return false;
}

int a2s_query_batch(uint32_t *ips, uint16_t *ports, int count,
                    a2s_server_info_t *results, int timeout_ms)
{
    if (count <= 0) return 0;

    int max_batch = 64;
    int total_valid = 0;

    for (int base = 0; base < count; base += max_batch)
    {
        int batch = count - base;
        if (batch > max_batch) batch = max_batch;

        SOCKET socks[64];
        DWORD starts[64];
        bool challenged[64];

        for (int i = 0; i < batch; i++)
        {
            int idx = base + i;
            memset(&results[idx], 0, sizeof(results[idx]));
            challenged[i] = false;

            socks[i] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socks[i] == INVALID_SOCKET) continue;

            struct sockaddr_in dest;
            memset(&dest, 0, sizeof(dest));
            dest.sin_family = AF_INET;
            dest.sin_addr.s_addr = ips[idx];
            dest.sin_port = ports[idx];

            starts[i] = GetTickCount();
            sendto(socks[i], (const char *)A2S_INFO_REQUEST, sizeof(A2S_INFO_REQUEST), 0,
                   (struct sockaddr *)&dest, sizeof(dest));
        }

        DWORD deadline = GetTickCount() + timeout_ms;

        while (1)
        {
            DWORD now = GetTickCount();
            if (now >= deadline) break;

            fd_set readfds;
            FD_ZERO(&readfds);
            int active = 0;

            for (int i = 0; i < batch; i++)
            {
                if (socks[i] == INVALID_SOCKET) continue;
                FD_SET(socks[i], &readfds);
                active++;
            }

            if (active == 0) break;

            struct timeval tv;
            DWORD remain = deadline - now;
            tv.tv_sec = remain / 1000;
            tv.tv_usec = (remain % 1000) * 1000;

            int sel = select(0, &readfds, NULL, NULL, &tv);
            if (sel <= 0) break;

            for (int i = 0; i < batch; i++)
            {
                if (socks[i] == INVALID_SOCKET) continue;
                if (!FD_ISSET(socks[i], &readfds)) continue;

                int idx = base + i;
                uint8_t buf[2048];
                struct sockaddr_in from;
                int fromlen = sizeof(from);
                int recv_len = recvfrom(socks[i], (char *)buf, sizeof(buf), 0,
                                        (struct sockaddr *)&from, &fromlen);

                DWORD elapsed = GetTickCount() - starts[i];

                if (recv_len >= 9 && buf[0] == 0xFF && buf[1] == 0xFF && 
                    buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0x41 && !challenged[i])
                {
                    challenged[i] = true;
                    uint8_t req_buf[64];
                    memcpy(req_buf, A2S_INFO_REQUEST, sizeof(A2S_INFO_REQUEST));
                    memcpy(req_buf + sizeof(A2S_INFO_REQUEST), buf + 5, 4);

                    struct sockaddr_in dest;
                    memset(&dest, 0, sizeof(dest));
                    dest.sin_family = AF_INET;
                    dest.sin_addr.s_addr = ips[idx];
                    dest.sin_port = ports[idx];

                    sendto(socks[i], (const char *)req_challenge, sizeof(A2S_INFO_REQUEST) + 4, 0,
                           (struct sockaddr *)&dest, sizeof(dest));
                    continue;
                }

                if (recv_len > 0 && parse_a2s_response(buf, recv_len, &results[idx]))
                {
                    results[idx].ip = ips[idx];
                    results[idx].port = ports[idx];
                    results[idx].ping_ms = (int)elapsed;
                    total_valid++;
                }

                closesocket(socks[i]);
                socks[i] = INVALID_SOCKET;
            }
        }

        for (int i = 0; i < batch; i++)
        {
            if (socks[i] != INVALID_SOCKET)
                closesocket(socks[i]);
        }
    }

    return total_valid;
}
