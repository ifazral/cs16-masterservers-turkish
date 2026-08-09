#define FD_SETSIZE 256
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "matchmaking_impl.h"
#include "master_query.h"
#include "a2s_query.h"
#include "vdf_parser.h"
#include "server_cache.h"
#include "utils.h"

static const uint8_t A2S_INFO_REQ_BYTES[] = { 
    0xFF, 0xFF, 0xFF, 0xFF, 0x54, 0x53, 0x6F, 0x75, 0x72, 0x63, 0x65, 0x20, 
    0x45, 0x6E, 0x67, 0x69, 0x6E, 0x65, 0x20, 0x51, 0x75, 0x65, 0x72, 0x79, 0x00 
};

extern void RealMasterLog(const char *fmt, ...);

// --- DEBUG SİSTEMİ ---
// UI ve Motor spam loglarını görmek istiyorsanız true kalsın. Kapatmak için false yapın.
bool g_bVerboseDebug = true; 

static void VerboseLog(const char* fmt, ...) 
{
    if (!g_bVerboseDebug) return;

    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    // Hem DebugView (Canlı izleme) aracına hem de normal loga gönder
    OutputDebugStringA("[CRealMasterMatchmaking] ");
    OutputDebugStringA(buffer);
    OutputDebugStringA("\n");
    
    RealMasterLog("[VERBOSE] %s", buffer);
}
// ---------------------

static CRealMasterMatchmaking g_RealMaster;
static master_list_t g_MasterList;
bool g_MasterListLoaded = false;

struct QueryThreadData
{
    CRealMasterMatchmaking *self;
    gameserveritem_t *servers;
    volatile int *serverCount;
    uint32_t reqId;
};

static void load_master_list()
{
    if (g_MasterListLoaded) return;
    g_MasterListLoaded = true;

    find_plugin_dir();

    char vdf_path[512];
    snprintf(vdf_path, sizeof(vdf_path), "%s\\platform\\config\\MasterServers.vdf", g_PluginDir);

    if (!vdf_parse_master_servers(vdf_path, &g_MasterList))
    {
        snprintf(vdf_path, sizeof(vdf_path), "platform\\config\\MasterServers.vdf");
        if (!vdf_parse_master_servers(vdf_path, &g_MasterList))
        {
            strcpy(g_MasterList.entries[0].addr, "95.173.174.197:27010");
            strcpy(g_MasterList.entries[1].addr, "95.173.174.198:27010");
            strcpy(g_MasterList.entries[2].addr, "185.252.233.104:27010");
            g_MasterList.count = 3;
        }
    }
}

CRealMasterMatchmaking::CRealMasterMatchmaking()
{
    m_serverCount = 0;
    m_refreshing = false;
    m_pResponse = NULL;
    m_hThread = NULL;
    m_requestCounter = 0;
    m_queryDone = false;
    m_cancelRequested = false;
    m_lastDispatchedIdx = 0;
    m_dispatching = false;
    m_pRealSteam = NULL;
    memset(m_servers, 0, sizeof(m_servers));
    memset(m_dispatched, 0, sizeof(m_dispatched));
    
    VerboseLog("CRealMasterMatchmaking initialized.");
}

CRealMasterMatchmaking::~CRealMasterMatchmaking()
{
    VerboseLog("CRealMasterMatchmaking destroyed.");
    if (m_hThread)
    {
        m_cancelRequested = true;
        WaitForSingleObject(m_hThread, 2000);
        CloseHandle(m_hThread);
    }
}

static bool IsThreadAlive(HANDLE h)
{
    if (!h) return false;
    DWORD code;
    return GetExitCodeThread(h, &code) && code == STILL_ACTIVE;
}

