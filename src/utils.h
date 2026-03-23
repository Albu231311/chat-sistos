#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

// ── Message type identifiers
enum MessageType : uint8_t {
    // Client → Server
    TYPE_REGISTER       = 1,
    TYPE_MSG_GENERAL    = 2,
    TYPE_MSG_DM         = 3,
    TYPE_CHANGE_STATUS  = 4,
    TYPE_LIST_USERS     = 5,
    TYPE_GET_USER_INFO  = 6,
    TYPE_QUIT           = 7,
    // Server → Client
    TYPE_SERVER_RESP    = 10,
    TYPE_ALL_USERS      = 11,
    TYPE_FOR_DM         = 12,
    TYPE_BROADCAST      = 13,
    TYPE_USER_INFO_RESP = 14,
};

// ── Low-level I/O helpers 

// MSG_NOSIGNAL
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Send exactly n bytes (retries on partial sends)
static inline bool sendAll(int fd, const char* buf, size_t n) {
    size_t sent = 0;
    while (sent < n) {
        ssize_t s = send(fd, buf + sent, n - sent, MSG_NOSIGNAL);
        if (s <= 0) return false;
        sent += s;
    }
    return true;
}

// Receive exactly n bytes (retries on partial reads)
static inline bool recvAll(int fd, char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

// ── Public framing API

inline bool sendMsg(int fd, uint8_t type, const std::string& payload) {
    uint8_t header[5];
    header[0] = type;
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    memcpy(header + 1, &len, 4);
    if (!sendAll(fd, reinterpret_cast<char*>(header), 5)) return false;
    if (!payload.empty() && !sendAll(fd, payload.data(), payload.size())) return false;
    return true;
}


inline bool recvMsg(int fd, uint8_t& type, std::string& payload) {
    uint8_t header[5];
    if (!recvAll(fd, reinterpret_cast<char*>(header), 5)) return false;
    type = header[0];
    uint32_t len;
    memcpy(&len, header + 1, 4);
    len = ntohl(len);
    payload.resize(len);
    if (len > 0 && !recvAll(fd, &payload[0], len)) return false;
    return true;
}
