#pragma once
#include "IOCPServer.h"
#include <map>
#include <vector>
#include <mutex>
#include <set>
#include <algorithm>

#define ARENA_W 160
#define ARENA_H 40

// IOCP 서버의 한 갈래로 클라이언트들을 2차원 평면에서 마구 돌아다니게(Roam) 하는 서버
class RoamServer : public IOCPServer {
private:
    struct GameCommand { int pId; char input; bool isDisconnect; };
    struct Player { int id; char sym; float x, y; Session* session; };

    std::map<int, Player*> players;
    std::vector<GameCommand> cmdQueue;
    std::mutex qMtx;
    int idCounter = 1;

    // Static buffers for bundled data (N size, not N^2)
    char g_bundleBuffers[MAX_PLAYERS][16384]; 

protected:
    void OnRawConnect(Session* s) override {
        s->playerId = idCounter++;
        const char* syms = "@#$&%?!*VWXYZKMLNHPabcdefghijklmnopqrstuvwxyz0123456789+={}<>";
        players[s->playerId] = new Player{ s->playerId, syms[s->playerId % 61], 20.0f, 7.0f, s };
        
        char wMsg[64];
        int len = sprintf_s(wMsg, "W %d %c %.1f %.1f\n", s->playerId, syms[s->playerId % 61], 20.0f, 7.0f);
        send(s->socket, wMsg, len, 0);
    }

    void OnRawReceive(int pId, char* data, int len) override {
        std::lock_guard<std::mutex> lock(qMtx);
        cmdQueue.push_back({ pId, data[0], false });
    }

    void OnRawDisconnect(int pId) override {
        std::lock_guard<std::mutex> lock(qMtx);
        cmdQueue.push_back({ pId, 0, true });
    }

    // 게임 상태를 업데이트하는 루틴
    void OnUpdate() override {
        std::vector<GameCommand> frameCmds;
        {
            std::lock_guard<std::mutex> lock(qMtx);
            if (cmdQueue.empty()) return;
            frameCmds.swap(cmdQueue);
        }

        std::reverse(frameCmds.begin(), frameCmds.end());
        std::set<int> movedThisFrame;
        
        // Master frame buffer to hold all U updates for the current tick
        static char masterFrameBuffer[16384]; 
        int masterLen = 0;

        for (auto& cmd : frameCmds) {
            if (cmd.isDisconnect) {
                if (players.count(cmd.pId)) {
                    char dMsg[32];
                    int dLen = sprintf_s(dMsg, "D %d\n", cmd.pId);
                    delete players[cmd.pId];
                    players.erase(cmd.pId);
                    for (auto const& [id, p] : players) send(p->session->socket, dMsg, dLen, 0);
                }
                continue;
            }

            if (movedThisFrame.count(cmd.pId) || !players.count(cmd.pId)) continue;
            movedThisFrame.insert(cmd.pId);

            Player* p = players[cmd.pId];
            if (cmd.input == 'W') p->y -= 1.0f;
            else if (cmd.input == 'S') p->y += 1.0f;
            else if (cmd.input == 'A') p->x -= 1.0f;
            else if (cmd.input == 'D') p->x += 1.0f;

            p->x = (std::max)(1.0f, (std::min)((float)ARENA_W - 2, p->x));
            p->y = (std::max)(1.0f, (std::min)((float)ARENA_H - 2, p->y));

            // Append this movement to the bundled string
            masterLen += sprintf_s(masterFrameBuffer + masterLen, 16384 - masterLen, 
                                   "U %d %c %.1f %.1f\n", p->id, p->sym, p->x, p->y);
        }

        if (masterLen <= 0) return;

        // Broadcast the bundle to everyone
        for (auto const& [targetId, targetPlayer] : players) {
            Session* s = targetPlayer->session;
            
            // Only send if the previous bundle has been fully transmitted
            if (!s->isSending) {
                memcpy(g_bundleBuffers[targetId], masterFrameBuffer, masterLen);

                WSABUF wsaBuf;
                wsaBuf.buf = g_bundleBuffers[targetId];
                wsaBuf.len = (ULONG)masterLen;

                ZeroMemory(&s->sendOverlapped, sizeof(WSAOVERLAPPED));
                s->isSending = true;

                if (WSASend(s->socket, &wsaBuf, 1, NULL, 0, &s->sendOverlapped, NULL) == SOCKET_ERROR) {
                    if (WSAGetLastError() != WSA_IO_PENDING) s->isSending = false;
                }
            }
        }
        Sleep(20);
    }
};