DWORD WINAPI CRealMasterMatchmaking::QueryThread(LPVOID param)
{
    QueryThreadData *data = (QueryThreadData *)param;
    uint32_t myReqId = data->reqId;
    CRealMasterMatchmaking *self = data->self;

    auto is_active = [&]() {
        return !self->m_cancelRequested && self->m_requestCounter == myReqId;
    };

    RealMasterLog("QueryThread started [ID: %u]", myReqId);
    VerboseLog("Thread %u running...", myReqId);

    load_master_list();

    master_query_result_t master_result;
    memset(&master_result, 0, sizeof(master_result));
    int total = 0;

    for (int m = 0; m < g_MasterList.count && is_active(); m++)
    {
        VerboseLog("Requesting servers from master %s", g_MasterList.entries[m].addr);
        master_query_result_t result;
        if (master_query_servers(g_MasterList.entries[m].addr, &result))
        {
            VerboseLog("Received %d server IPs from Master Server", result.count);
            for (int i = 0; i < result.count && total < MAX_GAME_SERVERS; i++)
            {
                master_result.servers[total] = result.servers[i];
                total++;
            }
            break;
        }
    }

    if (!is_active()) { 
        VerboseLog("Thread %u killed before reading cache.", myReqId); 
        delete data; return 0; 
    }

    if (total == 0)
    {
        char cache_path[512];
        snprintf(cache_path, sizeof(cache_path), "%s\\cache\\servers.dat", g_PluginDir);
        server_cache_t cache;
        if (cache_load(cache_path, &cache))
        {
            for (int i = 0; i < cache.count && i < MAX_GAME_SERVERS; i++)
            {
                master_result.servers[i].ip = cache.servers[i].ip;
                master_result.servers[i].port = cache.servers[i].port;
            }
            total = cache.count;
            VerboseLog("Loaded %d servers from fallback cache.", total);
        }
    }
    else
    {
        char cache_path[512];
        snprintf(cache_path, sizeof(cache_path), "%s\\cache\\servers.dat", g_PluginDir);
        server_cache_t cache;
        cache.count = 0;
        for (int i = 0; i < total && cache.count < MAX_CACHED_SERVERS; i++)
        {
            cache.servers[cache.count].ip = master_result.servers[i].ip;
            cache.servers[cache.count].port = master_result.servers[i].port;
            cache.count++;
        }
        cache_save(cache_path, &cache);
    }

    if (!is_active()) { delete data; return 0; }

    for (int i = 0; i < total; i++)
    {
        gameserveritem_t *gs = &data->servers[i];
        memset(gs, 0, sizeof(*gs));
        gs->m_NetAdr.Init(ntohl(master_result.servers[i].ip),
            ntohs(master_result.servers[i].port),
            ntohs(master_result.servers[i].port));
        gs->m_bHadSuccessfulResponse = false;
    }
    *data->serverCount = total;

    VerboseLog("A2S Queries starting for %d servers...", total);

    const int WINDOW = 64;
    const int PER_SERVER_TIMEOUT = 2000;

    SOCKET socks[MAX_GAME_SERVERS];
    DWORD sendTimes[MAX_GAME_SERVERS];
    bool challenged[MAX_GAME_SERVERS];

    for (int i = 0; i < total; i++)
    {
        socks[i] = INVALID_SOCKET;
        challenged[i] = false;
    }

    int nextToSend = 0;
    int activeCount = 0;
    int responded = 0;
    int finished = 0;

    auto sendQuery = [&](int idx) {
        socks[idx] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socks[idx] == INVALID_SOCKET) { finished++; return; }

        struct sockaddr_in dest;
        memset(&dest, 0, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_addr.s_addr = master_result.servers[idx].ip;
        dest.sin_port = master_result.servers[idx].port;

        sendTimes[idx] = GetTickCount();
        sendto(socks[idx], (const char *)A2S_INFO_REQ_BYTES, sizeof(A2S_INFO_REQ_BYTES), 0,
            (struct sockaddr *)&dest, sizeof(dest));
        activeCount++;
    };

    while (nextToSend < total && activeCount < WINDOW && is_active())
    {
        sendQuery(nextToSend);
        nextToSend++;
    }

    while (activeCount > 0 && is_active())
    {
        fd_set readfds;
        FD_ZERO(&readfds);
        SOCKET max_sock = 0;
        int fdCount = 0;

        for (int i = 0; i < nextToSend && fdCount < FD_SETSIZE; i++)
        {
            if (socks[i] == INVALID_SOCKET) continue;
            FD_SET(socks[i], &readfds);
            if (socks[i] > max_sock) max_sock = socks[i];
            fdCount++;
        }
        if (fdCount == 0) break;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50000;

        int sel = select((int)max_sock + 1, &readfds, NULL, NULL, &tv);

        if (sel > 0)
        {
            for (int i = 0; i < nextToSend && sel > 0; i++)
            {
                if (socks[i] == INVALID_SOCKET) continue;
                if (!FD_ISSET(socks[i], &readfds)) continue;

                uint8_t buf[2048];
                struct sockaddr_in from;
                int fromlen = sizeof(from);
                int recv_len = recvfrom(socks[i], (char *)buf, sizeof(buf), 0,
                    (struct sockaddr *)&from, &fromlen);

                if (!is_active()) {
                    VerboseLog("Thread %u aborted during socket read.", myReqId);
                    break;
                }

                DWORD elapsed = GetTickCount() - sendTimes[i];
                gameserveritem_t *gs = &data->servers[i];

                if (recv_len >= 9 && buf[0] == 0xFF && buf[1] == 0xFF && 
                    buf[2] == 0xFF && buf[3] == 0xFF && buf[4] == 0x41 && !challenged[i])
                {
                    challenged[i] = true;
                    uint8_t req_challenge[64];
                    memcpy(req_challenge, A2S_INFO_REQ_BYTES, sizeof(A2S_INFO_REQ_BYTES));
                    memcpy(req_challenge + sizeof(A2S_INFO_REQ_BYTES), buf + 5, 4);

                    struct sockaddr_in dest;
                    memset(&dest, 0, sizeof(dest));
                    dest.sin_family = AF_INET;
                    dest.sin_addr.s_addr = master_result.servers[i].ip;
                    dest.sin_port = master_result.servers[i].port;

                    sendTimes[i] = GetTickCount(); 
                    sendto(socks[i], (const char *)req_challenge, (int)(sizeof(A2S_INFO_REQ_BYTES) + 4), 0, (struct sockaddr *)&dest, sizeof(dest));
                    
                    sel--;
                    continue; 
                }

                a2s_server_info_t info;
                memset(&info, 0, sizeof(info));
                if (recv_len > 0 && parse_a2s_response(buf, recv_len, &info))
                {
                    gs->m_nPing = (int)elapsed;
                    gs->SetName(info.name);
                    
                    strncpy(gs->m_szMap, info.map, sizeof(gs->m_szMap) - 1);
                    gs->m_szMap[sizeof(gs->m_szMap) - 1] = '\0';
                    strncpy(gs->m_szGameDir, info.gamedir, sizeof(gs->m_szGameDir) - 1);
                    gs->m_szGameDir[sizeof(gs->m_szGameDir) - 1] = '\0';
                    strncpy(gs->m_szGameDescription, info.gamedesc, sizeof(gs->m_szGameDescription) - 1);
                    gs->m_szGameDescription[sizeof(gs->m_szGameDescription) - 1] = '\0';
                    
                    gs->m_nAppID = info.appid;
                    gs->m_nPlayers = info.players;
                    gs->m_nMaxPlayers = info.max_players;
                    gs->m_nBotPlayers = info.bots;
                    gs->m_bPassword = info.password != 0;
                    gs->m_bSecure = info.secure != 0;
                    
                    MemoryBarrier(); 
                    gs->m_bHadSuccessfulResponse = true;
                    responded++;
                    
                    // Her 10 sunucuda bir konsolu spamlama amacıyla log sınırlandı
                    if (responded % 10 == 0) {
                        VerboseLog("Parsed A2S_INFO for 10 more servers. Total so far: %d", responded);
                    }
                }

                closesocket(socks[i]);
                socks[i] = INVALID_SOCKET;
                activeCount--;
                finished++;
                sel--;

                if (nextToSend < total && is_active())
                {
                    sendQuery(nextToSend);
                    nextToSend++;
                }
            }
        }

        DWORD now = GetTickCount();
        for (int i = 0; i < nextToSend; i++)
        {
            if (socks[i] == INVALID_SOCKET) continue;
            if (now - sendTimes[i] > (DWORD)PER_SERVER_TIMEOUT)
            {
                closesocket(socks[i]);
                socks[i] = INVALID_SOCKET;
                activeCount--;
                finished++;

                if (nextToSend < total && is_active())
                {
                    sendQuery(nextToSend);
                    nextToSend++;
                }
            }
        }
    }

    for (int i = 0; i < nextToSend; i++)
    {
        if (socks[i] != INVALID_SOCKET)
            closesocket(socks[i]);
    }

    VerboseLog("QueryThread [ID: %u] exiting naturally. Processed: %d, Responded: %d", myReqId, finished, responded);
    
    if (is_active()) {
        self->m_queryDone = true;
    }
    
    delete data;
    return 0;
}

