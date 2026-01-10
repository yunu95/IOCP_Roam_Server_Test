#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <string>
#include <map>
#include <unordered_set>

#pragma comment(lib, "ws2_32.lib")

#define MAX_BUFFER 1024
#define MAP_WIDTH 40
#define MAP_HEIGHT 15

// 플레이어 정보
struct PlayerInfo {
    int id;
    char symbol;
    float x, y;
    SOCKET socket;
};
struct Command {
    int playerId;
    char move;
};

std::vector<Command> g_commandQueue;
std::mutex g_queueLock; // Locking a queue for 1ms is much better than locking game logic

// 플레이어 id별 정보 객체
std::map<int, PlayerInfo*> g_players;
// 최우선 척결대상 뮤텍스
std::mutex g_gameLock;
int g_idCounter = 1;
HANDLE g_hIOCP;

struct Session {
    WSAOVERLAPPED overlapped;
    SOCKET socket;
    char buffer[MAX_BUFFER];
    WSABUF wsaBuf;
    // 세션이 관리하는 플레이어 id
    int playerId;
};

// 전 플레이어들에게 브로드캐스팅
void Broadcast(std::string msg) {
    std::lock_guard<std::mutex> lock(g_gameLock);
    for (auto const& [id, player] : g_players) {
        send(player->socket, msg.c_str(), (int)msg.length(), 0);
    }
}

// 워커 스레드 하나가 다수 플레이어들의 산발적인 요청을 동시에 관리함
void WorkerThread() {
    while (true) {
        DWORD bytesTransferred = 0;
        ULONG_PTR completionKey = 0;
        LPOVERLAPPED pOverlapped = nullptr;

        BOOL ret = GetQueuedCompletionStatus(g_hIOCP, &bytesTransferred, &completionKey, &pOverlapped, INFINITE);
        Session* client = (Session*)pOverlapped;

        // 클라이언트가 연결을 끊은 경우
        if (!ret || bytesTransferred == 0) {
            if (client) {
                std::lock_guard<std::mutex> lock(g_gameLock);

                // 1. 연결해제 메시지 설정
                char dMsg[32];
                sprintf_s(dMsg, "D %d\n", client->playerId);

                // 2. 플레이어 메모리 해제
                if (g_players.count(client->playerId)) {
                    delete g_players[client->playerId];
                    g_players.erase(client->playerId);
                }

                // 3. 플레이어 삭제 메시지 브로드캐스팅
                for (auto const& [id, player] : g_players) {
                    send(player->socket, dMsg, (int)strlen(dMsg), 0);
                }

                std::cout << "Player " << client->playerId << " Disconnected and removed.\n";
                closesocket(client->socket);
                delete client;
            }
            continue;
        }

        // W=Up, S=Down, A=Left, D=Right
        char command = client->buffer[0];
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            g_commandQueue.push_back({ client->playerId, command });
        }

        // 수신 초기화 및 비동기 수신 시작
        ZeroMemory(&client->overlapped, sizeof(WSAOVERLAPPED));
        client->wsaBuf.len = MAX_BUFFER;
        client->wsaBuf.buf = client->buffer;
        DWORD flags = 0;
        WSARecv(client->socket, &client->wsaBuf, 1, NULL, &flags, &client->overlapped, NULL);
    }
}
void LogicThread() {
    // command의 id가 중복되는 경우, 커맨드가 두번 실행되는 것을 막기 위한 중복방지 set
    std::unordered_set<int> processedIds;
    while (true) {
        processedIds.clear();
        std::vector<Command> processingQueue;

        // Swiftly swap the queue to keep lock time near zero
        {
            std::lock_guard<std::mutex> lock(g_queueLock);
            processingQueue.swap(g_commandQueue);
        }

        // Process all moves at once WITHOUT ANY LOCKS
        for (auto it = processingQueue.rbegin(); it != processingQueue.rend(); ++it) {
            Command& cmd = *it;

            // 중복체크
            if (processedIds.find(cmd.playerId) != processedIds.end())
                continue;
            processedIds.insert(cmd.playerId);

            PlayerInfo* p = g_players[cmd.playerId];
            if (!p) continue;

            // Handle Movement
            if (g_players.count(cmd.playerId)) {
                PlayerInfo* p = g_players[cmd.playerId];

                if (cmd.move == 'W') p->y -= 1.0f;
                if (cmd.move == 'S') p->y += 1.0f;
                if (cmd.move == 'A') p->x -= 1.0f;
                if (cmd.move == 'D') p->x += 1.0f;

                // Clamp Boundaries
                if (p->x < 1) p->x = 1;
                if (p->x > MAP_WIDTH - 2) p->x = MAP_WIDTH - 2;
                if (p->y < 1) p->y = 1;
                if (p->y > MAP_HEIGHT - 2) p->y = MAP_HEIGHT - 2;

                // Broadcast Update
                char packet[64];
                sprintf_s(packet, "U %d %c %.1f %.1f\n", p->id, p->symbol, p->x, p->y);
                for (auto const& [id, player] : g_players) {
                    send(player->socket, packet, (int)strlen(packet), 0);
                }
            }
            // Broadcast the new state
            char packet[64];
            sprintf_s(packet, "U %d %c %.1f %.1f\n", p->id, p->symbol, p->x, p->y);
            // Broadcast is now safe because only THIS thread is touching the map
            for (auto const& [id, player] : g_players) {
                send(player->socket, packet, (int)strlen(packet), 0);
            }
        }

        Sleep(16); // Run at ~60 FPS
    }
}

