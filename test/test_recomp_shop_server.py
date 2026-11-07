"""The console's local server (recomp/tools/shop/server.py), asked as the console asks it."""
import hashlib
import http.client
import json
import re
import sys
import tempfile
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "recomp/tools/shop"))

import pages  # noqa: E402
import server  # noqa: E402

# A made-up catalogue, as assets.py writes it.
TITLES = [
    {"id": "0001000146414141", "title1": "Made-up Quest<sup>TM</sup>", "title2": "", "console": "NES", "publisher": "Nobody",
     "genre": "Action", "date": "1/2/2007", "points": "0", "players": "For 1 player", "rating": "E", "ratingdetails": "",
     "thumbnail": "FFFD0001", "controllers": {"pictures": [], "lines": []}},
    {"id": "0001000157414141", "title1": "Made-up Ware", "title2": "", "console": "WiiWare", "publisher": "Nobody",
     "genre": "Puzzle", "date": "3/4/2009", "points": "800", "players": "", "rating": "", "ratingdetails": "",
     "thumbnail": "FFFD0001", "controllers": {"pictures": [], "lines": []}},
]


class LocalServer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.assets = tempfile.TemporaryDirectory()
        with open(f"{cls.assets.name}/catalogue.json", "w") as out:
            json.dump({"region": "US", "titles": TITLES}, out)
        cls.server = server.Server(("127.0.0.1", 0), server.Handler)
        cls.server.assets = cls.assets.name
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.assets.cleanup()

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

    def test_shop_starts_at_its_welcome_page(self):
        response, _ = self.get("oss-auth.shop.wii.com", "/")
        self.assertEqual(response.status, 302)
        self.assertEqual(response.getheader("Location"), "/oss/serv/W_01.jsp")
        for path in ("W_01.jsp", "W_03.jsp", "B_01.jsp?kind=wiiware", "B_02.jsp", "B_03.jsp?kind=vc", "W_04.jsp?what=owned"):
            response, body = self.get("oss-auth.shop.wii.com", "/oss/serv/" + path)
            self.assertEqual(response.status, 200, path)
            self.assertIn("new wiiShop()", body, path)  # Every page stops the channel's connecting timer.
            self.assertIn("shop.connecting", body, path)

    def test_lists_and_details_come_from_the_catalogue(self):
        _, body = self.get("oss-auth.shop.wii.com", "/oss/serv/B_04.jsp?kind=vc&order=popular")
        self.assertIn("Made-up Quest\u2122", body)  # The catalogue's markup becomes the mark itself.
        self.assertNotIn("Made-up Ware", body)
        self.assertIn("500 Wii Points", body)  # No price in the catalogue: the system's usual one.
        _, body = self.get("oss-auth.shop.wii.com", "/oss/serv/B_05.jsp?titleId=0001000157414141")
        self.assertIn("800 Wii Points", body)
        self.assertIn("B_06.jsp?titleId=0001000157414141", body)  # Download leads to the refusal.
        _, body = self.get("oss-auth.shop.wii.com", "/oss/serv/B_06.jsp?titleId=0001000157414141")
        self.assertIn("can't be downloaded", body)

    def test_unknown_pages_and_pictures(self):
        response, _ = self.get("oss-auth.shop.wii.com", "/oss/serv/X_99.jsp")
        self.assertEqual(response.status, 404)
        response, _ = self.get("oss-auth.shop.wii.com", "/oss/oss/common/../../../etc/passwd")
        self.assertEqual(response.status, 404)

    def test_prices(self):
        self.assertEqual(pages.points_text({"points": "0", "console": "Nintendo 64"}), "1000 Wii Points")
        self.assertEqual(pages.points_text({"points": "0", "console": "Wii Channels"}), "Free")
        self.assertEqual(pages.points_text({"points": "0", "console": "WiiWare"}), "")

    def test_anything_else_is_not_found(self):
        response, _ = self.get("example.com", "/")
        self.assertEqual(response.status, 404)


if __name__ == "__main__":
    unittest.main()
