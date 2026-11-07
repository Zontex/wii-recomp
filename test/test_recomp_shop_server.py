"""The console's local server (recomp/tools/shop/server.py), asked as the console asks it."""
import hashlib
import http.client
import re
import sys
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "recomp/tools/shop"))

import server  # noqa: E402


class LocalServer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def get(self, host, path):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1])
        connection.request("GET", path, headers={"Host": host})
        response = connection.getresponse()
        body = response.read().decode()
        connection.close()
        return response, body

    def title(self, body):
        return re.search(r"<title>(.*?)</title>", body, re.S).group(1)

    def test_connection_test_names_nintendo(self):
        response, _ = self.get("conntest.nintendowifi.net", "/")
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("X-Organization"), "Nintendo")

    def test_agreement_index_carries_what_the_program_checks(self):
        # The agreement program takes the first 32 characters of the title as hex digits of a
        # SHA-1 over its country and the languages after them, which come as " xx" each.
        response, body = self.get("cfh.wapp.wii.com", "/eula/049/index.html")
        self.assertEqual(response.status, 200)
        title = self.title(body)
        digest, languages = title[:32], title[32:]
        self.assertRegex(languages, r"^( [a-z]{2}){1,4}$")
        signed = f"Nintendo: End User License Agreement: country code = 049{languages}"
        self.assertEqual(digest, hashlib.sha1(signed.encode()).hexdigest()[:32])
        _, other = self.get("cfh.wapp.wii.com", "/eula/064/index.html")
        self.assertNotEqual(self.title(other)[:32], digest)

    def test_agreement_page_has_the_title_the_viewer_wants(self):
        response, body = self.get("cfh.wapp.wii.com", "/eula/049/en.html")
        self.assertEqual(response.status, 200)
        self.assertEqual(self.title(body), "End User License Agreement")
        self.assertTrue(response.getheader("Last-Modified"))

    def test_anything_else_is_not_found(self):
        response, _ = self.get("example.com", "/")
        self.assertEqual(response.status, 404)


if __name__ == "__main__":
    unittest.main()
