#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <vector>
#include <thread>
#include <mutex>
#include <queue>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

struct Session {
    WSAOVERLAPPED overlapped;
    SOCKET socket;
    char buffer[1024];
    WSABUF wsaBuf;
    int playerId;
};

class IOCPServer {
public:
    virtual ~IOCPServer() { Stop(); }

    bool Start(int port) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);

        //listenSock = socket(AF_INET, SOCK_STREAM, 0);
        // Create the listener in Overlapped mode
        listenSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        sockaddr_in addr = { AF_INET, htons(port), INADDR_ANY };
        bind(listenSock, (sockaddr*)&addr, sizeof(addr));
        listen(listenSock, SOMAXCONN);

        isRunning = true;

        // Start Logic/Update Thread
        std::thread(&IOCPServer::UpdateRoutine, this).detach();

        // Start Worker Threads
        int threads = std::thread::hardware_concurrency() * 2;
        for (int i = 0; i < threads; i++) {
            std::thread(&IOCPServer::WorkerRoutine, this).detach();
        }
        return true;
    }

    void Run() {
        while (isRunning) {
            sockaddr_in cAddr;
            int len = sizeof(cAddr);
            SOCKET cSock = WSAAccept(listenSock, (sockaddr*)&cAddr, &len, nullptr, NULL);
            if (cSock != INVALID_SOCKET) OnAccept(cSock);
        }
    }

    void Stop() { isRunning = false; closesocket(listenSock); WSACleanup(); }

protected:
    virtual void OnUpdate() = 0;
    virtual void OnRawReceive(int playerId, char* data, int len) = 0;
    virtual void OnRawConnect(Session* s) = 0;
    virtual void OnRawDisconnect(int playerId) = 0;

private:
    HANDLE hIocp;
    SOCKET listenSock;
    bool isRunning = false;

    void UpdateRoutine() {
        while (isRunning) {
            OnUpdate();
            Sleep(16); // ~60 FPS
        }
    }

    void WorkerRoutine() {
        DWORD bytes;
        ULONG_PTR key;
        LPOVERLAPPED overlapped;
        while (GetQueuedCompletionStatus(hIocp, &bytes, &key, &overlapped, INFINITE)) {
            Session* s = (Session*)overlapped;
            if (bytes == 0) {
                OnRawDisconnect(s->playerId);
                closesocket(s->socket);
                delete s;
                continue;
            }
            OnRawReceive(s->playerId, s->buffer, bytes);

            DWORD flags = 0;
            ZeroMemory(&s->overlapped, sizeof(WSAOVERLAPPED));
            s->wsaBuf.buf = s->buffer;
            s->wsaBuf.len = sizeof(s->buffer);
            WSARecv(s->socket, &s->wsaBuf, 1, NULL, &flags, &s->overlapped, NULL);
        }
    }

    void OnAccept(SOCKET cSock) {
        Session* s = new Session();
        s->socket = cSock;
        OnRawConnect(s);
        CreateIoCompletionPort((HANDLE)cSock, hIocp, (ULONG_PTR)s, 0);
        DWORD flags = 0;
        s->wsaBuf.buf = s->buffer;
        s->wsaBuf.len = sizeof(s->buffer);
        WSARecv(cSock, &s->wsaBuf, 1, NULL, &flags, &s->overlapped, NULL);
    }
};