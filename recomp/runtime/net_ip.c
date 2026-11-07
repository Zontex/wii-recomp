// /dev/net/ip/top, IOS's sockets, on the computer's own. Only a few names resolve: the
// services the console answers itself (net_routes.c), each given a made-up address that
// connects to the local server standing in for them. Every other name is "not found",
// so everything else stays offline, as with no internet. Request numbers, buffers and
// error codes follow Dolphin's IOS/Network/IP/Top.cpp and Socket.cpp (GPL-2.0+).
//
// A request that would wait, such as connecting, receiving or polling, is held and
// finished by net_update, which the interrupt loop calls, as IOS answers it later.
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cpu.h"
#include "gaps.h"
#include "ios.h"
#include "ios_vectors.h"
#include "net.h"

enum {
  SO_ACCEPT = 1, SO_BIND = 2, SO_CLOSE = 3, SO_CONNECT = 4, SO_FCNTL = 5, SO_GETPEERNAME = 6, SO_GETSOCKNAME = 7,
  SO_GETSOCKOPT = 8, SO_SETSOCKOPT = 9, SO_LISTEN = 10, SO_POLL = 11, SO_RECVFROM = 12, SO_SENDTO = 13, SO_SHUTDOWN = 14,
  SO_SOCKET = 15, SO_GETHOSTID = 16, SO_GETHOSTBYNAME = 17, SO_GETADDRINFO = 24, SO_INETATON = 21, SO_INETPTON = 22, SO_INETNTOP = 23,
  SO_STARTUP = 26, SO_CLEANUP = 27, SO_GETINTERFACEOPT = 28, SO_SETINTERFACEOPT = 29, SO_INITINTERFACE = 31,
  // Errors, negated as IOS answers them.
  E_AGAIN = 6, E_ALREADY = 7, E_BADF = 8, E_CONNREFUSED = 14, E_CONNRESET = 15, E_INPROGRESS = 26, E_INVAL = 28,
  E_ISCONN = 30, E_MFILE = 33, E_NETUNREACH = 40, E_NOTCONN = 56, E_NOPROTOOPT = 51, HOST_NOT_FOUND = -305,
  AF_WII_INET = 2, SOCK_WII_STREAM = 1, SOCK_WII_DGRAM = 2, F_GET = 3, F_SET = 4, F_NONBLOCK = 4,
  MSG_WII_NONBLOCK = 4, WII_POLL_IN = 0x0003, WII_POLL_OUT = 0x0008, WII_POLL_ERR = 0x0020, WII_POLL_HUP = 0x0040, WII_POLL_NVAL = 0x0080,
  HOSTENT_BYTES = 0x460, HOSTENT_NAME = 0x10, HOSTENT_ADDRESSES = 0x110, HOSTENT_LIST = 0x340,
  ADDRINFO_BYTES = 0x20, ADDRINFO_ADDRESSES = 0x460, SOCKADDR_BYTES = 8,
  MAX_SOCKETS = 64, MAX_HELD = 64, CHECK_TICKS = 60750,  // About a millisecond.
};
#define HOST_ADDRESS 0x0a000010u  // 10.0.0.16: the console's own address on its made-up network.

typedef struct {
  int host;             // The computer's socket, or -1 when free.
  int nonblocking, connecting, connected, stream;
  int dns;              // The far end of a pair, when this socket talks to the console's DNS server.
} Socket;

typedef struct {
  uint32_t request;
  int kind, socket;     // What waits: connect, receive, send or poll (SO_ codes), or an SSL one.
  uint64_t deadline;    // For a poll with a timeout; 0 waits for good.
} Held;

static Socket sockets[MAX_SOCKETS];
static Held held[MAX_HELD];
static int held_count, started;

static void start(void) {
  if (started) return;
  started = 1;
  for (int i = 0; i < MAX_SOCKETS; i++) sockets[i].host = -1;
}

