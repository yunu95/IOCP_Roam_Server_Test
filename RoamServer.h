#include <map>
#include <vector>
#include <mutex>
#include <algorithm>
#include <iostream>
#include <set>
#include "IOCPServer.h"

#define ARENA_W 160
#define ARENA_H 40
#define MAX_PLAYERS 5000

class RoamServer : public IOCPServer {
private:
    // Command defined ONLY in the child
    struct GameCommand {
        int pId;
        char input;
        bool isDisconnect;
    };

    struct Player { int id; char sym; float x, y; SOCKET sock; };

    std::map<int, Player*> players;
    std::vector<GameCommand> cmdQueue;
    std::mutex qMtx;
    int idCounter = 1;

protected:
    void OnRawConnect(Session* s) override {
        // We still use a local lock here for the join/leave map management
        s->playerId = idCounter++;
        const char* syms = "@#$&%?!*VWXYZKMLNHPabcdefghijklmnopqrstuvwxyz0123456789+={}<>";
        players[s->playerId] = new Player{ s->playerId, syms[s->playerId % 61], 20.0f, 7.0f, s->socket };
        std::cout << "Player " << s->playerId << " joined.\n";

        // 들어온 플레이어에게 자신의 id를 알려준다. 
        char uMsg[64];
        auto p = players[s->playerId];
        sprintf_s(uMsg, "W %d %c %.1f %.1f\n", p->id, p->sym, p->x, p->y);
        send(p->sock, uMsg, (int)strlen(uMsg), 0);
    }

    void OnRawReceive(int pId, char* data, int len) override {
        std::lock_guard<std::mutex> lock(qMtx);
        cmdQueue.push_back({ pId, data[0], false });
    }

    void OnRawDisconnect(int pId) override {
        std::lock_guard<std::mutex> lock(qMtx);
        cmdQueue.push_back({ pId, 0, true });
    }
    struct BroadCastSendContext {
        WSAOVERLAPPED overlapped;
        int playerId;
    };
    static BroadCastSendContext broadCastOverlapped[MAX_PLAYERS];
    static bool g_sendingBroadCast[MAX_PLAYERS];
    static WSABUF broadCastWSABuf[MAX_PLAYERS];
    static void CALLBACK BroadCastSendCallback(DWORD dwError, DWORD cbTransferred, LPWSAOVERLAPPED lpOverlapped, DWORD dwFlags)
    {
        // We cast the pointer back to our containing structure
        BroadCastSendContext* ctx = reinterpret_cast<BroadCastSendContext*>(lpOverlapped);
        // Open the gate using the clean ID
        g_sendingBroadCast[ctx->playerId] = false;
    }

    // FULLY IMPLEMENTED OnUpdate
    void OnUpdate() override {
        static std::set<int> processedIds;
        processedIds.clear();
        std::vector<GameCommand> frameCmds;
        {
            std::lock_guard<std::mutex> lock(qMtx);
            if (cmdQueue.empty()) return;
            frameCmds.swap(cmdQueue);
        }

        // Process in REVERSE (Newest first) as requested
        std::reverse(frameCmds.begin(), frameCmds.end());

        for (auto& cmd : frameCmds) {
            // 1. Handle Disconnection
            if (cmd.isDisconnect) {
                if (players.count(cmd.pId)) {
                    char dMsg[32];
                    sprintf_s(dMsg, "D %d\n", cmd.pId);
                    delete players[cmd.pId];
                    players.erase(cmd.pId);
                    for (auto const& [id, p] : players) send(p->sock, dMsg, (int)strlen(dMsg), 0);
                }
                processedIds.insert(cmd.pId);
                continue;
            }
            if (processedIds.find(cmd.pId) != processedIds.end())
                continue;
            else
                processedIds.insert(cmd.pId);

            // 2. Handle Movement
            if (players.count(cmd.pId)) {
                Player* p = players[cmd.pId];
                if (cmd.input == 'W') p->y -= 1.0f;
                else if (cmd.input == 'S') p->y += 1.0f;
                else if (cmd.input == 'A') p->x -= 1.0f;
                else if (cmd.input == 'D') p->x += 1.0f;

                // 3. Boundary Check
                if (p->x < 1) p->x = 1;
                if (p->x > ARENA_W - 2) p->x = ARENA_W - 2;
                if (p->y < 1) p->y = 1;
                if (p->y > ARENA_H - 2) p->y = ARENA_H - 2;

                // 4. Broadcast Update
                // uBroadCastMsg[id]는 id 플레이어의 현재 위치 정보를 담고 있음.
                static char uBroadCastMsg[MAX_PLAYERS][64];
                int msgLen = sprintf_s(uBroadCastMsg[cmd.pId], "U %d %c %.1f %.1f\n", p->id, p->sym, p->x, p->y);
                broadCastWSABuf[cmd.pId].buf = uBroadCastMsg[cmd.pId];
                broadCastWSABuf[cmd.pId].len = msgLen;

                DWORD bytesSent = 0;

                for (auto const& [id, other] : players) {
                    ZeroMemory(&broadCastOverlapped[cmd.pId], sizeof(BroadCastSendContext));
                    broadCastOverlapped[id].playerId = id;
                    send(other->sock, uBroadCastMsg[cmd.pId], msgLen, 0);
                    if (!g_sendingBroadCast[id])
                    {
                        int result = WSASend(other->sock, &broadCastWSABuf[cmd.pId], 1, &bytesSent, 0, &broadCastOverlapped[other->id].overlapped, BroadCastSendCallback);
                        if (result == SOCKET_ERROR)
                        {
                            int damn = WSAGetLastError();
                            damn = WSAGetLastError();
                        }
                        else
                        {
                            g_sendingBroadCast[id] = true;
                        }
                    }
                    else
                    {
                        int trash = 0;
                    }
                }
            }
        }
        //Sleep(20);
    }
};

RoamServer::BroadCastSendContext RoamServer::broadCastOverlapped[MAX_PLAYERS];
bool RoamServer::g_sendingBroadCast[MAX_PLAYERS] = { false };
WSABUF RoamServer::broadCastWSABuf[MAX_PLAYERS];
