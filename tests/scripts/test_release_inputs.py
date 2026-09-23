"""Hosted-boundary regressions; injected API data is always test-fixture evidence."""
from __future__ import annotations

import copy
import hashlib
from http.client import IncompleteRead
from http.server import BaseHTTPRequestHandler, HTTPServer
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest import mock
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit
from urllib.request import urlopen

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
from verify_release_inputs import GitHubAPI, HostedInputsError, main, verify_hosted_inputs


class Response(io.BytesIO):
    def __init__(self, value, headers=None, status=200):
        super().__init__(value if isinstance(value, bytes) else json.dumps(value).encode())
        self.headers = headers or {}
        self.status = status


class HostedFixture:
    def __init__(self):
        self.expected = dict(schema_version=1, repository="owner/engine", repository_id=11,
            source_sha="a" * 40, trigger_head_sha="a" * 40, source_mode="head",
            run_id=12, run_attempt=1, workflow_path=".github/workflows/ci.yml",
            called_workflow_path=".github/workflows/validate-engine.yml", called_workflow_sha="b" * 40)
        self.policy = dict(schema_version=1, required_jobs={"linux": "Validate / Linux", "web": "Validate / Web"},
                           artifact_roles={"linux":"linux", "web":"web"})
        self.producers = dict(schema_version=1, artifacts={role: dict(artifact_id=40 + i,
            artifact_digest="sha256:" + str(i + 1) * 64, job_id=30 + i, run_attempt=1,
            manifest_sha256=str(i + 3) * 64) for i, role in enumerate(self.policy["artifact_roles"])})
        self.run = dict(id=12, run_attempt=1, head_sha="a" * 40, workflow_id=13,
            path=self.expected["workflow_path"], status="in_progress", conclusion=None, event="push",
            repository=dict(id=11, full_name="owner/engine"), head_repository=dict(id=11),
            referenced_workflows=[dict(path="owner/engine/.github/workflows/validate-engine.yml@refs/heads/main", sha="b" * 40)],
            pull_requests=[])
        self.workflow = dict(id=13, path=self.expected["workflow_path"])
        self.jobs = {1: [dict(id=30 + i, run_id=12, run_attempt=1, name=name,
            head_sha="a" * 40, status="completed", conclusion="success")
            for i, name in enumerate(self.policy["required_jobs"].values())]}
        self.attempts = {1: copy.deepcopy(self.run)}
        self.artifacts = {entry["artifact_id"]: dict(id=entry["artifact_id"], name=role,
            digest=entry["artifact_digest"], expired=False, expires_at="2999-01-01T00:00:00Z",
            workflow_run=dict(id=12, repository_id=11, head_repository_id=11, head_sha="a" * 40))
            for role, entry in self.producers["artifacts"].items()}
        self.calls = []
        self.override = {}
        self.run_reads = 0
        self.advance_on_final_read = False

    def opener(self, request, timeout):
        self.calls.append(request)
        assert request.method == "GET"
        parsed = urlsplit(request.full_url)
        assert parsed.scheme == "https" and parsed.netloc == "api.github.com"
        route = parsed.path.removeprefix("/repos/owner/engine")
        key = route + ("?" + parsed.query if parsed.query else "")
        if key in self.override:
            result = self.override[key]
            if isinstance(result, Exception):
                raise result
            return result() if callable(result) else Response(result)
        if route == "/actions/runs/12":
            self.run_reads += 1
            value = copy.deepcopy(self.run)
            if self.advance_on_final_read and self.run_reads > 1:
                value["run_attempt"] += 1
            return Response(value)
        if route == "/actions/workflows/13":
            return Response(self.workflow)
        if route.startswith("/actions/runs/12/attempts/"):
            attempt = int(route.split("/")[5])
            if route.endswith("/jobs"):
                page = int(parse_qs(parsed.query)["page"][0])
                size = int(parse_qs(parsed.query)["per_page"][0])
                jobs = self.jobs[attempt]
                headers = {}
                if page * size < len(jobs):
                    headers["Link"] = f'<https://api.github.com{parsed.path}?per_page={size}&page={page + 1}>; rel="next"'
                return Response(dict(total_count=len(jobs), jobs=jobs[(page - 1) * size:page * size]), headers)
            return Response(self.attempts[attempt])
        if route.startswith("/actions/artifacts/"):
            return Response(self.artifacts[int(route.rsplit("/", 1)[1])])
        raise AssertionError(f"Unexpected API request: {request.full_url}")

    def add_prior_attempt(self):
        self.expected["run_attempt"] = self.run["run_attempt"] = 2
        self.attempts[2] = copy.deepcopy(self.run)
        self.jobs[2] = [dict(job, id=job["id"] + 10, run_attempt=2) for job in self.jobs[1]]
        self.attempts[1].update(status="completed", conclusion="success")
        for entry in self.producers["artifacts"].values():
            entry.update(job_id=entry["job_id"] + 10, run_attempt=2)

    def use_pr_merge(self):
        self.expected.update(source_mode="pull-request-merge", source_sha="c" * 40, pull_request_number=7)
        association = dict(number=7, head=dict(sha="a" * 40), base=dict(sha="d" * 40, repo=dict(id=11)))
        self.run.update(event="pull_request", pull_requests=[association])
        self.attempts[1] = copy.deepcopy(self.run)
        self.override["/pulls/7"] = dict(number=7, merge_commit_sha="c" * 40,
            head=dict(sha="a" * 40, repo=dict(id=11)), base=dict(sha="d" * 40, repo=dict(id=11)))
        self.override["/git/commits/" + "c" * 40] = dict(sha="c" * 40,
            parents=[dict(sha="d" * 40), dict(sha="a" * 40)])


class HostedInputsTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="caesura-hosted-fixture-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.fixture = HostedFixture()
        self.counter = 0

    def verify(self):
        self.counter += 1
        api = GitHubAPI("fixture-token-must-never-be-persisted", self.root / str(self.counter), opener=self.fixture.opener)
        return verify_hosted_inputs(self.fixture.expected, self.fixture.policy, self.fixture.producers, api)

    def reject(self, text=None):
        with self.assertRaises(HostedInputsError) as error:
            self.verify()
        if text:
            self.assertIn(text, str(error.exception))
        summary = json.loads((self.root / str(self.counter) / "summary.json").read_text())
        self.assertEqual(summary["status"], "FAIL")
        self.assertFalse(summary["release_ready"])
        return summary

    def test_current_in_progress_run_with_completed_required_jobs_is_verified_fixture_only(self):
        report = self.verify()
        self.assertEqual(report["status"], "HOSTED_INPUTS_VERIFIED")
        self.assertEqual(report["transport"], "fixture")
        self.assertFalse(report["release_ready"])
        self.assertEqual(set(report["jobs"]), {"linux", "web"})
        self.assertEqual(report["unverified"]["package_payloads"], "NOT_VERIFIED")
        self.assertEqual(report["unverified"]["execution_evidence"], "NOT_VERIFIED")
        self.assertEqual(report["artifacts"]["linux"]["artifact_id"], 40)

    def test_every_required_unsuccessful_state_is_rejected(self):
        for status, conclusion in [("completed", item) for item in (
                "failure", "cancelled", "skipped", "timed_out", "action_required", "neutral", "stale", None)] + [
                ("queued", None), ("in_progress", None), ("waiting", None), ("unknown", "success")]:
            with self.subTest(status=status, conclusion=conclusion):
                self.fixture.jobs[1][0].update(status=status, conclusion=conclusion)
                self.reject("Required")

    def test_required_job_missing_or_duplicated_is_rejected(self):
        jobs = copy.deepcopy(self.fixture.jobs[1])
        for changed in (jobs[1:], jobs + [dict(jobs[0], id=99)]):
            with self.subTest(jobs=changed):
                self.fixture.jobs[1] = changed
                self.reject("exactly one")

    def test_wrong_run_repository_source_attempt_workflow_and_called_sha_fail(self):
        mutations = [("id", 99), ("run_attempt", 2), ("head_sha", "f" * 40),
            ("path", ".github/workflows/other.yml"), ("repository", dict(id=99, full_name="owner/engine")),
            ("repository", dict(id=11, full_name="other/engine")),
            ("referenced_workflows", []), ("referenced_workflows", [dict(
                path="owner/engine/.github/workflows/validate-engine.yml@main", sha="f" * 40)])]
        before = copy.deepcopy(self.fixture.run)
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                self.fixture.run = dict(before, **{key: value})
                self.reject()

    def test_attempt_response_and_workflow_id_metadata_are_independently_checked(self):
        self.fixture.attempts[1]["run_attempt"] = 2
        self.reject()
        self.fixture.attempts[1]["run_attempt"] = 1
        self.fixture.workflow["path"] = ".github/workflows/untrusted.yml"
        self.reject()

    def test_new_attempt_started_during_verification_is_rejected(self):
        self.fixture.advance_on_final_read = True
        self.reject("attempt")

    def test_full_attempt_history_is_preserved_and_prior_failure_is_never_hidden(self):
        self.fixture.add_prior_attempt()
        self.fixture.jobs[1][0]["conclusion"] = "failure"
        self.fixture.attempts[1]["conclusion"] = "failure"
        report = self.reject("attempt 1")
        self.assertEqual([item["attempt"] for item in report["attempts"]], [1, 2])
        self.assertEqual(report["attempts"][0]["jobs"]["linux"]["conclusion"], "failure")

    def test_all_green_prior_attempt_is_recorded_but_mixed_producer_attempt_is_rejected(self):
        self.fixture.add_prior_attempt()
        self.assertEqual(len(self.verify()["attempts"]), 2)
        self.fixture.producers["artifacts"]["linux"]["run_attempt"] = 1
        self.reject("producer")

    def test_jobs_are_fully_paginated_and_duplicate_pages_or_missing_next_fail(self):
        self.fixture.jobs[1].extend(dict(self.fixture.jobs[1][0], id=100 + i, name=f"Optional {i}") for i in range(101))
        self.assertEqual(self.verify()["attempts"][0]["job_count"], 103)
        page2 = "/actions/runs/12/attempts/1/jobs?per_page=100&page=2"
        self.fixture.override[page2] = dict(total_count=103, jobs=self.fixture.jobs[1][:3])
        self.reject("Duplicate")
        del self.fixture.override[page2]
        self.fixture.override["/actions/runs/12/attempts/1/jobs?per_page=100&page=1"] = dict(
            total_count=103, jobs=self.fixture.jobs[1][:100])
        self.reject("pagination")

    def test_pagination_count_drift_truncated_pages_and_upper_bound_fail(self):
        endpoint = "/actions/runs/12/attempts/1/jobs?per_page=100&page=1"
        for count, jobs in ((True, []), (10001, []), (3, self.fixture.jobs[1]), (1, self.fixture.jobs[1])):
            with self.subTest(count=count):
                self.fixture.override[endpoint] = dict(total_count=count, jobs=jobs)
                self.reject()

    def test_job_run_attempt_source_and_boolean_id_cannot_be_substituted(self):
        before = copy.deepcopy(self.fixture.jobs[1][0])
        for key, value in (("run_id", 98), ("run_attempt", 2), ("head_sha", "e" * 40), ("id", True)):
            with self.subTest(key=key):
                self.fixture.jobs[1][0] = dict(before, **{key: value})
                self.reject()

    def test_artifact_id_run_repo_head_digest_expiry_are_checked_without_name_lookup(self):
        before = copy.deepcopy(self.fixture.artifacts[40])
        changes = [dict(id=99), dict(expired=True), dict(expires_at="2000-01-01T00:00:00Z"),
                   dict(digest="sha256:" + "e" * 64), dict(workflow_run=dict(before["workflow_run"], id=98)),
                   dict(workflow_run=dict(before["workflow_run"], repository_id=99)),
                   dict(workflow_run=dict(before["workflow_run"], head_sha="f" * 40))]
        for change in changes:
            with self.subTest(change=change):
                self.fixture.artifacts[40] = dict(before, **change)
                self.reject("artifact")
        self.assertTrue(all("/actions/artifacts?" not in call.full_url for call in self.fixture.calls))

    def test_external_producers_must_cover_roles_exactly_and_bind_actual_job(self):
        before = copy.deepcopy(self.fixture.producers)
        for change in ("missing", "extra", "job", "id-bool", "duplicate-id", "bad-manifest"):
            with self.subTest(change=change):
                self.fixture.producers = copy.deepcopy(before)
                artifacts = self.fixture.producers["artifacts"]
                if change == "missing": del artifacts["linux"]
                elif change == "extra": artifacts["other"] = copy.deepcopy(artifacts["linux"])
                elif change == "job": artifacts["linux"]["job_id"] = 31
                elif change == "id-bool": artifacts["linux"]["artifact_id"] = True
                elif change == "duplicate-id": artifacts["linux"]["artifact_id"] = 41
                else: artifacts["linux"]["manifest_sha256"] = "not a digest"
                self.reject()

    def test_two_artifacts_from_one_authenticated_job_do_not_require_another_build(self):
        self.fixture.policy["artifact_roles"] = {"linux":"linux", "web":"web", "linux-u1":"linux"}
        second = dict(self.fixture.producers["artifacts"]["linux"], artifact_id=42,
                      artifact_digest="sha256:" + "9" * 64, manifest_sha256="8" * 64)
        self.fixture.producers["artifacts"]["linux-u1"] = second
        self.fixture.artifacts[42] = dict(self.fixture.artifacts[40], id=42, name="linux-u1", digest=second["artifact_digest"])
        report = self.verify()
        self.assertEqual(set(report["jobs"]), {"linux", "web"})
        self.assertEqual(set(report["artifacts"]), {"linux", "web", "linux-u1"})
        self.assertEqual(report["artifacts"]["linux"]["job_id"], report["artifacts"]["linux-u1"]["job_id"])
        self.assertNotEqual(report["artifacts"]["linux"]["artifact_id"], report["artifacts"]["linux-u1"]["artifact_id"])

    def test_artifact_roles_require_an_explicit_map_without_legacy_list_coercion(self):
        self.fixture.policy["artifact_roles"] = ["linux", "web"]
        self.reject("artifact roles")
        self.assertFalse(self.fixture.calls)

    def test_artifact_mapping_rejects_wrong_unknown_or_nonstring_job_role(self):
        for target in ("web", "unknown", True, None):
            with self.subTest(target=target):
                self.fixture.policy["artifact_roles"] = {"linux":target, "web":"web"}
                self.reject()

    def test_pr_merge_execution_binds_commit_parents_without_conflating_trigger_head(self):
        self.fixture.use_pr_merge()
        report = self.verify()
        self.assertNotEqual(report["expected"]["source_sha"], report["expected"]["trigger_head_sha"])
        self.assertEqual(report["source_association"]["merge_sha"], "c" * 40)

    def test_unrelated_or_stale_pr_merge_and_wrong_parent_are_rejected(self):
        self.fixture.use_pr_merge()
        original = copy.deepcopy(self.fixture.override)
        association = copy.deepcopy(self.fixture.run["pull_requests"])
        for change in ("head", "merge", "parent", "association", "event"):
            with self.subTest(change=change):
                self.fixture.override = copy.deepcopy(original)
                self.fixture.run.update(event="pull_request", pull_requests=copy.deepcopy(association))
                if change == "head": self.fixture.override["/pulls/7"]["head"]["sha"] = "e" * 40
                elif change == "merge": self.fixture.override["/pulls/7"]["merge_commit_sha"] = "e" * 40
                elif change == "parent": self.fixture.override["/git/commits/" + "c" * 40]["parents"][1]["sha"] = "e" * 40
                elif change == "association": self.fixture.run["pull_requests"] = []
                else: self.fixture.run["event"] = "pull_request_target"
                self.reject()

    def test_pr_repository_boolean_cannot_equal_a_numeric_identity(self):
        self.fixture.use_pr_merge()
        self.fixture.expected["repository_id"] = self.fixture.run["repository"]["id"] = 1
        self.fixture.attempts[1]["repository"]["id"] = 1
        for artifact in self.fixture.artifacts.values():
            artifact["workflow_run"]["repository_id"] = 1
        self.fixture.override["/pulls/7"]["base"]["repo"]["id"] = True
        self.reject("repository")
        self.assertTrue(self.fixture.calls[-1].full_url.endswith("/pulls/7"))

    def test_truncated_http_transfer_is_recorded_as_failure_without_retry(self):
        self.fixture.override["/actions/runs/12"] = IncompleteRead(b"partial response", 20)
        self.reject("transport")
        self.assertEqual(len(self.fixture.calls), 1)
        self.assertEqual((self.root / "1" / "0001.body").read_bytes(), b"partial response")

    def test_adapter_total_size_and_request_budgets_stop_without_an_extra_get(self):
        api = GitHubAPI("fixture-auth", self.root / "bounded", opener=self.fixture.opener)
        with mock.patch("verify_release_inputs.MAX_REQUESTS", 1):
            api.get("/repos/owner/engine/actions/runs/12")
            with self.assertRaisesRegex(HostedInputsError, "budget"):
                api.get("/repos/owner/engine/actions/runs/12")
        self.assertEqual(len(self.fixture.calls), 1)
        api = GitHubAPI("fixture-auth", self.root / "bounded-bytes", opener=self.fixture.opener)
        with mock.patch("verify_release_inputs.MAX_TOTAL_RESPONSE", 1):
            with self.assertRaisesRegex(HostedInputsError, "size limit"):
                api.get("/repos/owner/engine/actions/runs/12")
        self.assertEqual(len(self.fixture.calls), 2)

    def test_real_redirect_handler_cannot_forward_authorization_to_another_host(self):
        from verify_release_inputs import _NoRedirect
        from urllib.request import Request
        request = Request("https://api.github.com/repos/owner/engine", headers={"Authorization":"Bearer private-token"})
        self.assertIsNone(_NoRedirect().redirect_request(request, None, 302, "Found", {}, "https://other.invalid"))

    def test_missing_earlier_attempt_is_fatal_even_when_current_producers_are_green(self):
        self.fixture.add_prior_attempt()
        self.fixture.override["/actions/runs/12/attempts/1"] = HTTPError(
            "https://api.github.com", 404, "missing", {}, io.BytesIO(b'{}'))
        self.reject("HTTP 404")
        self.assertFalse(any("/actions/artifacts/" in call.full_url for call in self.fixture.calls))

    def test_page_two_count_drift_and_foreign_pagination_host_are_rejected(self):
        self.fixture.jobs[1].extend(dict(self.fixture.jobs[1][0], id=100 + i, name=f"Optional {i}") for i in range(101))
        page2 = "/actions/runs/12/attempts/1/jobs?per_page=100&page=2"
        self.fixture.override[page2] = dict(total_count=104, jobs=self.fixture.jobs[1][100:])
        self.reject("total changed")
        del self.fixture.override[page2]
        self.fixture.override["/actions/runs/12/attempts/1/jobs?per_page=100&page=1"] = lambda: Response(
            dict(total_count=103, jobs=self.fixture.jobs[1][:100]),
            {"Link":'<https://untrusted.invalid/?page=2>; rel="next"'})
        self.reject("destination")
        self.assertTrue(all(urlsplit(call.full_url).netloc == "api.github.com" for call in self.fixture.calls))

    def test_duplicate_json_keys_arrays_and_nonfinite_numbers_fail_before_identity(self):
        for raw in (b'{"id":12,"id":12}', b'[]', b'{"id":NaN}'):
            with self.subTest(raw=raw):
                self.fixture.override["/actions/runs/12"] = lambda raw=raw: Response(raw)
                self.reject()

    def test_existing_snapshot_directory_is_never_reused_or_overwritten(self):
        api = GitHubAPI("fixture-auth", self.root / "occupied", opener=self.fixture.opener)
        marker = api.directory / "previous-attempt.log"
        marker.write_bytes(b"keep the first failure")
        with self.assertRaises(HostedInputsError):
            GitHubAPI("fixture-auth", api.directory, opener=self.fixture.opener)
        self.assertEqual(marker.read_bytes(), b"keep the first failure")

    def test_current_attempt_id_limit_and_incomplete_prior_attempt_are_rejected(self):
        self.fixture.expected["run_attempt"] = 21
        self.reject()
        self.assertFalse(self.fixture.calls)
        self.fixture.expected["run_attempt"] = 1
        self.fixture.add_prior_attempt()
        self.fixture.attempts[1]["status"] = "in_progress"
        self.reject("prior attempt")

    def test_pr_change_during_verification_is_rejected_on_final_readback(self):
        self.fixture.use_pr_merge()
        before = copy.deepcopy(self.fixture.override["/pulls/7"])
        calls = 0
        def change_after_first():
            nonlocal calls
            calls += 1
            return Response(before if calls == 1 else dict(before, merge_commit_sha="e" * 40))
        self.fixture.override["/pulls/7"] = change_after_first
        self.reject("merge source")

    def test_expected_ids_and_policy_ambiguity_fail_before_network(self):
        for key in ("repository_id", "run_id", "run_attempt"):
            with self.subTest(key=key):
                before = self.fixture.expected[key]
                self.fixture.expected[key] = True
                self.reject()
                self.fixture.expected[key] = before
        self.assertFalse(self.fixture.calls)
        self.fixture.policy["required_jobs"]["web"] = self.fixture.policy["required_jobs"]["linux"]
        self.reject()
        self.assertFalse(self.fixture.calls)

    def test_missing_prior_attempt_http_error_and_malformed_json_are_not_retried(self):
        endpoint = "/actions/runs/12"
        for value in (HTTPError("https://api.github.com", 403, "forbidden", {}, io.BytesIO(b'{"message":"denied"}')),
                      URLError("connection lost"), lambda: Response(b'{"id":')):
            with self.subTest(value=type(value).__name__):
                self.fixture.override[endpoint] = value
                before = len(self.fixture.calls)
                self.reject()
                self.assertEqual(len(self.fixture.calls), before + 1)

    def test_raw_snapshots_have_hashes_and_never_persist_auth(self):
        self.verify()
        directory = self.root / "1"
        records = list(directory.glob("*.response.json"))
        self.assertGreaterEqual(len(records), 6)
        for metadata_path in records:
            metadata = json.loads(metadata_path.read_text())
            body = (directory / metadata["body_path"]).read_bytes()
            self.assertEqual(hashlib.sha256(body).hexdigest(), metadata["body_sha256"])
            self.assertEqual(metadata["method"], "GET")
        for path in directory.iterdir():
            self.assertNotIn(b"fixture-token-must-never-be-persisted", path.read_bytes())
        self.assertTrue(all(call.get_header("Authorization") == "Bearer fixture-token-must-never-be-persisted"
                            for call in self.fixture.calls))

    def test_response_bounds_redirect_and_token_echo_fail_closed_without_secret_log(self):
        for name, value in (("oversize", lambda: Response(b"x" * (8 * 1024 * 1024 + 1))),
                            ("redirect", lambda: Response(b"", {"Location":"https://elsewhere.invalid"}, 302)),
                            ("echo", lambda: Response(b'{"message":"fixture-token-must-never-be-persisted"}'))):
            with self.subTest(name=name):
                self.fixture.override["/actions/runs/12"] = value
                self.reject()
                for path in (self.root / str(self.counter)).iterdir():
                    self.assertNotIn(b"fixture-token-must-never-be-persisted", path.read_bytes())

    def wire_run_response(self, headers, payload, *, accepted=False):
        """A real HTTPResponse at the injected network boundary, without auth forwarding."""
        observed = []
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass
            def do_GET(self):
                observed.append(self.headers.get("Authorization"))
                self.send_response(200)
                for name, value in headers:
                    self.send_header(name, value)
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(payload)
                self.close_connection = True
        with HTTPServer(("127.0.0.1", 0), Handler) as server:
            worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval":0.01})
            worker.start()
            self.fixture.override["/actions/runs/12"] = lambda: urlopen(
                f"http://127.0.0.1:{server.server_port}/run", timeout=3)
            try:
                result = self.verify() if accepted else self.reject()
            finally:
                server.shutdown()
                worker.join(timeout=3)
                self.assertFalse(worker.is_alive())
        self.assertEqual(observed, [None, None] if accepted else [None], "No retry or forwarded Authorization")
        return result

    def test_real_http_complete_json_shorter_than_declared_body_is_rejected(self):
        body = json.dumps(self.fixture.run).encode()
        self.wire_run_response([("Content-Length", str(len(body) + 37))], body)
        metadata = json.loads((self.root / "1" / "0001.response.json").read_text())
        self.assertEqual(metadata["headers"]["content-length"], str(len(body) + 37))
        self.assertEqual(metadata["received_size"], len(body))
        self.assertEqual((self.root / "1" / "0001.body").read_bytes(), body)

    def test_real_http_exact_content_length_accepts_metadata_as_fixture_only(self):
        body = json.dumps(self.fixture.run).encode()
        result = self.wire_run_response([("Content-Length", str(len(body)))], body, accepted=True)
        self.assertEqual(result["status"], "HOSTED_INPUTS_VERIFIED")
        self.assertEqual(result["transport"], "fixture")
        self.assertFalse(result["release_ready"])

    def test_real_http_invalid_duplicate_or_ambiguous_content_length_is_rejected(self):
        body = json.dumps(self.fixture.run).encode()
        for headers in ([('Content-Length', '-1')], [('Content-Length', 'unknown')],
                        [('Content-Length', str(len(body))), ('Content-Length', str(len(body)))],
                        [('Content-Length', str(len(body))), ('Transfer-Encoding', 'chunked')]):
            with self.subTest(headers=headers):
                payload = body if headers[-1][0] != 'Transfer-Encoding' else f'{len(body):x}\r\n'.encode() + body + b'\r\n0\r\n\r\n'
                self.wire_run_response(headers, payload)

    def test_real_http_chunked_body_requires_terminal_chunk(self):
        body = json.dumps(self.fixture.run).encode()
        chunk = f'{len(body):x}\r\n'.encode() + body + b'\r\n'
        self.wire_run_response([('Transfer-Encoding', 'chunked')], chunk)
        self.wire_run_response([('Transfer-Encoding', 'chunked')], chunk + b'0\r\n\r\n', accepted=True)

    def test_real_http_transfer_encoding_must_activate_the_actual_chunk_parser(self):
        body = json.dumps(self.fixture.run).encode()
        for transfer in ('chunked ', 'chunked\t'):
            with self.subTest(transfer=transfer):
                # HTTPResponse does not activate chunk parsing for these values.
                # A declared chunked body cannot be accepted as raw JSON at EOF.
                self.wire_run_response([('Transfer-Encoding', transfer)], body)

    def test_cli_rejects_unlocked_policy_and_fixture_dry_run_never_returns_real_success(self):
        paths = {}
        for name in ("expected", "policy", "producers"):
            paths[name] = self.root / (name + ".json")
            paths[name].write_text(json.dumps(getattr(self.fixture, name)), encoding="utf-8")
        args = ["--expected", str(paths["expected"]), "--policy", str(paths["policy"]),
                "--producers", str(paths["producers"]), "--output-dir", str(self.root / "cli"),
                "--policy-sha256", "f" * 64]
        with mock.patch.dict(os.environ, {"GH_TOKEN":"fixture-auth"}), mock.patch("verify_release_inputs.GitHubAPI") as api_class:
            self.assertEqual(main(args), 1)
            api_class.assert_not_called()
        args[-1] = hashlib.sha256(paths["policy"].read_bytes()).hexdigest()
        api = GitHubAPI("fixture-auth", self.root / "cli-fixture", opener=self.fixture.opener)
        with mock.patch.dict(os.environ, {"GH_TOKEN":"fixture-auth"}), mock.patch("verify_release_inputs.GitHubAPI", return_value=api):
            self.assertEqual(main(args), 77)


if __name__ == "__main__":
    unittest.main()
