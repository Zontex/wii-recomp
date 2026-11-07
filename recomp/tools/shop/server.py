#!/usr/bin/env python3
"""The local server that stands in for the online services the console answers itself.

    python3 recomp/tools/shop/server.py [--port 8360] [--certificates FOLDER]

The console starts it on its own when a program first looks one of the names up
(recomp/runtime/net_routes.c), and every connection to those names reaches it, on one
port. A connection that opens with TLS gets it, with the console's own certificates
(certificates.py), as the Shop's browser speaks it itself; the rest is plain HTTP,
including what IOS's secure connections carry, which the console passes through as it
is (net_ssl.c). It tells the services apart by the Host header:

    conntest.nintendowifi.net   Nintendo's connection test, which the Wii Menu runs
    *.shop.wii.com              the Wii Shop Channel: its pages and its SOAP services

Browsing is all it offers. Nothing here hands out titles, and the Shop's download
servers aren't routed at all, so a download ends with the Shop's own error message.
Every request is logged to stderr, which is the console's log.
"""
import argparse
import hashlib
import os
import re
import socket
import ssl
import sys
import warnings
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


# Shown where the Wii Network Services Agreement was. Plain HTML, as the agreement
# program's browser shows the original.
AGREEMENT = """<html><head><meta http-equiv="Content-Type" content="text/html; charset=utf-8">
<title>End User License Agreement</title></head>
<body style="font-family: sans-serif; font-size: 18px; color: #333; background: #fff">
<p><b>Wii Network Services, on this console</b></p>
<p>Nintendo's online services for the Wii have closed. This console answers a few of them
itself, from a small server running on this computer: the connection test and a look-only
Wii Shop Channel.</p>
<p>Nothing is sent anywhere else, nothing can be bought, and nothing can be downloaded.</p>
</body></html>
"""


AGREEMENT_DATE = "Fri, 06 Nov 2026 00:00:00 GMT"
AGREEMENT_LANGUAGES = " en"  # The agreement's languages, each as a space and its code.


def agreement_index(country):
    """The page the agreement program loads first, for a country number (049 is the US).

    Its title names the languages the agreement comes in, after the first 32 hex digits of
    a SHA-1 over the country and those languages, which the program checks before it asks
    for <language>.html. A page without it, such as a network's login page, is refused
    with error 221001."""
    signed = f"Nintendo: End User License Agreement: country code = {country:03d}{AGREEMENT_LANGUAGES}"
    title = hashlib.sha1(signed.encode()).hexdigest()[:32] + AGREEMENT_LANGUAGES
    return f"<html><head><title>{title}</title></head><body></body></html>"


def log(message):
    print(f"shop server: {message}", file=sys.stderr, flush=True)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, format, *args):  # Our own lines instead of the standard ones.
        pass

    def reply(self, status, body=b"", content_type="text/html; charset=utf-8", headers=()):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.close_connection = True
        for name, value in headers:
            self.send_header(name, value)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def handle_any(self):
        host = (self.headers.get("Host") or "").split(":")[0].lower()
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        log(f"{self.command} {host}{self.path} {self.headers.get('SOAPAction') or ''} {len(body)} bytes")
        log("  headers: " + "; ".join(f"{k}: {v}" for k, v in self.headers.items()))
        if body:
            log("  " + body[:600].decode("utf-8", "replace").replace("\n", " "))
        if host == "conntest.nintendowifi.net":
            # What Nintendo's test page answered: a short page, and this header.
            self.reply(200, b"<html><head><title>HTML Page</title></head><body bgcolor=\"#FFFFFF\">"
                            b"This is test.html page</body></html>", headers=[("X-Organization", "Nintendo")])
            return
        if host == "cfh.wapp.wii.com" and self.path.startswith("/eula/"):
            # The agreement program shows this page in its own window, with its own buttons
            # to accept. Nintendo's text is gone with its servers; this one is ours.
            # Last-Modified says which version of the agreement this is.
            index = re.fullmatch(r"/eula/(\d{3})/index\.html", self.path)
            page = agreement_index(int(index.group(1))) if index else AGREEMENT
            self.reply(200, page.encode("utf-8"), headers=[("Last-Modified", AGREEMENT_DATE)])
            return
        self.reply(404, b"")

    do_GET = do_POST = do_HEAD = handle_any


class Server(ThreadingHTTPServer):
    daemon_threads = True
    secure = None  # An ssl.SSLContext, with --certificates.

    def finish_request(self, request, client_address):
        if self.secure:
            request.settimeout(30)
            try:
                first = request.recv(1, socket.MSG_PEEK)
            except OSError:
                return
            # A TLS handshake record, or the older form of hello that old browsers send.
            if first and first[0] in (0x16, 0x80):
                try:
                    request = self.secure.wrap_socket(request, server_side=True)
                except (ssl.SSLError, OSError) as error:
                    log(f"a secure connection failed: {error}")
                    return
        self.RequestHandlerClass(request, client_address, self)


def secure_context(folder):
    """TLS as old Wii browsers speak it: TLS 1.0 at most, RSA with SHA-1, which today's
    defaults refuse, so they are allowed again here."""
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", DeprecationWarning)
        context.minimum_version = ssl.TLSVersion.TLSv1
    context.set_ciphers("AES128-SHA:AES256-SHA:DES-CBC3-SHA:RC4-SHA:@SECLEVEL=0")
    context.load_cert_chain(os.path.join(folder, "server.pem"), os.path.join(folder, "server.key"))
    return context


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--port", type=int, default=8360)
    parser.add_argument("--with-console", action="store_true", help="exit when the console that started it does")
    parser.add_argument("--certificates", help="the console's certificates (certificates.py), for secure pages")
    options = parser.parse_args()
    server = Server(("127.0.0.1", options.port), Handler)
    if options.certificates:
        server.secure = secure_context(options.certificates)
    log(f"listening on 127.0.0.1:{options.port}{', secure pages too' if server.secure else ''}")
    if options.with_console:
        threading.Thread(target=leave_with, args=(os.getppid(),), daemon=True).start()
    server.serve_forever()


def leave_with(parent):
    """Exits once the console that started this server has, however it ended."""
    while os.getppid() == parent:
        time.sleep(1)
    os._exit(0)


if __name__ == "__main__":
    main()