HServerListRequest CRealMasterMatchmaking::RequestInternetServerList(
    uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters,
    ISteamMatchmakingServerListResponse *pResponse)
{
    m_requestCounter++; 
    m_cancelRequested = true;

    VerboseLog("UI Called RequestInternetServerList | New ID: %u", m_requestCounter);

    if (m_hThread)
    {
        if (IsThreadAlive(m_hThread))
        {
            VerboseLog("Waiting for previous Thread to safely close...");
            WaitForSingleObject(m_hThread, INFINITE);
            VerboseLog("Previous thread closed safely.");
        }
        CloseHandle(m_hThread); 
        m_hThread = NULL;
    }

    m_serverCount = 0;
    m_refreshing = true;
    m_queryDone = false;
    m_cancelRequested = false;
    m_lastDispatchedIdx = 0;
    memset(m_dispatched, 0, sizeof(m_dispatched));
    memset(m_servers, 0, sizeof(m_servers)); 
    m_pResponse = pResponse;

    QueryThreadData *data = new QueryThreadData;
    data->self = this;
    data->servers = m_servers;
    data->serverCount = &m_serverCount;
    data->reqId = m_requestCounter;

    m_hThread = CreateThread(NULL, 0, QueryThread, data, 0, NULL);
    
    return (HServerListRequest)(uintptr_t)m_requestCounter;
}

