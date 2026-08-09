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

// revSrvBrowser.dll içindeki orijinal A2S_INFO istek paketi şablonu [\xFF\xFF\xFF\xFFTSource Engine Query][cite: 1]
static const uint8_t A2S_INFO_REQUEST[] = {
    0xFF, 0xFF, 0xFF, 0xFF,
    0x54, // 'T'
    'S','o','u','r','c','e',' ','E','n','g','i','n','e',' ','Q','u','e','r','y',
    0x00
};

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

    // İlk A2S isteğini gönder
    int sent = sendto(sock, (const char*)A2S_INFO_REQUEST, sizeof(A2S_INFO_REQUEST), 0,
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

    // Challenge (0x41) yanıtı gelirse token'ı ekleyip tekrar gönder
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

    // 0x49 = A2S_INFO yanıt başlığı
    if (recv_len <= 5 || recv_buf[4] != 0x49) {
        return false;
    }

    if (info) {
        memset(info, 0, sizeof(*info));
        int pos = 5;
        pos++; // version

        snprintf(info->name, sizeof(info->name), "%s", &recv_buf[pos]);
        pos += (int)strlen(info->name) + 1;

        snprintf(info->map, sizeof(info->map), "%s", &recv_buf[pos]);
        pos += (int)strlen(info->map) + 1;

        snprintf(info->gamedir, sizeof(info->gamedir), "%s", &recv_buf[pos]);
        pos += (int)strlen(info->gamedir) + 1;

        snprintf(info->gametype, sizeof(info->gametype), "%s", &recv_buf[pos]);
    }

    return true;
}
