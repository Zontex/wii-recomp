// /dev/net/ssl, IOS's secure connections. Every name the console's network reaches is
// its own local server (net_routes.c), so a secure connection carries the bytes as
// they are over the socket: certificates are accepted, and the handshake is done once
// the socket is connected. The program sees what it would from a real server. Request
// numbers, buffers and answers follow Dolphin's IOS/Network/SSL.cpp and Socket.cpp
// (GPL-2.0+): each answer goes in the first input buffer, the context's number comes in
// the first output buffer, and the IPC reply itself is 0.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "cpu.h"
#include "ios.h"
#include "ios_vectors.h"
#include "net.h"

enum {
  SSL_NEW = 1, SSL_CONNECT = 2, SSL_HANDSHAKE = 3, SSL_READ = 4, SSL_WRITE = 5, SSL_SHUTDOWN = 6,
  SSL_SET_CLIENT_CERT = 7, SSL_SET_CLIENT_CERT_DEFAULT = 8, SSL_REMOVE_CLIENT_CERT = 9, SSL_SET_ROOT_CA = 10,
  SSL_SET_ROOT_CA_DEFAULT = 11, SSL_HANDSHAKE_EX = 12, SSL_SET_BUILTIN_ROOT_CA = 13, SSL_SET_BUILTIN_CLIENT_CERT = 14,
  SSL_DISABLE_VERIFY = 15,
  SSL_OK = 0, SSL_FAILED = -1, SSL_READ_AGAIN = -2, SSL_SYSCALL = -5, SSL_ZERO = -6, SSL_BAD_ID = -8,
  MAX_CONTEXTS = 8,
};

static struct { int active; uint32_t socket; } contexts[MAX_CONTEXTS];

// The context named by the first output buffer, 1-based, or -1.
static int context_of(const IosVectors *v) {
  uint32_t at = ios_out(v, 0);
  int id = at ? (int)rd32(at) - 1 : -1;
  return id >= 0 && id < MAX_CONTEXTS && contexts[id].active ? id : -1;
}

static int answer(const IosVectors *v, int result) {
  uint32_t at = ios_in(v, 0);
  if (at) wr32(at, (uint32_t)result);
  return 0;
}

// Handshake, read or write now; IOS_NO_REPLY when it must wait for the socket.
static int transfer(uint32_t request, int kind) {
  IosVectors v = ios_vectors(request);
  int id = context_of(&v);
  if (id < 0) return answer(&v, SSL_BAD_ID);
  int sock = net_socket_host(contexts[id].socket);
  if (sock < 0) return answer(&v, SSL_SYSCALL);
  if (kind == SSL_HANDSHAKE || kind == SSL_HANDSHAKE_EX) {
    return net_socket_connected(contexts[id].socket) ? answer(&v, SSL_OK) : IOS_NO_REPLY;
  }
  ssize_t done;
  if (kind == SSL_READ) {
    uint32_t to = ios_in(&v, 1), bytes = ios_in_bytes(&v, 1);
    done = recv(sock, to ? host(to) : 0, bytes, 0);
  } else {
    uint32_t from = ios_out(&v, 1), bytes = ios_out_bytes(&v, 1);
    done = send(sock, from ? host(from) : 0, bytes, 0);
  }
  if (done < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return IOS_NO_REPLY;
  if (done < 0) return answer(&v, SSL_SYSCALL);
  return answer(&v, done == 0 ? SSL_ZERO : (int)done);
}

int net_ssl_retry(uint32_t request, int kind) { return transfer(request, kind - NET_SSL_KINDS); }

int net_ssl_request(uint32_t command, uint32_t request) {
  if (command != IOS_IOCTLV) return IOS_UNHANDLED;
  IosVectors v = ios_vectors(request);
  uint32_t code = rd32(request + 12);
  switch (code) {
    case SSL_NEW: {
      uint32_t name = ios_out(&v, 1);
      for (int id = 0; id < MAX_CONTEXTS; id++) {
        if (contexts[id].active) continue;
        contexts[id].active = 1;
        contexts[id].socket = 0xffffffffu;
        char text[256] = {0};
        for (uint32_t i = 0; name && i < 255 && i < ios_out_bytes(&v, 1) && (text[i] = (char)rd8(name + i)); i++) {}
        fprintf(stderr, "net: a secure connection for %s\n", text);
        return answer(&v, id + 1);
      }
      return answer(&v, SSL_FAILED);
    }
    case SSL_CONNECT: {
      int id = context_of(&v);
      if (id < 0) return answer(&v, SSL_BAD_ID);
      contexts[id].socket = rd32(ios_out(&v, 1));
      return answer(&v, SSL_OK);
    }
    case SSL_HANDSHAKE: case SSL_HANDSHAKE_EX: case SSL_READ: case SSL_WRITE: {
      int result = transfer(request, (int)code);
      if (result == IOS_NO_REPLY) {
        int id = context_of(&v);
        net_hold(request, NET_SSL_KINDS + (int)code, id >= 0 ? contexts[id].socket : 0, 0);
      }
      return result;
    }
    case SSL_SHUTDOWN: {
      int id = context_of(&v);
      if (id < 0) return answer(&v, SSL_BAD_ID);
      contexts[id].active = 0;
      return answer(&v, SSL_OK);
    }
    // Certificates: the console's own server is trusted as it is.
    case SSL_SET_CLIENT_CERT: case SSL_SET_CLIENT_CERT_DEFAULT: case SSL_REMOVE_CLIENT_CERT: case SSL_SET_ROOT_CA:
    case SSL_SET_ROOT_CA_DEFAULT: case SSL_SET_BUILTIN_ROOT_CA: case SSL_SET_BUILTIN_CLIENT_CERT: case SSL_DISABLE_VERIFY:
      return answer(&v, context_of(&v) >= 0 ? SSL_OK : SSL_BAD_ID);
    default: return IOS_UNHANDLED;
  }
}
