#ifndef A2S_QUERY_H
#define A2S_QUERY_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// DERLEME HATASINI ÇÖZEN KISIM: [] yerine [25] yazıldı.
extern const uint8_t A2S_INFO_REQUEST[25];
extern const size_t A2S_INFO_REQUEST_LEN;

typedef struct {
    char name[128];
    char map[64];
    char gamedir[32];
    char gametype[32];
    char gamedesc[64];
    uint16_t appid;
    int players;
    int max_players;
    int bots;
    int protocol;
    bool is_dedicated;
    bool password;
    bool secure;
    int ping;
    int ping_ms;
} a2s_server_info_t;

bool a2s_get_server_info(const uint32_t ip, const uint16_t port, a2s_server_info_t *info);
bool a2s_query_server(const uint32_t ip, const uint16_t port, a2s_server_info_t *info);
bool parse_a2s_response(const uint8_t *data, int len, a2s_server_info_t *info);

#ifdef __cplusplus
}
#endif

#endif // A2S_QUERY_H
