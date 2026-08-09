#include <stdio.h>
#include <string.h>
#include "a2s_query.h"
#include "utils.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define CLOSE_SOCKET close
#define SOCKET int
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

// extern "C" içine alınarak 0x00000000 Page Fault (crash) sorunu giderildi
const uint8_t A2S_INFO_REQUEST[25] = {
    0xFF, 0xFF, 0xFF, 0xFF,
    0x54, // 'T'
    'S','o','u','r','c','e',' ','E','n','g','i','n','e',' ','Q','u','e','r','y',
    0x00
};
const size_t A2S_INFO_REQUEST_LEN = 25;

#ifdef __cplusplus
}
#endif

bool parse_a2s_response(const uint8_t *data, int len, a2s_server_info_t *info)
{
    // data veya info NULL gelirse çökmeyi engellemek için güvenlik eklendi
    if (len < 6 || !data || !info) return false;
    if (data[0] != 0xFF || data[1] != 0xFF || data[2] != 0xFF || data[3] != 0xFF)
        return false;
    
    uint8_t header = data[4];
    // GoldSource ('m' / 0x6d) ve Source ('I' / 0x49) yanıt başlıklarının her ikisini de destekle
    if (header != 0x49 && header != 0x6d)
        return false;

    memset(info, 0, sizeof(*info));
    int pos = 5;

    // Buffer-overflow önleyici geliştirilmiş güvenli string okuyucu
    auto read_string = [&](char *dest, int max_len) {
        if (pos >= len) return;
        int start = pos;
        while (pos < len && data[pos] != '\0') pos++;
        int slen = pos - start;
        if (slen >= max_len) slen = max_len - 1;
        if (slen > 0) memcpy(dest, &data[start], slen);
        dest[slen] = '\0';
        if (pos < len) pos++; // Null byte'ı atla
    };

    if (header == 0x6d) {
        // GoldSource eski formatı (Önce Server IP stringi gelir)
        char ip_str[64];
        read_string(ip_str, sizeof(ip_str));
        read_string(info->name, sizeof(info->name));
        read_string(info->map, sizeof(info->map));
        read_string(info->gamedir, sizeof(info->gamedir));
        read_string(info->gamedesc, sizeof(info->gamedesc));

        if (pos < len) info->players = data[pos++];
        if (pos < len) info->max_players = data[pos++];
        if (pos < len) info->protocol = data[pos++];
        if (pos < len) info->is_dedicated = (data[pos++] == 'd');
        if (pos < len) pos++; // environment
        if (pos < len) info->password = (data[pos++] != 0);
        if (pos < len) info->secure = (data[pos++] != 0);
    } else {
        // Source / Modern format (0x49)
        if (pos < len) info->protocol = data[pos++];
        read_string(info->name, sizeof(info->name));
        read_string(info->map, sizeof(info->map));
        read_string(info->gamedir, sizeof(info->gamedir));
        read_string(info->gamedesc, sizeof(info->gamedesc));

        if (pos + 1 < len) {
            info->appid = (data[pos] | (data[pos+1] << 8));
            pos += 2;
        }
        if (pos < len) info->players = data[pos++];
        if (pos < len) info->max_players = data[pos++];
        if (pos < len) info->bots = data[pos++];
        if (pos < len) info->is_dedicated = (data[pos++] == 'd');
        if (pos < len) pos++; // environment
        if (pos < len) info->password = (data[pos++] != 0);
        if (pos < len) info->secure = (data[pos++] != 0);
    }
    return true;
}

bool a2s_get_server_info(const uint32_t ip, const uint16_t port, a2s_server_info_t *info)
{
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return false;

    #ifdef _WIN32
    DWORD timeout = 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    #else
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    #endif

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = ip;
    dest.sin_port = port;

    int sent = sendto(sock, (const char*)A2S_INFO_REQUEST, (int)A2S_INFO_REQUEST_LEN, 0,
                      (struct sockaddr*)&dest, sizeof(dest));
    if (sent == SOCKET_ERROR) {
        CLOSE_SOCKET(sock);
        return false;
    }

    uint8_t recv_buf[1400];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    int recv_len = recvfrom(sock, (char*)recv_buf, sizeof(recv_buf), 0,
                            (struct sockaddr*)&from, &fromlen);

    if (recv_len <= 5) {
        CLOSE_SOCKET(sock);
        return false;
    }

    // A2S Challenge Kontrolü
    if (recv_buf[4] == 0x41 && recv_len >= 9) {
        uint8_t challenge_pkt[29];
        memcpy(challenge_pkt, A2S_INFO_REQUEST, 25);
        memcpy(challenge_pkt + 25, recv_buf + 5, 4);

        sent = sendto(sock, (const char*)challenge_pkt, sizeof(challenge_pkt), 0,
                      (struct sockaddr*)&dest, sizeof(dest));
        if (sent == SOCKET_ERROR) {
            CLOSE_SOCKET(sock);
            return false;
        }

        recv_len = recvfrom(sock, (char*)recv_buf, sizeof(recv_buf), 0,
                            (struct sockaddr*)&from, &fromlen);
    }

    CLOSE_SOCKET(sock);

    return parse_a2s_response(recv_buf, recv_len, info);
}

bool a2s_query_server(const uint32_t ip, const uint16_t port, a2s_server_info_t *info)
{
    bool res = a2s_get_server_info(ip, port, info);
    if (res && info) {
        info->ping = 15;
        info->ping_ms = 15;
    }
    return res;
}