HServerListRequest CRealMasterMatchmaking::RequestLANServerList(uint32_t iApp, ISteamMatchmakingServerListResponse *pResponse)
{
    if (m_pRealSteam) return m_pRealSteam->RequestLANServerList(iApp, pResponse);
    return NULL;
}

HServerListRequest CRealMasterMatchmaking::RequestFriendsServerList(uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, ISteamMatchmakingServerListResponse *pResponse)
{
    if (m_pRealSteam) return m_pRealSteam->RequestFriendsServerList(iApp, ppchFilters, nFilters, pResponse);
    return NULL;
}

HServerListRequest CRealMasterMatchmaking::RequestFavoritesServerList(uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, ISteamMatchmakingServerListResponse *pResponse)
{
    if (m_pRealSteam) return m_pRealSteam->RequestFavoritesServerList(iApp, ppchFilters, nFilters, pResponse);
    return NULL;
}

HServerListRequest CRealMasterMatchmaking::RequestHistoryServerList(uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, ISteamMatchmakingServerListResponse *pResponse)
{
    if (m_pRealSteam) return m_pRealSteam->RequestHistoryServerList(iApp, ppchFilters, nFilters, pResponse);
    return NULL;
}

HServerListRequest CRealMasterMatchmaking::RequestSpectatorServerList(uint32_t iApp, MatchMakingKeyValuePair_t **ppchFilters, uint32_t nFilters, ISteamMatchmakingServerListResponse *pResponse)
{
    if (pResponse) pResponse->RefreshComplete(NULL, eNoServersListedOnMasterServer);
    return NULL;
}

