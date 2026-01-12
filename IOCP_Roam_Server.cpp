#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <string>
#include <map>
#include <unordered_set>
#include "RoamServer.h"

#pragma comment(lib, "ws2_32.lib")

int main() {
    RoamServer server;
    if (server.Start(9000)) server.Run();
    return 0;
}
