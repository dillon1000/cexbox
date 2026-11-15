// Network tracing: logs the socket calls the program makes, with addresses, so
// connections that the Xbox's network isolation blocks show up in the log.
#include "xb.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <stdio.h>
#include <string.h>

namespace xb {

namespace {

std::string address_text(const sockaddr* sa, int len) {
    if (!sa || len <= 0) return "-";
    char host[INET6_ADDRSTRLEN] = "?";
    unsigned port = 0;
    if (sa->sa_family == AF_INET && len >= static_cast<int>(sizeof(sockaddr_in))) {
        auto* in = reinterpret_cast<const sockaddr_in*>(sa);
        inet_ntop(AF_INET, &in->sin_addr, host, sizeof host);
        port = ntohs(in->sin_port);
    } else if (sa->sa_family == AF_INET6 && len >= static_cast<int>(sizeof(sockaddr_in6))) {
        auto* in6 = reinterpret_cast<const sockaddr_in6*>(sa);
        inet_ntop(AF_INET6, &in6->sin6_addr, host, sizeof host);
        port = ntohs(in6->sin6_port);
    } else {
        char buf[32];
        snprintf(buf, sizeof buf, "family %u", sa->sa_family);
        return buf;
    }
    char buf[96];
    snprintf(buf, sizeof buf, "%s:%u", host, port);
    return buf;
}

LPFN_CONNECTEX real_ConnectEx = nullptr;

int WSAAPI n_connect(SOCKET s, const sockaddr* name, int len) {
    int r = connect(s, name, len);
    int e = r ? WSAGetLastError() : 0;
    log("connect(%llu, %s) -> %d (error %d)", static_cast<unsigned long long>(s), address_text(name, len).c_str(), r, e);
    if (r) WSASetLastError(e);
    return r;
}

int WSAAPI n_bind(SOCKET s, const sockaddr* name, int len) {
    int r = bind(s, name, len);
    int e = r ? WSAGetLastError() : 0;
    log("bind(%llu, %s) -> %d (error %d)", static_cast<unsigned long long>(s), address_text(name, len).c_str(), r, e);
    if (r) WSASetLastError(e);
    return r;
}

int WSAAPI n_listen(SOCKET s, int backlog) {
    int r = listen(s, backlog);
    int e = r ? WSAGetLastError() : 0;
    log("listen(%llu) -> %d (error %d)", static_cast<unsigned long long>(s), r, e);
    if (r) WSASetLastError(e);
    return r;
}

SOCKET WSAAPI n_accept(SOCKET s, sockaddr* addr, int* len) {
    log("accept(%llu) waiting", static_cast<unsigned long long>(s));
    SOCKET r = accept(s, addr, len);
    int e = r == INVALID_SOCKET ? WSAGetLastError() : 0;
    log("accept(%llu) -> %llu (error %d)", static_cast<unsigned long long>(s), static_cast<unsigned long long>(r), e);
    if (r == INVALID_SOCKET) WSASetLastError(e);
    return r;
}

BOOL PASCAL n_ConnectEx(SOCKET s, const sockaddr* name, int len, PVOID buf, DWORD n, LPDWORD sent, LPOVERLAPPED ov) {
    BOOL r = real_ConnectEx(s, name, len, buf, n, sent, ov);
    int e = r ? 0 : WSAGetLastError();
    log("ConnectEx(%llu, %s) -> %d (error %d)", static_cast<unsigned long long>(s), address_text(name, len).c_str(), r, e);
    if (!r) WSASetLastError(e);
    return r;
}

int WSAAPI n_WSAIoctl(SOCKET s, DWORD code, LPVOID in, DWORD in_len, LPVOID out, DWORD out_len, LPDWORD returned,
                      LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE done) {
    int r = WSAIoctl(s, code, in, in_len, out, out_len, returned, ov, done);
    if (r == 0 && code == SIO_GET_EXTENSION_FUNCTION_POINTER && in && in_len >= sizeof(GUID) && out &&
        out_len >= sizeof(void*)) {
        static const GUID connect_ex = WSAID_CONNECTEX;
        if (memcmp(in, &connect_ex, sizeof(GUID)) == 0) {
            real_ConnectEx = *static_cast<LPFN_CONNECTEX*>(out);
            *static_cast<LPFN_CONNECTEX*>(out) = n_ConnectEx;
        }
    }
    return r;
}

INT WSAAPI n_GetAddrInfoW(PCWSTR node, PCWSTR service, const ADDRINFOW* hints, PADDRINFOW* result) {
    INT r = GetAddrInfoW(node, service, hints, result);
    log("GetAddrInfoW(%s, %s) -> %d", node ? narrow(node).c_str() : "-", service ? narrow(service).c_str() : "-", r);
    return r;
}

INT WSAAPI n_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* result) {
    INT r = getaddrinfo(node, service, hints, result);
    log("getaddrinfo(%s, %s) -> %d", node ? node : "-", service ? service : "-", r);
    return r;
}

struct Override {
    const char* name;
    void* fn;
};

#define NET(n) {#n, reinterpret_cast<void*>(&n_##n)}
const Override kNetOverrides[] = {
    NET(connect), NET(bind), NET(listen), NET(accept), NET(WSAIoctl), NET(GetAddrInfoW), NET(getaddrinfo),
};
#undef NET

}  // namespace

void* net_override(const char* name) {
    for (const Override& o : kNetOverrides) {
        if (strcmp(o.name, name) == 0) return o.fn;
    }
    return nullptr;
}

// Names of the Winsock 1 (wsock32.dll) functions programs import by ordinal.
const char* wsock32_name(uint16_t ordinal) {
    switch (ordinal) {
    case 1: return "accept";
    case 2: return "bind";
    case 3: return "closesocket";
    case 4: return "connect";
    case 5: return "getpeername";
    case 6: return "getsockname";
    case 7: return "getsockopt";
    case 8: return "htonl";
    case 9: return "htons";
    case 10: return "ioctlsocket";
    case 11: return "inet_addr";
    case 12: return "inet_ntoa";
    case 13: return "listen";
    case 14: return "ntohl";
    case 15: return "ntohs";
    case 16: return "recv";
    case 17: return "recvfrom";
    case 18: return "select";
    case 19: return "send";
    case 20: return "sendto";
    case 21: return "setsockopt";
    case 22: return "shutdown";
    case 23: return "socket";
    case 51: return "gethostbyaddr";
    case 52: return "gethostbyname";
    case 55: return "getservbyname";
    case 56: return "getservbyport";
    case 57: return "gethostname";
    case 111: return "WSAGetLastError";
    case 112: return "WSASetLastError";
    case 115: return "WSAStartup";
    case 116: return "WSACleanup";
    default: return nullptr;
    }
}

}  // namespace xb
