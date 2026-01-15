#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

#define MAX_PLAYERS 2001

// 세션별로 송/수신 overlapped를 따로 관리
struct Session {
    WSAOVERLAPPED recvOverlapped;
    WSAOVERLAPPED sendOverlapped; // Dedicated overlapped for bundling
    SOCKET socket;
    char buffer[1024];
    WSABUF wsaBuf;
    int playerId;
    bool isSending; // The "Gate" for this player
};

class IOCPServer {
public:
    virtual ~IOCPServer() { Stop(); }

    // 포트에 리스너 소켓을 만들고 클라이언트로부터 수신을 받는 워커스레드, 게임 로직을 실행하는 업데이트 스레드 생성
    bool Start(int port) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);

        hIocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
        listenSock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        sockaddr_in addr = { AF_INET, htons(port), INADDR_ANY };
        bind(listenSock, (sockaddr*)&addr, sizeof(addr));
        listen(listenSock, SOMAXCONN);

        isRunning = true;
        int threads = std::thread::hardware_concurrency();
        for (int i = 0; i < threads; i++) std::thread(&IOCPServer::WorkerRoutine, this).detach();
        std::thread(&IOCPServer::UpdateRoutine, this).detach();

        return true;
    }

    // 무한루프를 돌며 Accept받는 구간
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
    // 업데이트 스레드를 위해 존재하는 가상 함수
    virtual void OnUpdate() = 0;
    // 패킷이 도착했을 때 워커 스레드에서 처리되는 가상함수
    virtual void OnRawReceive(int playerId, char* data, int len) = 0;
    virtual void OnRawConnect(Session* s) = 0;
    virtual void OnRawDisconnect(int playerId) = 0;
    HANDLE hIocp;

private:
    SOCKET listenSock;
    bool isRunning = false;

    void UpdateRoutine() {
        while (isRunning) {
            OnUpdate();
            Sleep(16);
        }
    }

    // CPU 개수 * 2 숫자로 돌아가는 워커 스레드들이 돌리는 함수
    void WorkerRoutine() {
        DWORD bytes;
        ULONG_PTR key;
        LPOVERLAPPED lpOverlapped;
        while (GetQueuedCompletionStatus(hIocp, &bytes, &key, &lpOverlapped, INFINITE)) {
            if (!lpOverlapped) continue;
            Session* s = (Session*)key;

            // send가 끝난거면 다시 소켓 게이트를 열어준다.
            if (lpOverlapped == &s->sendOverlapped) {
                s->isSending = false;
                continue;
            }

            // RECV Completion
            if (bytes == 0) {
                OnRawDisconnect(s->playerId);
                closesocket(s->socket);
                delete s;
                continue;
            }
            OnRawReceive(s->playerId, s->buffer, bytes);

            DWORD flags = 0;
            ZeroMemory(&s->recvOverlapped, sizeof(WSAOVERLAPPED));
            s->wsaBuf.buf = s->buffer;
            s->wsaBuf.len = sizeof(s->buffer);
            WSARecv(s->socket, &s->wsaBuf, 1, NULL, &flags, &s->recvOverlapped, NULL);
        }
    }

    // accept할 경우 세션을 만들어주고 WSA 통신을 위한 세팅 작업을 진행한다.
    void OnAccept(SOCKET cSock) {
        BOOL bNoDelay = TRUE;
        setsockopt(cSock, IPPROTO_TCP, TCP_NODELAY, (const char*)&bNoDelay, sizeof(bNoDelay));

        Session* s = new Session();
        s->socket = cSock;
        s->isSending = false;
        ZeroMemory(&s->recvOverlapped, sizeof(WSAOVERLAPPED));
        ZeroMemory(&s->sendOverlapped, sizeof(WSAOVERLAPPED));
        
        OnRawConnect(s);
        CreateIoCompletionPort((HANDLE)cSock, hIocp, (ULONG_PTR)s, 0);

        DWORD flags = 0;
        s->wsaBuf.buf = s->buffer;
        s->wsaBuf.len = sizeof(s->buffer);
        WSARecv(cSock, &s->wsaBuf, 1, NULL, &flags, &s->recvOverlapped, NULL);
    }
};