static Socket *socket_at(uint32_t fd) {
  start();
  return fd < MAX_SOCKETS && sockets[fd].host >= 0 ? &sockets[fd] : 0;
}

int net_socket_host(uint32_t fd) {
  Socket *s = socket_at(fd);
  return s ? s->host : -1;
}

int net_socket_connected(uint32_t fd) {
  Socket *s = socket_at(fd);
  return s && s->connected;
}

// The computer's error as IOS's, negated.
static int wii_error(int host_error) {
  switch (host_error) {
    case EAGAIN: return -E_AGAIN;
    case EINPROGRESS: return -E_INPROGRESS;
    case EALREADY: return -E_ALREADY;
    case EISCONN: return -E_ISCONN;
    case ECONNREFUSED: return -E_CONNREFUSED;
    case ECONNRESET: case EPIPE: return -E_CONNRESET;
    case ENOTCONN: return -E_NOTCONN;
    case EBADF: return -E_BADF;
    case ENETUNREACH: case EHOSTUNREACH: return -E_NETUNREACH;
    default: return -E_INVAL;
  }
}

void net_hold(uint32_t request, int kind, uint32_t fd, uint64_t deadline) {
  if (held_count == MAX_HELD) {
    fprintf(stderr, "stop: more than %d network requests waiting at once\n", MAX_HELD);
    exit(9);
  }
  held[held_count++] = (Held){request, kind, (int)fd, deadline};
}

