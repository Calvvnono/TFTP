#include <iostream>
#include <winsock2.h>
#include <string.h>
#include <ctime>
#include <vector>
#include <unordered_map>

#pragma comment(lib, "ws2_32.lib")

#define LIMIT_T 30
#define BUFLEN 1024
const char* MODE[2] = { "netascii", "octet" };

struct ClientInfo {
    sockaddr_in addr;
    FILE* fp;
    int mode;
    clock_t last_active;
};

std::unordered_map<unsigned short, ClientInfo> clients;

void sendError(SOCKET serverSock, const sockaddr_in& clientAddr, int errorCode, const char* errMsg) {
    char sendBuf[BUFLEN];
    sendBuf[0] = 0;
    sendBuf[1] = 5;
    sendBuf[2] = (errorCode >> 8) & 0xFF;
    sendBuf[3] = errorCode & 0xFF;
    strcpy(sendBuf + 4, errMsg);
    int len = 4 + strlen(errMsg) + 1;
    sendto(serverSock, sendBuf, len, 0, (sockaddr*)&clientAddr, sizeof(clientAddr));
    printf("Sent ERROR to %s:%d, Code: %d, Message: %s\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port), errorCode, errMsg);
}

void handleRRQ(SOCKET serverSock, const sockaddr_in& clientAddr, int clientPort, char* filename, int mode) {
    char fullpath[256] = ".\\";
    strcat(fullpath, filename);

    FILE* fp;
    if (mode == 0)
        fp = fopen(fullpath, "r");
    else
        fp = fopen(fullpath, "rb");

    if (fp == NULL) {
        sendError(serverSock, clientAddr, 1, "File not found");
        return;
    }

    ClientInfo clientInfo;
    clientInfo.addr = clientAddr;
    clientInfo.fp = fp;
    clientInfo.mode = mode;
    clientInfo.last_active = clock();
    clients[clientPort] = clientInfo;

    char sendBuf[BUFLEN];
    sendBuf[0] = 0;
    sendBuf[1] = 3;
    sendBuf[2] = 0;
    sendBuf[3] = 1;
    int bytesRead = fread(sendBuf + 4, 1, 512, fp);
    int len = bytesRead + 4;
    sendto(serverSock, sendBuf, len, 0, (sockaddr*)&clientAddr, sizeof(clientAddr));
    printf("Sent DATA block 1 to %s:%d\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
}

void handleWRQ(SOCKET serverSock, const sockaddr_in& clientAddr, int clientPort, char* filename, int mode) {
    char fullpath[256] = ".\\";
    strcat(fullpath, filename);

    FILE* fp;
    if (mode == 0)
        fp = fopen(fullpath, "w");
    else
        fp = fopen(fullpath, "wb");

    if (fp == NULL) {
        sendError(serverSock, clientAddr, 2, "Access violation");
        return;
    }

    ClientInfo clientInfo;
    clientInfo.addr = clientAddr;
    clientInfo.fp = fp;
    clientInfo.mode = mode;
    clientInfo.last_active = clock();
    clients[clientPort] = clientInfo;

    char sendBuf[BUFLEN];
    sendBuf[0] = 0;
    sendBuf[1] = 4;
    sendBuf[2] = 0;
    sendBuf[3] = 0;
    sendto(serverSock, sendBuf, 4, 0, (sockaddr*)&clientAddr, sizeof(clientAddr));
    printf("Sent ACK block 0 to %s:%d\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
}

void handleACK(SOCKET serverSock, const sockaddr_in& clientAddr, int clientPort, int blockNum) {
    auto it = clients.find(clientPort);
    if (it == clients.end()) {
        return;
    }

    ClientInfo& clientInfo = it->second;
    clientInfo.last_active = clock();

    char sendBuf[BUFLEN];
    sendBuf[0] = 0;
    sendBuf[1] = 3;
    sendBuf[2] = (blockNum >> 8) & 0xFF;
    sendBuf[3] = blockNum & 0xFF;
    int bytesRead = fread(sendBuf + 4, 1, 512, clientInfo.fp);
    int len = bytesRead + 4;
    sendto(serverSock, sendBuf, len, 0, (sockaddr*)&clientAddr, sizeof(clientAddr));
    printf("Sent DATA block %d to %s:%d\n", blockNum, inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));

    if (bytesRead < 512) {
        fclose(clientInfo.fp);
        clients.erase(it);
        printf("File transfer complete for %s:%d\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
    }
}

void handleDATA(SOCKET serverSock, const sockaddr_in& clientAddr, int clientPort, int blockNum, char* data, int dataLen) {
    auto it = clients.find(clientPort);
    if (it == clients.end()) {
        return;
    }

    ClientInfo& clientInfo = it->second;
    clientInfo.last_active = clock();

    fwrite(data, 1, dataLen, clientInfo.fp);

    char sendBuf[BUFLEN];
    sendBuf[0] = 0;
    sendBuf[1] = 4;
    sendBuf[2] = (blockNum >> 8) & 0xFF;
    sendBuf[3] = blockNum & 0xFF;
    sendto(serverSock, sendBuf, 4, 0, (sockaddr*)&clientAddr, sizeof(clientAddr));
    printf("Sent ACK block %d to %s:%d\n", blockNum, inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));

    if (dataLen < 512) {
        fclose(clientInfo.fp);
        clients.erase(it);
        printf("File transfer complete for %s:%d\n", inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
    }
}

void cleanupInactiveClients() {
    clock_t now = clock();
    for (auto it = clients.begin(); it != clients.end(); ) {
        if ((now - it->second.last_active) / CLOCKS_PER_SEC >= LIMIT_T) {
            printf("Client %s:%d timed out\n", inet_ntoa(it->second.addr.sin_addr), ntohs(it->second.addr.sin_port));
            if (it->second.fp)
                fclose(it->second.fp);
            it = clients.erase(it);
        }
        else {
            ++it;
        }
    }
}

int main() {
    WSADATA wsaData;
    int nRC;
    SOCKET serverSock;

    nRC = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (nRC != 0) {
        printf("WSAStartup failed with error: %d\n", nRC);
        return 1;
    }

    serverSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (serverSock == INVALID_SOCKET) {
        printf("socket failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(69);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        printf("bind failed with error: %d\n", WSAGetLastError());
        closesocket(serverSock);
        WSACleanup();
        return 1;
    }

    printf("TFTP server started on port 69\n");

    while (true) {
        cleanupInactiveClients();

        char recvBuf[BUFLEN];
        sockaddr_in clientAddr;
        int clientAddrLen = sizeof(clientAddr);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSock, &readfds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int selRet = select(serverSock + 1, &readfds, NULL, NULL, &tv);
        if (selRet == SOCKET_ERROR)
        {
            printf("select failed with error: %d\n", WSAGetLastError());
            closesocket(serverSock);
            WSACleanup();
            return 1;
        }

        if (selRet > 0 && FD_ISSET(serverSock, &readfds)) {
            int bytesReceived = recvfrom(serverSock, recvBuf, BUFLEN, 0, (sockaddr*)&clientAddr, &clientAddrLen);
            if (bytesReceived == SOCKET_ERROR) {
                printf("recvfrom failed with error: %d\n", WSAGetLastError());
                continue;
            }
            
            int clientPort = ntohs(clientAddr.sin_port);
            int opcode = ((unsigned char)recvBuf[0] << 8) + (unsigned char)recvBuf[1];
            switch (opcode) {
            case 1:
            {
                char filename[BUFLEN];
                char modeStr[BUFLEN];
                int i = 2, j = 0;
                while (recvBuf[i] != 0) {
                    filename[j++] = recvBuf[i++];
                }
                filename[j] = 0;
                i++;
                j = 0;
                while (recvBuf[i] != 0) {
                    modeStr[j++] = recvBuf[i++];
                }
                modeStr[j] = 0;
                int mode = (strcmp(modeStr, "netascii") == 0) ? 0 : 1;

                printf("Received RRQ from %s:%d for %s (%s)\n", inet_ntoa(clientAddr.sin_addr), clientPort, filename, modeStr);
                
                clientAddr.sin_port = htons(0);
                if (bind(serverSock, (sockaddr*)&clientAddr, sizeof(clientAddr)) == SOCKET_ERROR) {
                    printf("bind failed with error: %d\n", WSAGetLastError());
                    closesocket(serverSock);
                    WSACleanup();
                    return 1;
                }
                
                getsockname(serverSock, (sockaddr*)&clientAddr, &clientAddrLen);
                
                handleRRQ(serverSock, clientAddr, ntohs(clientAddr.sin_port), filename, mode);
                
                serverAddr.sin_family = AF_INET;
                serverAddr.sin_port = htons(69);
                serverAddr.sin_addr.s_addr = INADDR_ANY;
                if (bind(serverSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
                    printf("bind failed with error: %d\n", WSAGetLastError());
                    closesocket(serverSock);
                    WSACleanup();
                    return 1;
                }
                
                break;
            }
            case 2:
            {
                char filename[BUFLEN];
                char modeStr[BUFLEN];
                int i = 2, j = 0;
                while (recvBuf[i] != 0) {
                    filename[j++] = recvBuf[i++];
                }
                filename[j] = 0;
                i++;
                j = 0;
                while (recvBuf[i] != 0) {
                    modeStr[j++] = recvBuf[i++];
                }
                modeStr[j] = 0;
                int mode = (strcmp(modeStr, "netascii") == 0) ? 0 : 1;

                printf("Received WRQ from %s:%d for %s (%s)\n", inet_ntoa(clientAddr.sin_addr), clientPort, filename, modeStr);

                clientAddr.sin_port = htons(0);
                if (bind(serverSock, (sockaddr*)&clientAddr, sizeof(clientAddr)) == SOCKET_ERROR) {
                    printf("bind failed with error: %d\n", WSAGetLastError());
                    closesocket(serverSock);
                    WSACleanup();
                    return 1;
                }
                
                getsockname(serverSock, (sockaddr*)&clientAddr, &clientAddrLen);

                handleWRQ(serverSock, clientAddr, ntohs(clientAddr.sin_port), filename, mode);

                serverAddr.sin_family = AF_INET;
                serverAddr.sin_port = htons(69);
                serverAddr.sin_addr.s_addr = INADDR_ANY;
                if (bind(serverSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
                    printf("bind failed with error: %d\n", WSAGetLastError());
                    closesocket(serverSock);
                    WSACleanup();
                    return 1;
                }
                break;
            }
            case 3:
            {
                int blockNum = ((unsigned char)recvBuf[2] << 8) + (unsigned char)recvBuf[3];
                printf("Received DATA block %d from %s:%d\n", blockNum, inet_ntoa(clientAddr.sin_addr), clientPort);
                handleDATA(serverSock, clientAddr, clientPort, blockNum, recvBuf + 4, bytesReceived - 4);
                break;
            }
            case 4:
            {
                int blockNum = ((unsigned char)recvBuf[2] << 8) + (unsigned char)recvBuf[3];
                printf("Received ACK block %d from %s:%d\n", blockNum, inet_ntoa(clientAddr.sin_addr), clientPort);
                handleACK(serverSock, clientAddr, clientPort, blockNum + 1);
                break;
            }
            default:
                printf("Received unknown opcode %d from %s:%d\n", opcode, inet_ntoa(clientAddr.sin_addr), ntohs(clientAddr.sin_port));
                sendError(serverSock, clientAddr, 4, "Illegal TFTP operation");
                break;
            }
        } else {
            continue;
        }
    }

    closesocket(serverSock);
    WSACleanup();
    return 0;
}