static bool IsOurRequest(HServerListRequest hRequest, uint32_t counter)
{
    return hRequest == (HServerListRequest)(uintptr_t)counter && counter != 0;
}

void CRealMasterMatchmaking::ReleaseRequest(HServerListRequest hRequest)
{
    if (IsOurRequest(hRequest, m_requestCounter))
    {
        VerboseLog("UI Called ReleaseRequest for ID: %u", (uint32_t)(uintptr_t)hRequest);
        m_cancelRequested = true;
        m_refreshing = false;
        
        if (m_hThread)
        {
            if (IsThreadAlive(m_hThread)) {
                WaitForSingleObject(m_hThread, INFINITE);
            }
            CloseHandle(m_hThread);
            m_hThread = NULL;
        }
        m_pResponse = NULL;
        return;
    }
    if (m_pRealSteam) m_pRealSteam->ReleaseRequest(hRequest);
}

gameserveritem_t *CRealMasterMatchmaking::GetServerDetails(HServerListRequest hRequest, int iServer)
{
    if (IsOurRequest(hRequest, m_requestCounter))
    {
        // Spams too much if enabled for every tick, we log mostly out-of-bounds attempts
        if (iServer < 0 || iServer >= m_serverCount) {
            VerboseLog("WARNING: UI tried to access invalid Server Index %d (Max: %d)", iServer, m_serverCount);
            return NULL;
        }
        return &m_servers[iServer];
    }
    if (m_pRealSteam) return m_pRealSteam->GetServerDetails(hRequest, iServer);
    return NULL;
}

void CRealMasterMatchmaking::CancelQuery(HServerListRequest hRequest)
{
    if (IsOurRequest(hRequest, m_requestCounter))
    {
        VerboseLog("UI Called CancelQuery for ID: %u", m_requestCounter);
        m_cancelRequested = true;
        
        if (m_hThread && IsThreadAlive(m_hThread)) {
            WaitForSingleObject(m_hThread, INFINITE);
        }

        if (m_pResponse)
        {
            m_pResponse->RefreshComplete(hRequest, eNoServersListedOnMasterServer);
        }
        m_refreshing = false;
        return;
    }
    if (m_pRealSteam) m_pRealSteam->CancelQuery(hRequest);
}

void CRealMasterMatchmaking::RefreshQuery(HServerListRequest hRequest)
{
    if (IsOurRequest(hRequest, m_requestCounter)) return;
    if (m_pRealSteam) m_pRealSteam->RefreshQuery(hRequest);
}

void CRealMasterMatchmaking::DispatchCallbacks()
{
    if (!m_pResponse) return;
    if (m_dispatching) return;
    m_dispatching = true;

    if (m_cancelRequested)
    {
        m_dispatching = false;
        return;
    }

    HServerListRequest hReq = (HServerListRequest)(uintptr_t)m_requestCounter;
    int current = m_serverCount;

    int dispatched = 0;
    int maxPerFrame = 20;
    for (int i = 0; i < current && dispatched < maxPerFrame; i++)
    {
        if (m_dispatched[i]) continue;
        if (m_servers[i].m_bHadSuccessfulResponse)
        {
            m_pResponse->ServerResponded(hReq, i);
            m_dispatched[i] = true;
            m_lastDispatchedIdx++;
            dispatched++;
        }
        else if (m_queryDone)
        {
            m_pResponse->ServerFailedToRespond(hReq, i);
            m_dispatched[i] = true;
            m_lastDispatchedIdx++;
        }
    }
    
    if (dispatched > 0) {
        VerboseLog("Engine requested dispatch: Handed %d servers to UI. (Total Dispatched: %d)", dispatched, m_lastDispatchedIdx);
    }

    if (m_queryDone && !m_cancelRequested && m_lastDispatchedIdx >= m_serverCount)
    {
        int responded = 0;
        for (int i = 0; i < m_serverCount; i++)
            if (m_servers[i].m_bHadSuccessfulResponse) responded++;
            
        VerboseLog("Query Done & UI List is Full. Dispatched RefreshComplete.");
        EMatchMakingServerResponse resp = (m_serverCount > 0) ?
            eServerResponded : eNoServersListedOnMasterServer;
        m_pResponse->RefreshComplete(hReq, resp);
        m_refreshing = false;
        m_queryDone = false;
    }

    m_dispatching = false;
}

