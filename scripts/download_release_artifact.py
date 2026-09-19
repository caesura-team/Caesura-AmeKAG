#!/usr/bin/env python3
"""Download one externally selected Actions artifact by ID and transport digest.

The API redirect request is authenticated; the signed blob request never gets
that credential. No retry, name selection, extraction, or publication occurs.
The caller authenticates job/run/attempt provenance before invoking this layer.
"""
from __future__ import annotations

import hashlib
from contextlib import contextmanager
from http.client import HTTPException, HTTPResponse, HTTPSConnection, IncompleteRead
import json
from pathlib import Path
import re
import socket
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request

from package_verification import _sha256_file

MAX_BYTES = 8 * 1024**3
MAX_SECONDS = 900


def _need(condition, message):
    if not condition:
        raise ValueError(message)


class _Deadline:
    """Interrupt only the socket retained by this download, including headers.

    A DNS resolver has no owned socket to interrupt; connect/TLS also retain
    their socket timeout. The total timer is never restarted between requests.
    """
    def __init__(self, seconds):
        self.end = time.monotonic() + seconds
        self.lock = threading.Lock()
        self.socket = None
        self.stopped = threading.Event()
        self.expired = False
        self.worker = threading.Thread(target=self._watch, name="artifact-download-deadline", daemon=True)
        self.worker.start()

    def _watch(self):
        if self.stopped.wait(max(0, self.end - time.monotonic())):
            return
        with self.lock:
            self.expired = True
            if self.socket is not None:
                try:
                    self.socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass  # It may already have reached EOF; the caller still fails.

    def check(self):
        _need(not self.expired and time.monotonic() < self.end,
              "Artifact download exceeded time limit")

    def retain(self, owned_socket):
        with self.lock:
            self.check()
            self.socket = owned_socket

    def release(self):
        with self.lock:
            self.socket = None

    def close(self):
        self.stopped.set()
        self.worker.join()


@contextmanager
def _owned_open(request, deadline, connection_factory):
    parsed = urlsplit(request.full_url)
    deadline.check()
    connection = connection_factory(parsed.hostname, timeout=min(30, deadline.end - time.monotonic()))
    response = None
    try:
        connection.connect()
        # Keep the actual socket even when HTTPConnection hands a close-framed
        # response its makefile and clears connection.sock in getresponse().
        deadline.retain(connection.sock)
        target = parsed.path + ("?" + parsed.query if parsed.query else "")
        connection.request("GET", target, headers=dict(request.header_items()))
        response = connection.getresponse()
        deadline.check()
        yield response
    except (HTTPException, OSError) as error:
        raise ValueError("HTTP transport failed: " + type(error).__name__) from None
    finally:
        deadline.release()
        if response is not None:
            response.close()
        connection.close()


def _headers(headers, name):
    if hasattr(headers, "get_all"):
        return headers.get_all(name, [])
    return [value for key, value in headers.items() if key.lower() == name.lower()]


def _blob_url(value):
    _need(isinstance(value, str) and 0 < len(value) <= 16384
          and not any(ord(char) < 33 or ord(char) == 127 for char in value), "Invalid artifact redirect")
    try:
        parsed = urlsplit(value)
        port = parsed.port
    except ValueError:
        raise ValueError("Invalid artifact redirect URL") from None
    _need(parsed.scheme == "https" and parsed.hostname and port in (None, 443)
          and not parsed.username and not parsed.password and not parsed.fragment,
          "Unsafe artifact redirect")
    _need(any(parsed.hostname.endswith(suffix) for suffix in
              (".blob.core.windows.net", ".githubusercontent.com")), "Unrecognized artifact redirect host")
    return parsed.hostname


def _open(opener, request):
    try:
        return opener(request, timeout=30)
    except HTTPError as response:
        return response
    except (URLError, HTTPException, OSError) as error:
        # Exception strings may contain the signed URL. Preserve the class,
        # never a token, Location header or query string in diagnostics.
        raise ValueError("HTTP transport failed: " + type(error).__name__) from None


