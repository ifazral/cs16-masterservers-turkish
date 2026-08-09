#ifndef A2S_QUERY_H
#define A2S_QUERY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[128];
    char map[64];
    char gamedir[32];
    char gametype[32];
    int players;
    int max_players;
    int bots;
    int protocol;
    bool is_dedicated;
    bool password;
    bool secure;
    int ping;
} a2s_server_info_t;

bool a2s_get_server_info(const uint32_t ip, const uint16_t port, a2s_server_info_t *info);

#ifdef __cplusplus
}
#endif

#endif // A2S_QUERY_H
