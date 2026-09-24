"""Real byte transport fixtures, never hosted artifact authentication."""
from __future__ import annotations

import hashlib
from http.client import HTTPConnection, IncompleteRead
from http.server import BaseHTTPRequestHandler, HTTPServer
import io
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest import mock
from urllib.error import HTTPError
from urllib.request import Request, urlopen
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import download_release_artifact as product
from download_release_artifact import download_artifact


class DownloadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="caesura-artifact-fixture-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, "w") as archive:
            archive.writestr("manifest.json", '{"test":"transport fixture"}')
        self.body = stream.getvalue()
        self.expected = hashlib.sha256(self.body).hexdigest()
        self.token = "disposable-fixture-auth-token"
        self.location = "https://productionresultssa1.blob.core.windows.net/fixture/artifact.zip?sig=fixture-secret"
        self.requests = []
        self.counter = 0

    def redirect(self, request, timeout):
        self.requests.append(request)
        if len(self.requests) == 1:
            raise HTTPError(request.full_url, 302, "Found", {"Location":self.location}, io.BytesIO())
        response = io.BytesIO(self.body)
        response.status = 200
        response.headers = {"Content-Length":str(len(self.body))}
        return response

    def download(self, **changes):
        self.counter += 1
        self.requests = []
        args = dict(repository="owner/engine", artifact_id=42, expected_sha256=self.expected,
                    token=self.token, work_dir=self.root / str(self.counter), opener=self.redirect)
        return download_artifact(**(args | changes))

    def test_fixed_id_exact_bytes_and_no_credentials_on_blob_request(self):
        report = self.download()
        self.assertEqual(report["status"], "ARTIFACT_DOWNLOADED")
        self.assertEqual(report["transport"], "fixture")
        self.assertFalse(report["release_ready"])
        self.assertEqual(self.requests[0].full_url, "https://api.github.com/repos/owner/engine/actions/artifacts/42/zip")
        self.assertEqual(self.requests[0].get_header("Authorization"), "Bearer " + self.token)
        self.assertIsNone(self.requests[1].get_header("Authorization"))
        self.assertEqual(Path(report["archive"]["path"]).read_bytes(), self.body)
        serialized = (self.root / str(self.counter) / "download.json").read_text(encoding="utf-8")
        self.assertNotIn(self.token, serialized)
        self.assertNotIn("fixture-secret", serialized)

    def test_digest_mismatch_is_failure_with_original_partial_bytes_retained(self):
        with self.assertRaisesRegex(ValueError, "digest"):
            self.download(expected_sha256="0" * 64)
        report = json.loads((self.root / "1/download.json").read_text())
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual((self.root / "1/artifact.zip").read_bytes(), self.body)

    def test_unknown_or_insecure_redirect_is_rejected_before_second_request(self):
        for location in ("http://localhost/x", "https://evil.example/x", "https://token@x.blob.core.windows.net/x",
                         "https://x.blob.core.windows.net:444/x", "https://x.blob.core.windows.net/x#fragment"):
            with self.subTest(location=location):
                self.location = location
                with self.assertRaisesRegex(ValueError, "redirect"):
                    self.download()
                self.assertEqual(len(self.requests), 1)

    def test_blob_redirect_or_error_is_not_followed_or_retried(self):
        for status in (302, 403, 410, 500):
            self.requests = []
            def opener(request, timeout):
                self.requests.append(request)
                raise HTTPError(request.full_url, 302 if len(self.requests) == 1 else status,
                                "fixture", {"Location":self.location}, io.BytesIO())
            with self.subTest(status=status), self.assertRaises(ValueError):
                self.download(opener=opener)
            self.assertEqual(len(self.requests), 2)

    def test_existing_attempt_is_not_overwritten(self):
        directory = self.root / "prior"
        directory.mkdir()
        (directory / "original").write_text("failed")
        with self.assertRaises(FileExistsError):
            self.download(work_dir=directory)
        self.assertEqual((directory / "original").read_text(), "failed")

    def test_boolean_id_and_malformed_identity_rejected(self):
        for change in ({"artifact_id":True}, {"repository":"owner/../engine"}, {"expected_sha256":"x"}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.download(**change)
            self.assertEqual(self.requests, [])

    def test_byte_limit_is_enforced(self):
        with self.assertRaisesRegex(ValueError, "limit"):
            self.download(max_bytes=len(self.body) - 1)

    def test_actual_http_truncated_content_length_fails_even_if_digest_matches_partial(self):
        fixture = self
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.send_header("Content-Length", str(len(fixture.body) + 10))
                self.end_headers()
                self.wfile.write(fixture.body)
            def log_message(self, *_):
                pass
        server = HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        def opener(request, timeout):
            if not self.requests:
                return self.redirect(request, timeout)
            self.requests.append(request)
            return urlopen(Request(f"http://127.0.0.1:{server.server_port}/"), timeout=timeout)
        try:
            with self.assertRaisesRegex(ValueError, "length|Length|Incomplete"):
                self.download(opener=opener)
        finally:
            server.shutdown(); server.server_close(); thread.join()
        self.assertEqual(len(self.requests), 2)

    def wire_server(self, handle, api_handle=None):
        fixture = self
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path.startswith("/repos/"):
                    if api_handle is not None:
                        api_handle(self)
                    else:
                        self.send_response(302)
                        self.send_header("Location", fixture.location)
                        self.end_headers()
                    return
                fixture.assertIsNone(self.headers.get("Authorization"))
                handle(self)
            def log_message(self, *_):
                pass
        server = HTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        def cleanup():
            server.shutdown()
            server.server_close()
            thread.join(timeout=3)
            self.assertFalse(thread.is_alive())
        self.addCleanup(cleanup)
        return server

    def wire_opener(self, server):
        def opener(request, timeout):
            if not self.requests:
                return self.redirect(request, timeout)
            self.requests.append(request)
            return urlopen(Request(f"http://127.0.0.1:{server.server_port}/"), timeout=timeout)
        return opener

    @staticmethod
    def wire_connection(server):
        return lambda host, timeout: HTTPConnection("127.0.0.1", server.server_port, timeout=timeout)

    def test_actual_incomplete_chunk_preserves_received_bytes_and_failure_digest(self):
        def handle(handler):
            handler.send_response(200)
            handler.send_header("Transfer-Encoding", "chunked")
            handler.end_headers()
            handler.wfile.write(f"{len(self.body):x}\r\n".encode() + self.body + b"\r\n")
            handler.wfile.flush()
        server = self.wire_server(handle)
        with self.assertRaisesRegex(ValueError, "IncompleteRead"):
            self.download(opener=self.wire_opener(server))
        saved = self.root / "1"
        report = json.loads((saved / "download.json").read_text())
        self.assertEqual((saved / "artifact.zip").read_bytes(), self.body)
        self.assertEqual(report["bytes"], len(self.body))
        self.assertEqual(report["archive"]["sha256"], self.expected)
        self.assertEqual(report["status"], "FAIL")

    def stalled_body_server(self):
        release = threading.Event()
        sent = threading.Event()
        self.addCleanup(release.set)
        def handle(handler):
            handler.send_response(200)
            handler.send_header("Content-Length", str(len(self.body)))
            handler.end_headers()
            handler.wfile.write(self.body[:10])
            handler.wfile.flush()
            sent.set()
            release.wait(.8)
            try:
                handler.wfile.write(self.body[10:])
            except OSError:
                pass
        server = self.wire_server(handle)
        return server, release, sent

    def assert_partial_body_failure(self):
        report = json.loads((self.root / "1/download.json").read_text())
        self.assertEqual(report["status"], "FAIL")
        self.assertEqual(report["bytes"], 10)
        self.assertEqual((self.root / "1/artifact.zip").read_bytes(), self.body[:10])
        self.assertEqual(report["archive"]["sha256"], hashlib.sha256(self.body[:10]).hexdigest())
        self.assertFalse(any(t.name == "artifact-download-deadline" for t in threading.enumerate()))
        return report

    def test_earlier_socket_timeout_is_not_a_total_deadline_expiry(self):
        server, release, sent = self.stalled_body_server()
        # A socket may reject the read before the independent total deadline.
        # Keep the real transport and the strict return-time/partial-byte
        # contract, but do not attribute this failure to the watchdog.
        connection = lambda host, timeout: HTTPConnection(
            "127.0.0.1", server.server_port, timeout=min(timeout, .02))
        started = time.monotonic()
        try:
            with mock.patch.object(product, "MAX_SECONDS", 1), self.assertRaisesRegex(
                    ValueError, "HTTP transport failed: TimeoutError"):
                self.download(opener=None, connection_factory=connection)
        finally:
            elapsed = time.monotonic() - started
            release.set()
        self.assertTrue(sent.is_set())
        self.assertLess(elapsed, .5, "A blocked body read ignored its socket timeout")
        report = self.assert_partial_body_failure()
        self.assertEqual(report["errors"], ["HTTP transport failed: TimeoutError"])
        self.assertFalse(report["deadline_exceeded"])

    def test_body_read_checks_total_deadline_after_retaining_available_bytes(self):
        server, release, sent = self.stalled_body_server()
        prefix_read = threading.Event()
        # Advance only the host clock, after ten bytes have really crossed
        # HTTPResponse.read1. Production streaming, preservation and deadline
        # checks remain intact. Socket timeouts keep their requested budget;
        # this does not rely on Windows cross-thread shutdown waking select.
        def clock():
            return time.perf_counter() + (1 if prefix_read.is_set() else 0)

        class ObservedConnection(HTTPConnection):
            def getresponse(connection):
                response = super().getresponse()
                read1 = response.read1
                received = 0
                def observe_read1(size):
                    nonlocal received
                    block = read1(size)
                    received += len(block)
                    if received >= 10:
                        prefix_read.set()
                    return block
                response.read1 = observe_read1
                return response

        connection = lambda host, timeout: ObservedConnection(
            "127.0.0.1", server.server_port, timeout=timeout)
        started = time.monotonic()
        try:
            with mock.patch.object(product, "MAX_SECONDS", .1), \
                    mock.patch.object(product, "time", SimpleNamespace(perf_counter=clock)), \
                    self.assertRaisesRegex(ValueError, "Artifact download exceeded time limit"):
                self.download(opener=None, connection_factory=connection)
        finally:
            elapsed = time.monotonic() - started
            release.set()
        self.assertTrue(sent.is_set())
        self.assertTrue(prefix_read.is_set())
        self.assertLess(elapsed, .5, "Available body bytes did not reach the deadline check")
        report = self.assert_partial_body_failure()
        self.assertEqual(report["errors"], ["Artifact download exceeded time limit"])
        self.assertTrue(report["deadline_exceeded"])

    def test_production_connection_path_bounds_api_headers_blob_headers_and_chunk_headers(self):
        for stage in ("api-headers", "blob-headers", "chunk-header"):
            with self.subTest(stage=stage):
                release = threading.Event()
                reached = threading.Event()
                def hold(handler):
                    if stage == "chunk-header":
                        prefix = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n9"
                    else:
                        prefix = b"HTTP/1.1 200 OK\r\nX-Incomplete: "
                    handler.wfile.write(prefix)
                    handler.wfile.flush()
                    reached.set()
                    release.wait(.8)
                server = self.wire_server(hold, api_handle=hold if stage == "api-headers" else None)
                started = time.monotonic()
                try:
                    with mock.patch.object(product, "MAX_SECONDS", .1), self.assertRaises(ValueError):
                        self.download(opener=None, connection_factory=self.wire_connection(server))
                finally:
                    elapsed = time.monotonic() - started
                    release.set()
                self.assertTrue(reached.is_set())
                self.assertLess(elapsed, .5, "An owned header read outlived the total deadline")
                report = json.loads((self.root / str(self.counter) / "download.json").read_text())
                self.assertEqual(report["status"], "FAIL")
                self.assertTrue(report["deadline_exceeded"])
                self.assertEqual(report["transport"], "fixture")
                self.assertFalse(report["release_ready"])
                self.assertFalse(any(t.name == "artifact-download-deadline" for t in threading.enumerate()))

    def test_production_connection_complete_download_and_no_auth_on_blob(self):
        def handle(handler):
            handler.send_response(200)
            handler.send_header("Content-Length", str(len(self.body)))
            handler.end_headers()
            handler.wfile.write(self.body)
        server = self.wire_server(handle)
        report = self.download(opener=None, connection_factory=self.wire_connection(server))
        self.assertEqual(report["status"], "ARTIFACT_DOWNLOADED")
        self.assertEqual(report["archive"]["sha256"], self.expected)
        self.assertFalse(report["deadline_exceeded"])

    def test_incomplete_read_exception_partial_is_preserved(self):
        class Broken(io.BytesIO):
            status = 200
            headers = {}
            def read1(inner, _):
                raise IncompleteRead(self.body)
        def opener(request, timeout):
            if not self.requests:
                return self.redirect(request, timeout)
            self.requests.append(request)
            return Broken()
        with self.assertRaisesRegex(ValueError, "IncompleteRead"):
            self.download(opener=opener)
        report = json.loads((self.root / "1/download.json").read_text())
        self.assertEqual((self.root / "1/artifact.zip").read_bytes(), self.body)
        self.assertEqual(report["bytes"], len(self.body))
        self.assertEqual(report["archive"]["sha256"], self.expected)

    def test_chunk_delimiter_fragment_is_not_appended_to_downloaded_zip(self):
        def handle(handler):
            handler.send_response(200)
            handler.send_header("Transfer-Encoding", "chunked")
            handler.end_headers()
            handler.wfile.write(f"{len(self.body):x}\r\n".encode() + self.body + b"\r")
            handler.wfile.flush()
        server = self.wire_server(handle)
        with self.assertRaisesRegex(ValueError, "IncompleteRead"):
            self.download(opener=None, connection_factory=self.wire_connection(server))
        report = json.loads((self.root / "1/download.json").read_text())
        self.assertEqual((self.root / "1/artifact.zip").read_bytes(), self.body)
        self.assertEqual(report["bytes"], len(self.body))
        self.assertEqual(report["archive"]["sha256"], self.expected)
        self.assertEqual(report["http_framing_partial"], {
            "bytes":1, "hex":"0d", "sha256":hashlib.sha256(b"\r").hexdigest()})


if __name__ == "__main__":
    unittest.main(verbosity=2)
