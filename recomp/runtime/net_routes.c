// Which names the console's network reaches, and where. A Wii is offline here, except
// for services the console answers itself with a small local server
// (recomp/tools/shop/server.py): Nintendo's connection test, the network services
// agreement and the Wii Shop Channel's pages and catalogue. Each of those names gets one
// made-up address, and connecting to it reaches the server, which tells them apart by
// the name in the request. The Shop's download servers aren't among them, so a download
// fails with the Shop's own message: browsing works, getting a title doesn't.
//
// WII_RECOMP_SHOP_SERVER=<host>:<port> uses a server already running; otherwise the
// console starts its own on 127.0.0.1:8360 the first time a name needs it.
// WII_RECOMP_SHOP_SERVER=off keeps every name unreachable. Secure pages are answered with
// the console's own certificates (WII_RECOMP_CERTIFICATES, made by
// recomp/tools/shop/certificates.py), which opera_trust.c makes the Shop's browser trust.
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "net.h"

#define SERVED_ADDRESS 0x0a000001u  // 10.0.0.1, the server's made-up address.
enum { DEFAULT_PORT = 8360 };

// Names the server answers: exact, or ending with a suffix that starts with a dot.
static const char *const served[] = {
  "conntest.nintendowifi.net", ".shop.wii.com", ".wapp.wii.com",
};
// Within those, the ones that hand out titles stay unreachable.
static const char *const never[] = {"ccs.shop.wii.com", "ccs.cdn.shop.wii.com", "nus.shop.wii.com"};

static char program_folder[PATH_MAX];
static pid_t server;

void net_remember_program(const char *program) {
  char full[PATH_MAX];
  if (!realpath(program, full)) return;
  char *slash = strrchr(full, '/');
  if (slash) *slash = 0;
  snprintf(program_folder, sizeof program_folder, "%s", full);
}

static int matches(const char *name, const char *pattern) {
  size_t length = strlen(name), wanted = strlen(pattern);
  if (pattern[0] != '.') return !strcasecmp(name, pattern);
  return length > wanted && !strcasecmp(name + length - wanted, pattern);
}

static int served_name(const char *name) {
  const char *setting = getenv("WII_RECOMP_SHOP_SERVER");
  if (setting && !strcmp(setting, "off")) return 0;
  for (size_t i = 0; i < sizeof never / sizeof *never; i++) if (matches(name, never[i])) return 0;
  for (size_t i = 0; i < sizeof served / sizeof *served; i++) if (matches(name, served[i])) return 1;
  return 0;
}

static void server_address(struct sockaddr_in *out) {
  const char *setting = getenv("WII_RECOMP_SHOP_SERVER");
  char host_part[64] = "127.0.0.1";
  int port = DEFAULT_PORT;
  if (setting && strcmp(setting, "off")) sscanf(setting, "%63[^:]:%d", host_part, &port);
  memset(out, 0, sizeof *out);
  out->sin_family = AF_INET;
  out->sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, host_part, &out->sin_addr);
}

static void stop_server(void) {
  if (server > 0) kill(server, SIGTERM);
}

// The console's own certificates: the folder they're in, made the first time.
static const char *certificates(void) {
  static int tried;
  static const char *ready;
  const char *folder = getenv("WII_RECOMP_CERTIFICATES");
  if (tried || !folder || !program_folder[0]) return ready;
  tried = 1;
  char script[PATH_MAX + 64];
  snprintf(script, sizeof script, "%s/../tools/shop/certificates.py", program_folder);
  pid_t maker = fork();
  if (maker == 0) {
    execlp("python3", "python3", script, folder, (char *)0);
    _exit(127);
  }
  int status = 0;
  if (maker > 0 && waitpid(maker, &status, 0) == maker && WIFEXITED(status) && WEXITSTATUS(status) == 0) ready = folder;
  else fprintf(stderr, "net: no certificates in %s (%s), so secure pages can't be answered\n", folder, script);
  return ready;
}

uint8_t *net_opera_entry(long *bytes) {
  const char *folder = certificates();
  if (!folder) return 0;
  char path[PATH_MAX + 32];
  snprintf(path, sizeof path, "%s/opera_entry.bin", folder);
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  fseek(file, 0, SEEK_END);
  *bytes = ftell(file);
  rewind(file);
  uint8_t *entry = *bytes > 0 ? malloc((size_t)*bytes) : 0;
  if (entry && fread(entry, 1, (size_t)*bytes, file) != (size_t)*bytes) {
    free(entry);
    entry = 0;
  }
  fclose(file);
  return entry;
}

// Starts the console's own server, unless one is set or already answering.
static void ensure_server(void) {
  static int tried;
  if (tried || getenv("WII_RECOMP_SHOP_SERVER") || !program_folder[0]) return;
  tried = 1;
  struct sockaddr_in address;
  server_address(&address);
  int probe = socket(AF_INET, SOCK_STREAM, 0);
  int running = probe >= 0 && connect(probe, (struct sockaddr *)&address, sizeof address) == 0;
  if (probe >= 0) close(probe);
  if (running) return;
  char script[PATH_MAX + 64], port[16];
  snprintf(script, sizeof script, "%s/../tools/shop/server.py", program_folder);
  snprintf(port, sizeof port, "%d", DEFAULT_PORT);
  if (access(script, R_OK)) {
    fprintf(stderr, "net: no local server at %s, so the Shop can't connect\n", script);
    return;
  }
  const char *secure = certificates();
  server = fork();
  if (server == 0) {
    if (secure) execlp("python3", "python3", script, "--port", port, "--with-console", "--certificates", secure, (char *)0);
    execlp("python3", "python3", script, "--port", port, "--with-console", (char *)0);
    _exit(127);
  }
  if (server > 0) {
    atexit(stop_server);
    fprintf(stderr, "net: started the local server, %s on port %d\n", script, DEFAULT_PORT);
    for (int i = 0; i < 50; i++) {  // Up to a second for it to listen.
      int test = socket(AF_INET, SOCK_STREAM, 0);
      int up = connect(test, (struct sockaddr *)&address, sizeof address) == 0;
      close(test);
      if (up) break;
      usleep(20000);
    }
  }
}

uint32_t net_route_name(const char *name) {
  if (!served_name(name)) return 0;
  ensure_server();
  return SERVED_ADDRESS;
}

int net_route_address(uint32_t address, uint16_t port, struct sockaddr_in *out) {
  (void)port;  // Plain and secure connections alike reach the one server.
  if (address != SERVED_ADDRESS) return 0;
  server_address(out);
  return 1;
}
