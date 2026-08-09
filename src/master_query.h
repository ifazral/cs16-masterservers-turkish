#ifndef MASTER_QUERY_H
#define MASTER_QUERY_H

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET socket_t;
#else
typedef int socket_t;
#define INVALID_SOCKET (-1)
#endif

#define MAX_QUERY_SERVERS 2048
#define PORT_MASTER 27010
#define QUERY_MAX_RETRIES 100
#define QUERY_TIMEOUT_MS 2000

typedef struct {
    uint32_t ip;
    uint16_t port;
} server_addr_t;

typedef struct {
    server_addr_t servers[MAX_QUERY_SERVERS];
    int count;
} master_query_result_t;

typedef struct {
    char hostname[128]; // dllmain.cpp uyumluluğu için eklendi
    char gamedir[32];
    char map[64];
    char version[32];
    int protocol;
    int players;
    int max_players;
    int bots;
    bool is_dedicated;
    int password;
    int secure;
    int lan;
} heartbeat_info_t;

#ifdef __cplusplus
extern "C" {
#endif

bool master_query_servers(const char *master_addr, master_query_result_t *result);
bool master_validate_server(const char *master_addr);
bool master_send_heartbeat(const char *master_addr, const heartbeat_info_t *info, unsigned int use_socket);

#ifdef __cplusplus
}
#endif

#endif // MASTER_QUERY_H