def download_artifact(*, repository, artifact_id, expected_sha256, token, work_dir,
                      opener=None, max_bytes=MAX_BYTES, connection_factory=None):
    """Save exact transfer bytes to a NEW directory and keep first-failure evidence.

    Supplying an opener or connection_factory always marks the report fixture.
    The legacy opaque opener is only for fixtures; socket deadline tests use a
    connection_factory so the production connection ownership path is exercised.
    expected_sha256 is the externally authenticated Actions ZIP digest, not an
    inner package checksum. Returned archive may then be safely prepared using
    package_verification.prepare_package with the same digest.
    """
    _need(isinstance(repository, str) and re.fullmatch(
        r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*", repository), "Invalid repository")
    _need(type(artifact_id) is int and artifact_id > 0, "Invalid artifact ID")
    _need(isinstance(expected_sha256, str) and re.fullmatch("[0-9a-f]{64}", expected_sha256), "Invalid artifact digest")
    _need(isinstance(token, str) and token and not any(char in token for char in "\r\n\x00"), "Missing/invalid read token")
    _need(type(max_bytes) is int and 0 < max_bytes <= MAX_BYTES, "Invalid byte limit")
    _need(opener is None or connection_factory is None, "Choose one fixture transport")
    work = Path(work_dir).absolute()
    _need(work.parent.is_dir(), "Download parent must exist")
    for parent in (work.parent, *work.parent.parents):
        _need(not parent.is_symlink() and not parent.is_junction(), "Download parent traverses a link")
    _need(work.parent == work.parent.resolve(strict=True), "Download parent is not canonical")
    work.mkdir()
    archive = work / "artifact.zip"
    report = {"schema":"caesura.artifact-download.v1", "status":"FAIL", "release_ready":False,
        "transport":"github" if opener is None and connection_factory is None else "fixture", "repository":repository,
        "artifact_id":artifact_id, "expected_sha256":expected_sha256, "bytes":0,
        "archive":{"path":str(archive), "sha256":None}, "errors":[]}
    started = time.monotonic()
    deadline = _Deadline(MAX_SECONDS)
    digest = hashlib.sha256()
    def request_open(request):
        if opener is not None:
            deadline.check()
            return _open(opener, request)
        return _owned_open(request, deadline, connection_factory or HTTPSConnection)
    try:
        endpoint = f"https://api.github.com/repos/{repository}/actions/artifacts/{artifact_id}/zip"
        request = Request(endpoint, headers={"Authorization":"Bearer " + token,
            "Accept":"application/vnd.github+json", "X-GitHub-Api-Version":"2022-11-28",
            "User-Agent":"Caesura-release-inputs"}, method="GET")
        with request_open(request) as response:
            deadline.check()
            report["api_status"] = response.status
            _need(response.status == 302, "Artifact API did not return the required redirect")
            locations = _headers(response.headers, "Location")
            _need(len(locations) == 1, "Artifact API returned ambiguous redirect")
            location = locations[0]
            report["blob_host"] = _blob_url(location)
        # Never reuse the authenticated Request or an automatic redirect chain.
        request = Request(location, headers={"User-Agent":"Caesura-release-inputs"}, method="GET")
        with request_open(request) as response:
            deadline.check()
            report["blob_status"] = response.status
            _need(response.status == 200, "Artifact blob did not return HTTP 200")
            lengths, transfers = _headers(response.headers, "Content-Length"), _headers(response.headers, "Transfer-Encoding")
            _need(len(lengths) <= 1 and not (lengths and transfers), "Ambiguous artifact body framing")
            declared = None
            if lengths:
                _need(re.fullmatch(r"[0-9]{1,12}", lengths[0].strip()), "Invalid artifact Content-Length")
                declared = int(lengths[0].strip())
                _need(declared <= max_bytes, "Artifact Content-Length exceeds byte limit")
            if transfers:
                _need(len(transfers) == 1 and transfers[0].lower() == "chunked"
                      and getattr(response, "chunked", False) is True, "Artifact chunk parser is not enabled")
            with archive.open("xb") as stream:
                def preserve(block):
                    # One extra byte proves an exceeded limit without retaining
                    # an unbounded exception payload from an injected transport.
                    block = block[:max_bytes - report["bytes"] + 1]
                    stream.write(block)
                    digest.update(block)
                    report["bytes"] += len(block)
                while True:
                    deadline.check()
                    try:
                        # read1 returns available decoded bytes promptly. The
                        # watchdog also bounds chunk-header and initial-header
                        # reads, which read1 alone cannot bound.
                        read = getattr(response, "read1", response.read)
                        block = read(min(1024 * 1024, max_bytes - report["bytes"] + 1))
                    except IncompleteRead as error:
                        if (getattr(read, "__func__", None) is HTTPResponse.read1
                                and response.chunked):
                            # stdlib chunked read1 has already returned payload
                            # incrementally; its nonempty exception partial can
                            # only be the incomplete two-byte chunk delimiter.
                            # Retain that framing separately, never in the ZIP.
                            _need(len(error.partial) <= 2, "Unexpected chunk framing partial")
                            report["http_framing_partial"] = {
                                "bytes":len(error.partial), "hex":error.partial.hex(),
                                "sha256":hashlib.sha256(error.partial).hexdigest()}
                        else:
                            preserve(error.partial)
                        raise ValueError("HTTP transport failed: IncompleteRead") from None
                    except (HTTPException, OSError) as error:
                        raise ValueError("HTTP transport failed: " + type(error).__name__) from None
                    preserve(block)
                    deadline.check()
                    if not block:
                        break
                    _need(report["bytes"] <= max_bytes, "Artifact exceeds byte limit")
            report["archive"]["sha256"] = digest.hexdigest()
            _need(declared is None or report["bytes"] == declared, "Incomplete artifact Content-Length")
            _need(report["archive"]["sha256"] == expected_sha256, "Artifact transport digest mismatch")
            _need(_sha256_file(archive) == expected_sha256, "Artifact bytes changed after download")
            deadline.check()
        report["status"] = "ARTIFACT_DOWNLOADED"
        return report
    except ValueError as error:
        # All generated messages are controlled strings; URL parser errors can
        # quote input fragments, so remove both secrets defensively as well.
        report["errors"].append(str(error).replace(token, "<redacted>").replace(locals().get("location", "\x00"), "<redacted>"))
        raise ValueError(report["errors"][-1]) from None
    except (OSError, HTTPException) as error:
        report["errors"].append("Download failed: " + type(error).__name__)
        raise ValueError(report["errors"][-1]) from None
    finally:
        deadline.close()
        if archive.exists():
            report["archive"]["sha256"] = digest.hexdigest()
        report["deadline_exceeded"] = deadline.expired or time.monotonic() >= deadline.end
        report["seconds"] = time.monotonic() - started
        with (work / "download.json").open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
