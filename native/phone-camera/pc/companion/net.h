#pragma once
// Sockets for Winsock and POSIX. The socket API itself (socket, send, recv, select, FD_SET...) is the same on
// both; this header brings in the right system headers and covers what differs. The only platform switch is
// the includes; the helpers are in platform_win.cpp / platform_posix.cpp.

#ifdef _WIN32
#ifndef FD_SETSIZE
#define FD_SETSIZE 128 // Winsock's default of 64 is less than WebServer's connection limit.
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include <stdint.h>

namespace mycam::net {

using Socket = uintptr_t;               // SOCKET on Windows, the fd on Linux.
constexpr Socket kInvalid = ~Socket(0); // INVALID_SOCKET; -1 on Linux.

bool Init();     // WSAStartup on Windows.
void Cleanup();
void Close(Socket s);
void Shutdown(Socket s);                // Both directions.
void SetNonBlocking(Socket s);
bool WouldBlock();                      // The last call failed only because it would have blocked.
int LastError();
// A listening TCP socket on every address, non-blocking. Windows: exclusive (nobody can take the port from
// under us); Linux: reusable at once after a restart. kInvalid on failure.
Socket ListenTcp(uint16_t port, int backlog);

} // namespace mycam::net
