#!/usr/bin/env python3
"""The console's own certificate authority, for the secure pages its local server answers.

    python3 recomp/tools/shop/certificates.py <folder>

The Wii Shop Channel's browser (Opera) speaks TLS itself and trusts only the authorities
in its own list. Each console makes its own authority once, here, and the runtime adds it
to that list as the browser reads it (recomp/runtime/opera_trust.c), so the browser trusts
pages from the local server, and nothing else changes. The folder gets:

    authority.der   the authority's certificate, which goes in the browser's list
    authority.pem   the same, for anything else that wants it
    authority.key   its private key, which only signs the server's certificate
    server.pem      the server's certificate, for the names the console answers
    server.key      its private key
    opera_entry.bin the authority as an entry of Opera's list (opcacrt6.dat), ready to add

Keys stay with the console and are never shared. Old browsers know only RSA and SHA-1,
so the certificates use those. Makes nothing if the folder already has them. Needs the
openssl command.
"""
import os
import subprocess
import sys
import tempfile

NAMES = ["*.shop.wii.com", "shop.wii.com", "*.wapp.wii.com", "conntest.nintendowifi.net"]
FILES = ["authority.der", "authority.pem", "authority.key", "server.pem", "server.key", "opera_entry.bin"]
DAYS = 7300  # About twenty years.


def openssl(*arguments):
    subprocess.run(["openssl", *arguments], check=True, capture_output=True)


def der_items(data, at, end):
    """(start of the item, start of its contents, end) of each DER item from at to end."""
    while at < end:
        start, length = at, data[at + 1]
        at += 2
        if length & 0x80:
            count = length & 0x7f
            length = int.from_bytes(data[at:at + count], "big")
            at += count
        yield start, at, at + length
        at += length


def subject_of(certificate):
    """The subject of a DER certificate, as DER: the fifth item of the signed part after
    the version, which is the first item when present."""
    _, start, end = next(der_items(certificate, 0, len(certificate)))
    _, start, end = next(der_items(certificate, start, end))
    items = list(der_items(certificate, start, end))
    if certificate[items[0][0]] == 0xa0:  # The version.
        items = items[1:]
    start, _, end = items[4]
    return certificate[start:end]


def opera_entry(certificate, name):
    """An entry of Opera's certificate list for an authority, as the Wii Shop Channel's
    browser reads it: Opera's tagged records (a one-byte tag, then a four-byte length),
    with the layout WSC-Patcher worked out (MIT, see CREDITS.md): a type of 1, the name,
    the subject, then the certificate."""
    def tag(number, contents):
        return bytes([number]) + len(contents).to_bytes(4, "big") + contents
    inner = tag(0x20, (1).to_bytes(4, "big")) + tag(0x21, name.encode()) + tag(0x22, subject_of(certificate)) + tag(0x23, certificate)
    return tag(0x02, inner)


def make(folder):
    """Makes the authority and the server's certificate in folder, unless they're there."""
    if all(os.path.isfile(os.path.join(folder, name)) for name in FILES):
        return False
    os.makedirs(folder, exist_ok=True)
    path = lambda name: os.path.join(folder, name)  # noqa: E731
    openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-sha1", "-days", str(DAYS),
            "-subj", "/O=Wii Recomp/CN=Wii Recomp console", "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            "-keyout", path("authority.key"), "-out", path("authority.pem"))
    openssl("x509", "-in", path("authority.pem"), "-outform", "DER", "-out", path("authority.der"))
    with open(path("authority.der"), "rb") as source, open(path("opera_entry.bin"), "wb") as out:
        out.write(opera_entry(source.read(), "Wii Recomp console"))
    with tempfile.TemporaryDirectory() as work:
        request, extensions = os.path.join(work, "server.csr"), os.path.join(work, "server.ext")
        with open(extensions, "w") as out:
            out.write("basicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\n"
                      "extendedKeyUsage=serverAuth\nsubjectAltName=" + ",".join("DNS:" + name for name in NAMES) + "\n")
        openssl("req", "-newkey", "rsa:2048", "-nodes", "-sha1", "-subj", f"/O=Wii Recomp/CN={NAMES[0]}",
                "-keyout", path("server.key"), "-out", request)
        openssl("x509", "-req", "-in", request, "-CA", path("authority.pem"), "-CAkey", path("authority.key"),
                "-CAcreateserial", "-CAserial", os.path.join(work, "serial"), "-sha1", "-days", str(DAYS),
                "-extfile", extensions, "-out", path("server.pem"))
    for name in ("authority.key", "server.key"):
        os.chmod(path(name), 0o600)
    return True


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__.split("\n\n")[1])
    try:
        made = make(sys.argv[1])
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"certificates: couldn't make them with openssl: {error}")
    print(f"certificates: {'made' if made else 'already'} in {sys.argv[1]}", file=sys.stderr)


if __name__ == "__main__":
    main()
