// The console's network (net_ip.c, net_ssl.c, net_routes.c): sockets that reach only the
// local server standing in for the services the console answers itself.
#pragma once
#include <stdint.h>

struct sockaddr_in;

// /dev/net/ip/top and /dev/net/ssl.
int net_ip_request(uint32_t command, uint32_t request);
int net_ssl_request(uint32_t command, uint32_t request);
void net_update(uint64_t now);  // Finishes held requests that can be.
int net_waiting(void);          // How many are held.

// Held requests, shared by both devices. SSL's kinds start at NET_SSL_KINDS.
enum { NET_SSL_KINDS = 100 };
void net_hold(uint32_t request, int kind, uint32_t fd, uint64_t deadline);
int net_ssl_retry(uint32_t request, int kind);  // IOS_NO_REPLY while it must still wait.
int net_socket_host(uint32_t fd);               // The computer's socket for a Wii one, or -1.
int net_socket_connected(uint32_t fd);

// Names and addresses (net_routes.c).
void net_remember_program(const char *program);  // Where the console runs from, to find the server.
uint32_t net_route_name(const char *name);       // A made-up address for a name served here, or 0.
int net_route_address(uint32_t address, uint16_t port, struct sockaddr_in *out);  // Where it really goes.

// The console's own certificate authority (net_routes.c), made the first time it's
// needed, in WII_RECOMP_CERTIFICATES. Its entry for Opera's list, malloc'd, or 0.
uint8_t *net_opera_entry(long *bytes);