int main() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    g_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(9000);
    bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr));
    listen(listenSocket, SOMAXCONN);

    // Symbols to assign to players sequentially
    const char* symbols = "@#$&%?!*";

    std::thread logicThread(LogicThread);
    unsigned int numCores = std::thread::hardware_concurrency();
    unsigned int numWorkers = numCores * 2;

    for (unsigned int i = 0; i < numWorkers; i++) {
        std::thread worker(WorkerThread);
        worker.detach();
    }

    logicThread.detach();

    std::cout << "Game Server Started on 9000\n";

    // 메인스레드에서는 플레이어의 accept만 처리
    // 플레이어가 들어오면 세션만 생성해주고 세션 관리는 워커스레드에서 진행
    while (true) {
        sockaddr_in clientAddr;
        int len = sizeof(clientAddr);
        SOCKET clientSock = accept(listenSocket, (sockaddr*)&clientAddr, &len);

        if (clientSock != INVALID_SOCKET) {
            // 세션 생성
            Session* newSession = new Session();
            newSession->socket = clientSock;
            newSession->playerId = g_idCounter++; // Assign Unique ID

            // 플레이어를 정중앙에 생성
            PlayerInfo* newPlayer = new PlayerInfo();
            newPlayer->id = newSession->playerId;
            newPlayer->socket = clientSock;
            newPlayer->x = 40.0f; // Start in middle
            newPlayer->y = 12.0f;
            // 플레이어 심볼 설정
            newPlayer->symbol = symbols[newPlayer->id % 8];

            // 맵에 플레이어 정보 등록
            {
                std::lock_guard<std::mutex> lock(g_gameLock);
                g_players[newPlayer->id] = newPlayer;
                std::cout << "Player " << newPlayer->id << " joined as " << newPlayer->symbol << "\n";
            }

            CreateIoCompletionPort((HANDLE)clientSock, g_hIOCP, (ULONG_PTR)newSession, 0);

            // accept한 클라이언트에 대해 비동기 리딩 시작
            newSession->wsaBuf.buf = newSession->buffer;
            newSession->wsaBuf.len = MAX_BUFFER;
            ZeroMemory(&newSession->overlapped, sizeof(WSAOVERLAPPED));
            DWORD flags = 0;
            WSARecv(clientSock, &newSession->wsaBuf, 1, NULL, &flags, &newSession->overlapped, NULL);
        }
    }
    return 0;
}