bool CRealMasterMatchmaking::IsRefreshing(HServerListRequest hRequest)
{
    if (IsOurRequest(hRequest, m_requestCounter))
    {
        DispatchCallbacks();
        return m_refreshing || IsThreadAlive(m_hThread);
    }
    if (m_pRealSteam) return m_pRealSteam->IsRefreshing(hRequest);
    return false;
}

int CRealMasterMatchmaking::GetServerCount(HServerListRequest hRequest)
{
    if (IsOurRequest(hRequest, m_requestCounter))
    {
        DispatchCallbacks();
        return m_lastDispatchedIdx;
    }
    if (m_pRealSteam) return m_pRealSteam->GetServerCount(hRequest);
    return 0;
}

void CRealMasterMatchmaking::RefreshServer(HServerListRequest hRequest, int iServer)
{
    if (!IsOurRequest(hRequest, m_requestCounter))
    {
        if (m_pRealSteam) m_pRealSteam->RefreshServer(hRequest, iServer);
        return;
    }
    if (iServer < 0 || iServer >= m_serverCount) return;
    
    VerboseLog("UI Requested Refresh for Server Index: %d", iServer);

    gameserveritem_t *gs = &m_servers[iServer];
    uint32_t ip_net = htonl(gs->m_NetAdr.GetIP());
    uint16_t port_net = htons(gs->m_NetAdr.GetQueryPort());

    a2s_server_info_t info;
    memset(&info, 0, sizeof(info));
    if (a2s_query_server(ip_net, port_net, &info))
    {
        gs->m_nPing = info.ping_ms;
        gs->SetName(info.name);
        
        strncpy(gs->m_szMap, info.map, sizeof(gs->m_szMap) - 1);
        gs->m_szMap[sizeof(gs->m_szMap) - 1] = '\0';
        
        gs->m_nPlayers = info.players;
        gs->m_nMaxPlayers = info.max_players;
        gs->m_nBotPlayers = info.bots;
        gs->m_bPassword = info.password != 0;
        gs->m_bSecure = info.secure != 0;
        
        MemoryBarrier();
        gs->m_bHadSuccessfulResponse = true;
    }
}

HServerQuery CRealMasterMatchmaking::PingServer(uint32_t unIP, uint16_t usPort, ISteamMatchmakingPingResponse *pResponse)
{
    if (m_pRealSteam) return m_pRealSteam->PingServer(unIP, usPort, pResponse);
    return -1;
}

HServerQuery CRealMasterMatchmaking::PlayerDetails(uint32_t unIP, uint16_t usPort, ISteamMatchmakingPlayersResponse *pResponse)
{
    if (m_pRealSteam) return m_pRealSteam->PlayerDetails(unIP, usPort, pResponse);
    return -1;
}

HServerQuery CRealMasterMatchmaking::ServerRules(uint32_t unIP, uint16_t usPort, ISteamMatchmakingRulesResponse *pResponse)
{
    if (m_pRealSteam) return m_pRealSteam->ServerRules(unIP, usPort, pResponse);
    return -1;
}

void CRealMasterMatchmaking::CancelServerQuery(HServerQuery hServerQuery)
{
    if (m_pRealSteam) m_pRealSteam->CancelServerQuery(hServerQuery);
}

ISteamMatchmakingServers *GetRealMasterMatchmaking()
{
    return &g_RealMaster;
}

void SetRealSteamMatchmaking(ISteamMatchmakingServers *pReal)
{
    g_RealMaster.m_pRealSteam = pReal;
}