static int new_socket(uint32_t in) {
  uint32_t family = rd32(in), type = rd32(in + 4);
  if (family != AF_WII_INET) return -E_INVAL;
  if (type != SOCK_WII_STREAM && type != SOCK_WII_DGRAM) return -E_INVAL;
  start();
  for (int fd = 0; fd < MAX_SOCKETS; fd++) {
    if (sockets[fd].host >= 0) continue;
    int host = socket(AF_INET, type == SOCK_WII_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (host < 0) return wii_error(errno);
    fcntl(host, F_SETFL, fcntl(host, F_GETFL) | O_NONBLOCK);  // The waiting is done here, in net_update.
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(host, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sockets[fd] = (Socket){host, 0, 0, 0, type == SOCK_WII_STREAM, -1};
    return fd;
  }
  return -E_MFILE;
}

static int close_socket(uint32_t fd) {
  Socket *s = socket_at(fd);
  if (!s) return -E_BADF;
  close(s->host);
  if (s->dns >= 0) close(s->dns);
  s->host = s->dns = -1;
  // Anything still waiting on it is answered, as the socket is gone.
  for (int i = 0; i < held_count; i++) {
    if (held[i].socket == (int)fd && held[i].kind != SO_POLL) {
      ios_complete(held[i].request, -E_BADF);
      held[i--] = held[--held_count];
    }
  }
  return 0;
}

// A Wii address, as len, family, port and address, to where the computer connects.
static int host_address(uint32_t at, struct sockaddr_in *out) {
  memset(out, 0, sizeof *out);
  out->sin_family = AF_INET;
  uint32_t address = rd32(at + 4);
  uint16_t port = (uint16_t)rd16(at + 2);
  if (!net_route_address(address, port, out)) return 0;
  return 1;
}

// The console's DNS server, at its own address (the DNS server the interface options
// give). Programs with their own resolver, as the web library has, ask it directly. A
// socket that talks to it becomes one end of a local pair; net_update answers each
// question at the other end: a name served here gets the server's made-up address, any
// other "no such name". Over a stream, each message has a two-byte length first.
enum { DNS_PORT = 53, DNS_MAX = 512 };

static void dns_answer(Socket *s) {
  uint8_t question[DNS_MAX + 2], reply[DNS_MAX + 2];
  ssize_t got;
  while ((got = recv(s->dns, question, sizeof question, 0)) > 0) {
    uint8_t *q = question + (s->stream ? 2 : 0);
    size_t length = (size_t)got - (s->stream ? 2 : 0);
    if (length < 12) continue;
    // The name asked for, from its labels.
    char name[256] = {0};
    size_t at = 12, used = 0;
    while (at < length && q[at] && used < 250) {
      size_t label = q[at++];
      if (at + label > length) break;
      if (used) name[used++] = '.';
      memcpy(name + used, q + at, label);
      used += label;
      at += label;
    }
    size_t question_end = at + 1 + 4;  // The name's end, then type and class.
    if (question_end > length) continue;
    uint16_t type = (uint16_t)(q[at + 1] << 8 | q[at + 2]);
    uint32_t address = net_route_name(name);
    fprintf(stderr, "net: DNS asks for %s -> %s\n", name, address ? "the console's own server" : "no such name");
    uint8_t *r = reply + (s->stream ? 2 : 0);
    memcpy(r, q, question_end);
    r[2] = 0x81, r[3] = address ? 0x80 : 0x83;  // A reply, recursion available; no error, or no such name.
    r[4] = 0, r[5] = 1, r[6] = 0, r[7] = address && type == 1 ? 1 : 0, r[8] = r[9] = r[10] = r[11] = 0;
    size_t out = question_end;
    if (address && type == 1) {
      static const uint8_t record[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 1, 0x2c, 0, 4};  // The name, A, IN, 300 s, 4 bytes.
      memcpy(r + out, record, sizeof record);
      out += sizeof record;
      r[out++] = (uint8_t)(address >> 24), r[out++] = (uint8_t)(address >> 16), r[out++] = (uint8_t)(address >> 8), r[out++] = (uint8_t)address;
    }
    if (s->stream) reply[0] = (uint8_t)(out >> 8), reply[1] = (uint8_t)out;
    send(s->dns, reply, out + (s->stream ? 2 : 0), 0);
  }
}

// Makes a socket talk to the console's DNS server. 1 if it now does.
static int become_dns(Socket *s) {
  if (s->dns >= 0) return 1;
  int pair[2];
  if (socketpair(AF_UNIX, s->stream ? SOCK_STREAM : SOCK_DGRAM, 0, pair)) return 0;
  for (int i = 0; i < 2; i++) fcntl(pair[i], F_SETFL, fcntl(pair[i], F_GETFL) | O_NONBLOCK);
  close(s->host);
  s->host = pair[0];
  s->dns = pair[1];
  s->connected = 1;
  return 1;
}

static int is_dns_server(uint32_t at) { return rd32(at + 4) == HOST_ADDRESS && rd16(at + 2) == DNS_PORT; }

static int connect_now(Socket *s) {
  int error = 0;
  socklen_t length = sizeof error;
  if (getsockopt(s->host, SOL_SOCKET, SO_ERROR, &error, &length) || error) return wii_error(error ? error : errno);
  s->connecting = 0;
  s->connected = 1;
  return 0;
}

static int connect_socket(uint32_t request, uint32_t in) {
  uint32_t fd = rd32(in);
  Socket *s = socket_at(fd);
  if (!s) return -E_BADF;
  if (s->connected) return -E_ISCONN;
  if (is_dns_server(in + 8)) return become_dns(s) ? 0 : -E_NETUNREACH;
  struct sockaddr_in address;
  if (!host_address(in + 8, &address)) return -E_NETUNREACH;  // Nothing else is reachable.
  if (connect(s->host, (struct sockaddr *)&address, sizeof address) == 0) {
    s->connected = 1;
    return 0;
  }
  if (errno != EINPROGRESS) return wii_error(errno);
  s->connecting = 1;
  if (s->nonblocking) return -E_INPROGRESS;
  net_hold(request, SO_CONNECT, fd, 0);
  return IOS_NO_REPLY;
}

// Writes a Wii address: length, family, port and address.
static void write_wii_address(uint32_t at, uint32_t address, uint16_t port) {
  wr8(at, SOCKADDR_BYTES);
  wr8(at + 1, AF_WII_INET);
  wr16(at + 2, port);
  wr32(at + 4, address);
}

static int receive(Socket *s, uint32_t request) {
  IosVectors v = ios_vectors(request);
  uint32_t out = ios_out(&v, 0), bytes = ios_out_bytes(&v, 0), flags = rd32(ios_in(&v, 0) + 4);
  ssize_t got = recv(s->host, out ? host(out) : 0, bytes, (flags & 2) ? MSG_PEEK : 0);
  if (got < 0) return wii_error(errno);
  if (ios_out(&v, 1) && ios_out_bytes(&v, 1) >= SOCKADDR_BYTES) write_wii_address(ios_out(&v, 1), 0, 0);
  return (int)got;
}

static int send_data(Socket *s, uint32_t request) {
  IosVectors v = ios_vectors(request);
  uint32_t data = ios_in(&v, 0), bytes = ios_in_bytes(&v, 0), options = ios_in(&v, 1);
  // A datagram sent to the DNS server's address without connecting first.
  if (options && rd32(options + 8) && is_dns_server(options + 12) && !become_dns(s)) return -E_NETUNREACH;
  ssize_t sent = send(s->host, data ? host(data) : 0, bytes, 0);
  if (s->dns >= 0) dns_answer(s);  // Answered at once, so the reply is there to receive.
  return sent < 0 ? wii_error(errno) : (int)sent;
}

// A receive or send now, or held until it can be done, unless it mustn't wait.
static int transfer(uint32_t request, int kind) {
  IosVectors v = ios_vectors(request);
  uint32_t fd = rd32(kind == SO_RECVFROM ? ios_in(&v, 0) : ios_in(&v, 1));
  uint32_t flags = rd32((kind == SO_RECVFROM ? ios_in(&v, 0) : ios_in(&v, 1)) + 4);
  Socket *s = socket_at(fd);
  if (!s) return -E_BADF;
  int result = kind == SO_RECVFROM ? receive(s, request) : send_data(s, request);
  if (result != -E_AGAIN || s->nonblocking || (flags & MSG_WII_NONBLOCK)) return result;
  net_hold(request, kind, fd, 0);
  return IOS_NO_REPLY;
}

static int poll_events(int host, int wanted) {
  struct pollfd p = {host, (short)(((wanted & WII_POLL_IN) ? POLLIN : 0) | ((wanted & WII_POLL_OUT) ? POLLOUT : 0)), 0};
  if (poll(&p, 1, 0) < 0) return WII_POLL_ERR;
  // A peer that closed shows as readable while its last data waits, as Linux reports it;
  // macOS adds a hang-up at once, which programs take as a failure before reading.
  int hung_up = (p.revents & POLLHUP) && !(p.revents & POLLIN);
  return ((p.revents & POLLIN) ? (wanted & WII_POLL_IN) : 0) | ((p.revents & POLLOUT) ? WII_POLL_OUT : 0)
       | ((p.revents & POLLERR) ? WII_POLL_ERR : 0) | (hung_up ? WII_POLL_HUP : 0);
}

// Fills in each entry's events. Returns how many have any.
static int poll_now(uint32_t request) {
  uint32_t out = rd32(request + 24) | 0x80000000u, entries = rd32(request + 28) / 12;
  int ready = 0;
  for (uint32_t i = 0; i < entries; i++) {
    uint32_t at = out + i * 12;
    Socket *s = socket_at(rd32(at));
    int events = s ? poll_events(s->host, (int)rd32(at + 4)) : WII_POLL_NVAL;
    wr32(at + 8, (uint32_t)events);
    static int told;
    if (ios_trace_all() && told < 200) told++, fprintf(stderr, "net: poll %u wants %x, gets %x\n", rd32(at), rd32(at + 4), events);
    ready += events != 0;
  }
  return ready;
}

static int poll_sockets(uint32_t request) {
  uint32_t in = rd32(request + 16) | 0x80000000u, entries = rd32(request + 28) / 12;
  if (!entries || entries > MAX_SOCKETS) return -E_INVAL;
  int64_t timeout = (int64_t)((uint64_t)rd32(in) << 32 | rd32(in + 4));
  // Answered from net_update, a moment later even when something is ready already, as
  // IOS takes a while to answer and Dolphin answers from its periodic update too. The
  // thread asking waits meanwhile, as on a Wii, so a thread that polls in a loop, as the
  // Shop's browser does, leaves time for the others.
  uint64_t now = time_base();
  net_hold(request, SO_POLL, 0, timeout > 0 ? now + (uint64_t)timeout * 60750 : timeout == 0 ? now : 0);
  return IOS_NO_REPLY;
}

// The name's made-up address, as gethostbyname's answer: the name, then the address and
// a list pointing at it, as Dolphin lays it out.
static int host_by_name(uint32_t request) {
  uint32_t in = rd32(request + 16) | 0x80000000u, out = rd32(request + 24) | 0x80000000u;
  if (rd32(request + 28) != HOSTENT_BYTES) return -1;
  char name[256] = {0};
  for (int i = 0; i < 255 && (name[i] = (char)rd8(in + (uint32_t)i)); i++) {}
  uint32_t address = net_route_name(name);
  fprintf(stderr, "net: looking up %s -> %s\n", name, address ? "the console's own server" : "not found");
  if (!address) return -1;
  memset(host(out), 0, HOSTENT_BYTES);
  wr32(out, out + HOSTENT_NAME);
  wr16(out + 8, AF_WII_INET);
  wr16(out + 10, 4);
  wr32(out + 12, out + HOSTENT_LIST);
  memcpy(host(out + HOSTENT_NAME), name, strlen(name) + 1);
  wr32(out + HOSTENT_ADDRESSES, address);
  wr32(out + HOSTENT_LIST, out + HOSTENT_ADDRESSES);
  wr32(out + HOSTENT_LIST + 4, 0);
  wr32(out + 4, out + HOSTENT_LIST + 4);  // No aliases.
  return 0;
}

static int address_info(uint32_t request) {
  IosVectors v = ios_vectors(request);
  uint32_t name_at = ios_in(&v, 0), out = ios_out(&v, 0);
  if (!name_at || !out) return HOST_NOT_FOUND;
  char name[256] = {0};
  for (int i = 0; i < 255 && i < (int)ios_in_bytes(&v, 0) && (name[i] = (char)rd8(name_at + (uint32_t)i)); i++) {}
  uint32_t address = net_route_name(name);
  fprintf(stderr, "net: looking up %s -> %s\n", name, address ? "the console's own server" : "not found");
  if (!address) return HOST_NOT_FOUND;
  memset(host(out), 0, ADDRINFO_BYTES);
  wr32(out + 4, AF_WII_INET);
  wr32(out + 8, SOCK_WII_STREAM);
  wr32(out + 16, SOCKADDR_BYTES);
  wr32(out + 24, out + ADDRINFO_ADDRESSES);
  write_wii_address(out + ADDRINFO_ADDRESSES, address, 0);
  return 0;
}

// A dotted address as text, as a number; 0 if it isn't one.
static int parse_address(uint32_t at, uint32_t *address) {
  char text[64] = {0};
  for (int i = 0; i < 63 && (text[i] = (char)rd8(at + (uint32_t)i)); i++) {}
  unsigned a, b, c, d;
  char extra;
  if (sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
    *address = net_route_name(text);  // inet_aton also takes a name, as Dolphin's looks it up.
    return *address != 0;
  }
  *address = a << 24 | b << 16 | c << 8 | d;
  return 1;
}

// Interface settings the network library asks for, with Dolphin's answers for a wired
// connection on the made-up network.
static int interface_option(uint32_t request) {
  IosVectors v = ios_vectors(request);
  uint32_t in = ios_in(&v, 0), out = ios_out(&v, 0), out_size = ios_out(&v, 1);
  if (!in || rd32(in) != 0xfffe) return -E_INVAL;
  uint32_t option = rd32(in + 4);
  if (ios_trace_all()) fprintf(stderr, "net: interface option %x asked\n", option);
  switch (option) {
    case 0xb003: if (out) wr32(out, HOST_ADDRESS), wr32(out + 4, HOST_ADDRESS); break;  // DNS servers: itself.
    case 0x1003: if (out) wr32(out, 0); break;
    case 0x1004: { static const uint8_t mac[6] = {0x02, 0x00, 0x5e, 0x10, 0x00, 0x01}; if (out) memcpy(host(out), mac, 6); break; }
    case 0x1005: if (out) wr32(out, 1); break;  // Link up.
    case 0x3001: if (out) wr32(out, 0x10); break;
    case 0x4002: if (out) wr32(out, 1); break;
    case 0x4003:
      if (out) wr32(out, HOST_ADDRESS), wr32(out + 4, 0xffffff00u), wr32(out + 8, HOST_ADDRESS | 0xff);
      if (out_size) wr32(out_size, 0xc);
      break;
    case 0x4005: if (out) wr32(out, 0x20); break;
    case 0x4006: if (out_size) wr32(out_size, 0); break;  // No routes to list.
    case 0x6003: case 0x600a: case 0x600c: if (out) wr32(out, 0x80); break;
    case 0xb002: if (out) wr32(out, 2); break;
    default:
      gap_hit("ios.dev_net_ip_top", 0x1c0000 | option);
      return -E_INVAL;
  }
  return 0;
}

// A socket's option. As in Dolphin, the socket, level and option come in the output
// buffer, which gets the value's length at 0xc and the value at 0x10. Each one is the
// host socket's, by Dolphin's table of names; the pending error is how a program learns
// a connection went through.
enum { WII_SOL_SOCKET = 0xffff, WII_SO_ERROR = 0x1009 };

static const struct { uint32_t wii; int host; } option_names[] = {
  {0x0004, SO_REUSEADDR}, {0x0080, SO_LINGER}, {0x0100, SO_OOBINLINE}, {0x1001, SO_SNDBUF},
  {0x1002, SO_RCVBUF}, {0x1003, SO_SNDLOWAT}, {0x1004, SO_RCVLOWAT}, {0x1008, SO_TYPE}, {0x1009, SO_ERROR},
};

static int get_option(uint32_t out) {
  Socket *s = socket_at(rd32(out));
  if (!s) return -E_BADF;
  uint32_t level = rd32(out + 4), name = rd32(out + 8);
  int value = 0, found = 0;
  for (size_t i = 0; level == WII_SOL_SOCKET && i < sizeof option_names / sizeof *option_names; i++) {
    if (option_names[i].wii != name) continue;
    socklen_t length = sizeof value;
    found = !getsockopt(s->host, SOL_SOCKET, option_names[i].host, &value, &length);
    if (name == WII_SO_ERROR) value = value ? -wii_error(value) : 0;
  }
  if (!found) {
    fprintf(stderr, "net: option %x/%x asked, which isn't known\n", level, name);
    return -E_INVAL;
  }
  wr32(out + 0xc, 4);
  wr32(out + 0x10, (uint32_t)value);
  return 0;
}

int net_ip_request(uint32_t command, uint32_t request) {
  uint32_t code = rd32(request + 12), in = rd32(request + 16) | 0x80000000u;
  if (command == IOS_IOCTLV) {
    switch (code) {
      case SO_STARTUP: case SO_CLEANUP: case SO_SETINTERFACEOPT: return 0;
      case SO_GETINTERFACEOPT: return interface_option(request);
      case SO_RECVFROM: case SO_SENDTO: return transfer(request, (int)code);
      case SO_GETADDRINFO: return address_info(request);
      default: return IOS_UNHANDLED;
    }
  }
  if (command != IOS_IOCTL) return IOS_UNHANDLED;
  switch (code) {
    case SO_INITINTERFACE: return 0;
    case SO_GETHOSTID: return (int)HOST_ADDRESS;
    case SO_SOCKET: return new_socket(in);
    case SO_CLOSE: return close_socket(rd32(in));
    case SO_CONNECT: return connect_socket(request, in);
    case SO_SHUTDOWN: { Socket *s = socket_at(rd32(in)); if (!s) return -E_BADF; shutdown(s->host, SHUT_RDWR); return 0; }
    case SO_FCNTL: {
      Socket *s = socket_at(rd32(in));
      if (!s) return -E_BADF;
      uint32_t what = rd32(in + 4), argument = rd32(in + 8);
      if (what == F_GET) return s->nonblocking ? F_NONBLOCK : 0;
      if (what == F_SET) s->nonblocking = (argument & F_NONBLOCK) != 0;
      return 0;
    }
    case SO_SETSOCKOPT: return socket_at(rd32(in)) ? 0 : -E_BADF;  // Buffer sizes and timeouts change nothing here.
    case SO_GETSOCKOPT: return get_option(rd32(request + 24) | 0x80000000u);
    case SO_GETHOSTBYNAME: return host_by_name(request);
    // Addresses as text and back, with Dolphin's buffers: aton writes the number at the
    // output, pton four bytes in, and ntop reads it eight bytes into the input.
    case SO_INETATON: case SO_INETPTON: {
      uint32_t address, out = rd32(request + 24) | 0x80000000u;
      if (!parse_address(in, &address)) return 0;
      wr32(out + (code == SO_INETPTON ? 4 : 0), address);
      return 1;
    }
    case SO_INETNTOP: {
      uint32_t address = rd32(in + 8), out = rd32(request + 24) | 0x80000000u;
      char text[16];
      int length = snprintf(text, sizeof text, "%u.%u.%u.%u", address >> 24, (address >> 16) & 255, (address >> 8) & 255, address & 255);
      memcpy(host(out), text, (size_t)length);
      return 0;
    }
    case SO_POLL: return poll_sockets(request);
    case SO_GETSOCKNAME: case SO_GETPEERNAME: {
      uint32_t out = rd32(request + 24) | 0x80000000u;
      if (!socket_at(rd32(in))) return -E_BADF;
      write_wii_address(out, code == SO_GETSOCKNAME ? HOST_ADDRESS : 0, 0);
      return 0;
    }
    case SO_BIND: case SO_LISTEN: case SO_ACCEPT:
      gap_hit("ios.dev_net_ip_top", code);  // Nothing here offers a service.
      return -E_INVAL;
    default: return IOS_UNHANDLED;
  }
}

// Finishes held requests that can be: a connection made, data come or room to send, a
// poll with something ready or out of time. Checked about once a millisecond.
void net_update(uint64_t now) {
  static uint64_t next;
  if (!held_count || now < next) return;
  next = now + CHECK_TICKS;
  for (int i = 0; i < held_count; i++) {
    Held *h = &held[i];
    int result = IOS_NO_REPLY;
    if (h->kind == SO_POLL) {
      int ready = poll_now(h->request);
      if (ready || (h->deadline && now >= h->deadline)) result = ready;
    } else if (h->kind >= NET_SSL_KINDS) {
      result = net_ssl_retry(h->request, h->kind);
    } else {
      Socket *s = socket_at((uint32_t)h->socket);
      if (!s) result = -E_BADF;
      else if (h->kind == SO_CONNECT) {
        int events = poll_events(s->host, WII_POLL_OUT);
        if (events) result = connect_now(s);
      } else {
        result = h->kind == SO_RECVFROM ? receive(s, h->request) : send_data(s, h->request);
        if (result == -E_AGAIN) result = IOS_NO_REPLY;
      }
    }
    if (result == IOS_NO_REPLY) continue;
    ios_complete(h->request, result);
    held[i--] = held[--held_count];
  }
}

int net_waiting(void) { return held_count; }
