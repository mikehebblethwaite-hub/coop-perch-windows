import unittest
from http.server import ThreadingHTTPServer
import threading
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from unittest.mock import patch

from server import (
    BAN_SECONDS,
    Handler,
    PROBE_LIMIT,
    RequestWatchdog,
    classify_probe,
)


class ProbeClassificationTests(unittest.TestCase):
    def test_coop_probe_categories_are_recognized(self):
        examples = {
            "/.env": "environment-file",
            "/wp-admin": "wordpress",
            "/wp-config.php": "wordpress",
            "/phpmyadmin/": "phpmyadmin",
            "/.git/config": "source-control",
            "/../../etc/passwd": "path-traversal",
            "/search?q=UNION%20SELECT": "sql-injection",
            "/.aws/credentials": "cloud-or-ssh-keys",
            "/backup.zip": "backup-or-database-dump",
            "/geoserver/web": "geoserver",
        }
        for target, expected in examples.items():
            with self.subTest(target=target):
                self.assertEqual(classify_probe(target), expected)

    def test_encoded_traversal_is_recognized(self):
        self.assertEqual(classify_probe("/%252e%252e/%252e%252e/etc/passwd"), "path-traversal")

    def test_normal_benchmark_paths_are_not_probes(self):
        for target in ("/", "/index.html", "/results.html", "/email-results.html", "/favicon.ico"):
            with self.subTest(target=target):
                self.assertIsNone(classify_probe(target))


class RequestWatchdogTests(unittest.TestCase):
    def setUp(self):
        self.watchdog = RequestWatchdog(log_path=None)
        self.source = "203.0.113.8"

    def test_bans_on_fifth_probe_within_sixty_seconds(self):
        for i in range(PROBE_LIMIT - 1):
            event = self.watchdog.record_probe(self.source, "environment-file", now=100 + i)
            self.assertEqual(event["action"], "rejected")
        event = self.watchdog.record_probe(self.source, "wordpress", now=104)
        self.assertEqual(event["action"], "banned-10m")
        self.assertEqual(event["probe_count_60s"], 5)
        self.assertEqual(event["ban_seconds"], BAN_SECONDS)
        self.assertEqual(self.watchdog.ban_remaining(self.source, now=104), BAN_SECONDS)

    def test_probe_counts_expire_outside_the_window(self):
        for i in range(4):
            self.watchdog.record_probe(self.source, "environment-file", now=i)
        event = self.watchdog.record_probe(self.source, "wordpress", now=64)
        self.assertEqual(event["action"], "rejected")
        self.assertEqual(event["probe_count_60s"], 1)

    def test_ban_expires_after_ten_minutes(self):
        for i in range(PROBE_LIMIT):
            event = self.watchdog.record_probe(self.source, "environment-file", now=10 + i)
        self.assertEqual(event["action"], "banned-10m")
        self.assertEqual(self.watchdog.ban_remaining(self.source, now=613), 1)
        self.assertEqual(self.watchdog.ban_remaining(self.source, now=614), 0)

    def test_loopback_and_nat_sources_are_exempt(self):
        for source in ("127.0.0.1", "::1", "10.0.2.2", "10.0.2.15", "::ffff:10.0.2.2"):
            with self.subTest(source=source):
                event = self.watchdog.record_probe(source, "environment-file", now=100)
                self.assertEqual(event["action"], "rejected-exempt-source")
                self.assertEqual(self.watchdog.ban_remaining(source, now=100), 0)

    def test_ipv6_sources_are_counted_without_ipv4_network_errors(self):
        event = self.watchdog.record_probe("2001:db8::8", "wordpress", now=100)
        self.assertEqual(event["action"], "rejected")
        self.assertEqual(self.watchdog.ban_remaining("2001:db8::8", now=100), 0)


class HandlerIntegrationTests(unittest.TestCase):
    def setUp(self):
        self.watchdog = RequestWatchdog(log_path=None)
        self.watchdog_patch = patch("server.WATCHDOG", self.watchdog)
        self.watchdog_patch.start()
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()
        self.port = self.server.server_address[1]

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.watchdog_patch.stop()

    def request(self, target):
        request = Request(
            f"http://127.0.0.1:{self.port}{target}",
            headers={"Host": f"127.0.0.1:{self.port}"},
        )
        try:
            with urlopen(request, timeout=2) as response:
                return response.status
        except HTTPError as error:
            return error.code

    def test_allowed_page_is_served_and_probe_is_rejected(self):
        self.assertEqual(self.request("/"), 200)
        self.assertEqual(self.request("/.env"), 403)


if __name__ == "__main__":
    unittest.main()
