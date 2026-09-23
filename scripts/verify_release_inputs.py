#!/usr/bin/env python3
"""Authenticate hosted metadata from externally locked inputs using GET only.

This layer does not download or validate payloads, parse test reports, check
versions, or grant publication approval. The next layer must reuse
verify_release_candidate.verify_evidence with separately trusted U1 receipts,
then verify U22 final-package receipts and rehash exact publication inputs.
"""
from __future__ import annotations

import argparse
import copy
from datetime import datetime, timezone
import hashlib
from http.client import HTTPException
import json
import os
from pathlib import Path
import re
import sys
from urllib.error import HTTPError, URLError
from urllib.parse import parse_qs, urlsplit
from urllib.request import HTTPRedirectHandler, Request, build_opener


API_ROOT = "https://api.github.com"
MAX_RESPONSE = 8 * 1024 * 1024
MAX_TOTAL_RESPONSE = 64 * 1024 * 1024
MAX_REQUESTS = 512
MAX_INPUT = 1024 * 1024
MAX_ATTEMPTS = 20
MAX_JOBS = 10000
PER_PAGE = 100
SHA = re.compile(r"[0-9a-f]{40}\Z")
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
REPOSITORY = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
WORKFLOW = re.compile(r"\.github/workflows/[A-Za-z0-9_-]+\.ya?ml\Z")


class HostedInputsError(RuntimeError):
    """Missing, inconsistent or unverifiable hosted input; never a skip/pass."""


def _need(condition, message):
    if not condition:
        raise HostedInputsError(message)


def _integer(value, name, *, maximum=None, minimum=1):
    _need(type(value) is int and value >= minimum and (maximum is None or value <= maximum),
          f"Invalid {name}: expected integer in allowed range")
    return value


def _object(value, name):
    _need(isinstance(value, dict), f"Invalid {name}: expected an object")
    return value


def _matches(value, pattern):
    return isinstance(value, str) and pattern.fullmatch(value) is not None


def _json(raw):
    def pairs(items):
        result = {}
        for key, value in items:
            _need(key not in result, "Duplicate JSON key")
            result[key] = value
        return result
    def constant(_):
        raise HostedInputsError("Non-finite JSON number")
    try:
        return json.loads(raw, object_pairs_hook=pairs, parse_constant=constant)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise HostedInputsError("Malformed API/input JSON") from error


def _sha(raw):
    return hashlib.sha256(raw).hexdigest()


def _new_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


class _NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def _framing_error(fields, received_size, chunked_decoded):
    lengths = fields.get("content-length", [])
    transfers = fields.get("transfer-encoding", [])
    if len(lengths) > 1 or (lengths and transfers):
        return "Ambiguous HTTP body framing"
    if transfers and (len(transfers) != 1 or transfers[0].lower() != "chunked" or chunked_decoded is not True):
        return "HTTP transfer encoding did not activate the chunk parser"
    if lengths:
        value = lengths[0].strip()
        if not re.fullmatch(r"[0-9]{1,10}", value):
            return "Invalid HTTP Content-Length"
        declared = int(value)
        if declared > MAX_RESPONSE:
            return "HTTP Content-Length exceeds response size limit"
        if declared != received_size:
            return "Incomplete HTTP body: Content-Length differs from received bytes"
    return None


class GitHubAPI:
    """Bounded HTTPS GET adapter with immutable raw snapshots, without retries.

    Injecting an opener marks every result as fixture evidence. There is no CLI
    switch to promote fixture responses or choose another host. Authorization
    is kept only in the outbound request, never in snapshot request metadata.
    """
    def __init__(self, token, evidence_dir, *, opener=None):
        _need(isinstance(token, str) and token and not any(char in token for char in "\r\n\0"),
              "A nonempty authentication token is required")
        given = Path(evidence_dir).absolute()
        self.directory = given.parent.resolve(strict=True) / given.name
        _need(not self.directory.exists() and not self.directory.is_symlink(), "Evidence directory must be new")
        self.directory.mkdir()
        self.transport = "github" if opener is None else "fixture"
        self._open = build_opener(_NoRedirect()).open if opener is None else opener
        self._token = token
        self._sequence = 0
        self._bytes = 0
        self.snapshots = []

    def _redact(self, value):
        return value.replace(self._token, "<redacted-auth-token>")

    def get(self, endpoint):
        _need(self._sequence < MAX_REQUESTS and self._bytes < MAX_TOTAL_RESPONSE, "API evidence request/size budget exhausted")
        _need(isinstance(endpoint, str) and endpoint.startswith("/repos/")
              and not any(char in endpoint for char in "\r\n\0#") and ".." not in endpoint,
              "Invalid GitHub API endpoint")
        url = API_ROOT + endpoint
        request = Request(url, headers={"Accept":"application/vnd.github+json",
            "Authorization":"Bearer " + self._token, "X-GitHub-Api-Version":"2022-11-28",
            "User-Agent":"caesura-hosted-input-verifier"}, method="GET")
        self._sequence += 1
        stem = f"{self._sequence:04d}"
        status, raw, headers, transport_error = None, b"", {}, None
        framing_headers = {}
        chunked_decoded = False
        try:
            try:
                response = self._open(request, timeout=30)
            except HTTPError as error:
                response = error
            with response:
                status = getattr(response, "status", getattr(response, "code", None))
                chunked_decoded = getattr(response, "chunked", False)
                for name, value in response.headers.items():
                    name = name.lower()
                    if name in {"content-length", "transfer-encoding"}:
                        framing_headers.setdefault(name, []).append(value)
                    if name in {"link", "x-github-request-id", "content-type", "content-length", "transfer-encoding"}:
                        headers[name] = value
                # read(amt) may return a complete JSON document on premature
                # EOF without raising IncompleteRead. Check declared framing
                # independently of JSON validity; chunk terminators are also
                # checked by the real HTTPResponse parser during this read.
                raw = response.read(MAX_RESPONSE + 1)
        except (OSError, URLError, TimeoutError, HTTPException) as error:
            # Exception messages and arbitrary response headers may contain
            # credentials. Preserve only the failure class, not their text.
            transport_error = type(error).__name__
            partial = getattr(error, "partial", None)
            if isinstance(partial, bytes):
                raw = partial[:MAX_RESPONSE + 1]
        observed_size = len(raw)
        framing_error = _framing_error(framing_headers, observed_size, chunked_decoded)
        self._bytes += observed_size
        truncated = len(raw) > MAX_RESPONSE
        raw = raw[:MAX_RESPONSE]
        sensitive = self._token.encode() in raw or self._token in json.dumps(headers)
        body = raw.replace(self._token.encode(), b"<redacted-auth-token>")
        body_name = stem + ".body"
        with (self.directory / body_name).open("xb") as stream:
            stream.write(body)
        metadata = dict(method="GET", url=url, status=status, transport=self.transport,
            recorded_at=datetime.now(timezone.utc).isoformat(), headers=headers,
            body_path=body_name, body_sha256=_sha(body), observed_body_sha256=_sha(raw),
            body_size=len(body), received_size=observed_size,
            framing_headers=framing_headers, chunked_decoded=chunked_decoded, framing_error=framing_error,
            truncated=truncated, redacted=sensitive, transport_error=transport_error)
        metadata = json.loads(self._redact(json.dumps(metadata)))
        _new_json(self.directory / (stem + ".response.json"), metadata)
        self.snapshots.append(metadata)
        _need(transport_error is None, "GitHub API transport failed; no retry performed")
        _need(status == 200, f"GitHub API returned HTTP {status}; no retry performed")
        _need(not truncated, "GitHub API response exceeds size limit")
        _need(self._bytes <= MAX_TOTAL_RESPONSE, "GitHub API evidence exceeds total size limit")
        _need(not sensitive, "GitHub API response contained authentication data; snapshot redacted")
        _need(framing_error is None, framing_error)
        return _object(_json(body), "API response"), headers

    def finish(self, report):
        report["snapshots"] = copy.deepcopy(self.snapshots)
        safe_report = json.loads(self._redact(json.dumps(report)))
        _new_json(self.directory / "summary.json", safe_report)


def _validate_inputs(expected, policy, producers):
    for name, value in (("expected", expected), ("policy", policy), ("producers", producers)):
        _object(value, name)
        _need(type(value.get("schema_version")) is int and value["schema_version"] == 1, f"Invalid {name} schema")
    _need(_matches(expected.get("repository"), REPOSITORY), "Invalid expected repository")
    for name in ("repository_id", "run_id"):
        _integer(expected.get(name), name)
    _integer(expected.get("run_attempt"), "run_attempt", maximum=MAX_ATTEMPTS)
    for name in ("source_sha", "trigger_head_sha", "called_workflow_sha"):
        _need(_matches(expected.get(name), SHA), f"Invalid expected {name}")
    for name in ("workflow_path", "called_workflow_path"):
        _need(_matches(expected.get(name), WORKFLOW), f"Invalid expected {name}")
    mode = expected.get("source_mode")
    _need(mode in ("head", "pull-request-merge"), "Explicit source_mode is required")
    if mode == "head":
        _need(expected["source_sha"] == expected["trigger_head_sha"], "Head execution source differs from trigger head")
    else:
        _integer(expected.get("pull_request_number"), "pull_request_number")
    jobs = _object(policy.get("required_jobs"), "required_jobs")
    _need(0 < len(jobs) <= MAX_JOBS, "Policy must predeclare required jobs")
    _need(all(_matches(role, re.compile(r"[a-z][a-z0-9_-]{0,99}\Z")) for role in jobs), "Invalid required role")
    _need(all(isinstance(name, str) and 0 < len(name) <= 512 and not any(ord(c) < 32 for c in name)
              for name in jobs.values()), "Invalid required job name")
    _need(len(set(jobs.values())) == len(jobs), "Required roles must map to distinct exact job names")
    roles = policy.get("artifact_roles")
    _need(isinstance(roles, dict)
          and all(_matches(role, re.compile(r"[a-z][a-z0-9_-]{0,99}\Z")) for role in roles)
          and all(isinstance(job_role, str) and job_role in jobs for job_role in roles.values()),
          "Invalid policy artifact roles: expected artifact-role to required-job-role map")
    artifacts = _object(producers.get("artifacts"), "producer artifacts")
    _need(set(artifacts) == set(roles), "External producer roles must exactly match policy artifact roles")
    ids = set()
    for role, entry in artifacts.items():
        _object(entry, "producer " + role)
        artifact_id = _integer(entry.get("artifact_id"), "producer artifact_id")
        _need(artifact_id not in ids, "Duplicate producer artifact ID")
        ids.add(artifact_id)
        _integer(entry.get("job_id"), "producer job_id")
        _integer(entry.get("run_attempt"), "producer run_attempt", maximum=MAX_ATTEMPTS)
        _need(entry["run_attempt"] == expected["run_attempt"], "External producer belongs to another attempt")
        _need(_matches(entry.get("artifact_digest"), re.compile(r"sha256:[0-9a-f]{64}\Z")), "Invalid producer artifact digest")
        _need(_matches(entry.get("manifest_sha256"), DIGEST), "Invalid producer manifest digest")


def _run_identity(run, expected, attempt, workflow_id=None):
    for field, value in (("id", expected["run_id"]), ("run_attempt", attempt)):
        _integer(run.get(field), "API run " + field)
        _need(run[field] == value, f"Wrong API run {field}/attempt identity")
    repository = _object(run.get("repository"), "run repository")
    _integer(repository.get("id"), "API repository_id")
    _need(repository["id"] == expected["repository_id"] and repository.get("full_name") == expected["repository"],
          "Wrong run repository identity")
    _need(run.get("head_sha") == expected["trigger_head_sha"], "Wrong run trigger source SHA")
    _need(run.get("path") == expected["workflow_path"], "Wrong run workflow path")
    identifier = _integer(run.get("workflow_id"), "workflow_id")
    _need(workflow_id is None or identifier == workflow_id, "Run workflow ID changed")
    _integer(_object(run.get("head_repository"), "head repository").get("id"), "head repository ID")
    _need(run.get("status") in ("in_progress", "completed"), "Run is not executing or completed")
    references = run.get("referenced_workflows")
    _need(isinstance(references, list) and all(isinstance(item, dict) for item in references), "Missing called workflow identity")
    target = expected["repository"] + "/" + expected["called_workflow_path"]
    selected = [item for item in references if isinstance(item.get("path"), str)
                and item["path"].split("@", 1)[0] == target]
    _need(len(selected) == 1 and selected[0].get("sha") == expected["called_workflow_sha"], "Wrong or ambiguous called workflow SHA/path")
    return identifier


def _source_association(api, base, run, expected):
    if expected["source_mode"] == "head":
        return dict(mode="head", source_sha=expected["source_sha"])
    number = expected["pull_request_number"]
    _need(run.get("event") == "pull_request", "Merge source requires a pull_request run")
    associated = run.get("pull_requests")
    _need(isinstance(associated, list), "Missing run PR association")
    associated = [item for item in associated if isinstance(item, dict) and type(item.get("number")) is int and item["number"] == number]
    _need(len(associated) == 1, "Run is not uniquely associated with the expected PR")
    pr, _ = api.get(f"{base}/pulls/{number}")
    _need(type(pr.get("number")) is int and pr["number"] == number, "Wrong PR number")
    head, target = _object(pr.get("head"), "PR head"), _object(pr.get("base"), "PR base")
    _need(head.get("sha") == expected["trigger_head_sha"], "PR head changed after the selected run")
    _need(pr.get("merge_commit_sha") == expected["source_sha"], "PR merge source changed or is unrelated")
    _need(_matches(target.get("sha"), SHA), "Invalid PR base SHA")
    base_id = _integer(_object(target.get("repo"), "PR base repository").get("id"), "PR base repository ID")
    head_id = _integer(_object(head.get("repo"), "PR head repository").get("id"), "PR head repository ID")
    _need(base_id == expected["repository_id"], "Wrong PR base repository")
    _need(head_id == run["head_repository"]["id"], "Wrong PR head repository")
    link_head, link_base = _object(associated[0].get("head"), "run PR head"), _object(associated[0].get("base"), "run PR base")
    _need(link_head.get("sha") == head["sha"] and link_base.get("sha") == target["sha"], "Run PR association changed")
    commit, _ = api.get(f'{base}/git/commits/{expected["source_sha"]}')
    parents = commit.get("parents")
    _need(commit.get("sha") == expected["source_sha"] and isinstance(parents, list)
          and all(isinstance(item, dict) for item in parents)
          and [item.get("sha") for item in parents] == [target["sha"], head["sha"]], "Merge commit parents do not bind the PR base and head")
    return dict(mode="pull-request-merge", number=number, merge_sha=commit["sha"],
                base_sha=target["sha"], head_sha=head["sha"])


def _next_link(headers):
    link = headers.get("link", "")
    if not link:
        return None
    matches = re.findall(r'<([^<>]+)>;\s*rel="([a-z]+)"', link)
    _need(matches and len(matches) == len(link.split(",")), "Malformed pagination Link header")
    next_urls = [url for url, relation in matches if relation == "next"]
    _need(len(next_urls) <= 1, "Duplicate pagination next link")
    return next_urls[0] if next_urls else None


def _jobs(api, base, expected, attempt):
    endpoint = f'{base}/actions/runs/{expected["run_id"]}/attempts/{attempt}/jobs'
    result, identifiers, total, page = [], set(), None, 1
    while True:
        data, headers = api.get(f"{endpoint}?per_page={PER_PAGE}&page={page}")
        count = _integer(data.get("total_count"), "jobs total_count", maximum=MAX_JOBS, minimum=0)
        _need(total is None or total == count, "Jobs pagination total changed")
        total = count
        jobs = data.get("jobs")
        _need(isinstance(jobs, list) and len(jobs) == min(PER_PAGE, total - len(result)), "Incomplete or oversized jobs pagination page")
        for job in jobs:
            _object(job, "job")
            identifier = _integer(job.get("id"), "job ID")
            _need(identifier not in identifiers, "Duplicate job ID/page")
            identifiers.add(identifier)
            for field, wanted in (("run_id", expected["run_id"]), ("run_attempt", attempt)):
                _integer(job.get(field), "job " + field)
                _need(job[field] == wanted, "Job belongs to another run/attempt")
            _need(job.get("head_sha") == expected["trigger_head_sha"], "Job trigger SHA differs")
            result.append(job)
        next_url = _next_link(headers)
        if len(result) == total:
            _need(next_url is None, "Unexpected extra jobs pagination page")
            return result
        _need(next_url is not None, "Missing jobs pagination next link")
        parsed = urlsplit(next_url)
        _need(parsed.scheme == "https" and parsed.netloc == "api.github.com"
              and parsed.path == endpoint and not parsed.fragment
              and parse_qs(parsed.query) == {"per_page":[str(PER_PAGE)], "page":[str(page + 1)]},
              "Invalid jobs pagination next destination")
        page += 1
        _need(page <= MAX_JOBS // PER_PAGE, "Jobs pagination exceeds page limit")


def _artifact(data, entry, expected, run, role):
    _integer(data.get("id"), "artifact ID")
    _need(data["id"] == entry["artifact_id"], f"Wrong artifact ID for {role}")
    _need(data.get("expired") is False, f"Expired or unknown artifact expiry for {role}")
    try:
        expiry = datetime.fromisoformat(data.get("expires_at", "").replace("Z", "+00:00"))
        _need(expiry.tzinfo is not None and expiry > datetime.now(timezone.utc), f"Expired artifact for {role}")
    except (TypeError, ValueError, AttributeError) as error:
        raise HostedInputsError(f"Invalid artifact expiry for {role}") from error
    _need(data.get("digest") == entry["artifact_digest"], f"Wrong artifact transport digest for {role}")
    origin = _object(data.get("workflow_run"), "artifact workflow_run")
    for field, wanted in (("id", expected["run_id"]), ("repository_id", expected["repository_id"]),
                          ("head_repository_id", run["head_repository"]["id"])):
        _integer(origin.get(field), "artifact " + field)
        _need(origin[field] == wanted, f"Wrong artifact {field} for {role}")
    _need(origin.get("head_sha") == expected["trigger_head_sha"], f"Wrong artifact trigger head for {role}")
    return dict(entry, name=data.get("name"), expires_at=data["expires_at"])


def verify_hosted_inputs(expected, policy, producers, api):
    """Verify trusted caller inputs; artifact contents cannot choose this policy.

    CLI locks policy file bytes with --policy-sha256. Library callers must lock
    this policy before fanout and transfer producer outputs through controlled
    workflow outputs; this function cannot authenticate arbitrary local files.
    A manifest digest here is a trusted selection, not proof of downloaded bytes.
    """
    report = dict(schema_version=1, kind="caesura.hosted-inputs.v1", status="FAIL",
        transport=api.transport, release_ready=False, expected=copy.deepcopy(expected),
        policy=copy.deepcopy(policy), attempts=[], jobs={}, artifacts={}, errors=[],
        unverified={name:"NOT_VERIFIED" for name in ("package_payloads", "execution_evidence",
            "versions", "preupload_bytes", "branch_protection")})
    try:
        _validate_inputs(expected, policy, producers)
        base = "/repos/" + expected["repository"]
        run, _ = api.get(f'{base}/actions/runs/{expected["run_id"]}')
        workflow_id = _run_identity(run, expected, expected["run_attempt"])
        report["source_association"] = _source_association(api, base, run, expected)
        workflow, _ = api.get(f"{base}/actions/workflows/{workflow_id}")
        _integer(workflow.get("id"), "API workflow ID")
        _need(workflow["id"] == workflow_id and workflow.get("path") == expected["workflow_path"], "Wrong workflow endpoint identity")
        failures = []
        for attempt in range(1, expected["run_attempt"] + 1):
            observed, _ = api.get(f'{base}/actions/runs/{expected["run_id"]}/attempts/{attempt}')
            _run_identity(observed, expected, attempt, workflow_id)
            _need(observed["head_repository"]["id"] == run["head_repository"]["id"], "Attempt head repository changed")
            _need(observed.get("event") == run.get("event"), "Attempt trigger event changed")
            _need(attempt == expected["run_attempt"] or observed.get("status") == "completed", "Incomplete prior attempt")
            all_jobs = _jobs(api, base, expected, attempt)
            selected = {}
            for role, name in policy["required_jobs"].items():
                matches = [job for job in all_jobs if job.get("name") == name]
                if len(matches) != 1:
                    failures.append(f"Required role {role} in attempt {attempt} needs exactly one job named {name}")
                    continue
                job = matches[0]
                selected[role] = {key:job.get(key) for key in ("id", "name", "run_id", "run_attempt", "head_sha", "status", "conclusion")}
                if job.get("status") != "completed" or job.get("conclusion") != "success":
                    failures.append(f'Required role {role} in attempt {attempt}: {job.get("status")}/{job.get("conclusion")}')
            report["attempts"].append(dict(attempt=attempt, status=observed.get("status"),
                conclusion=observed.get("conclusion"), job_count=len(all_jobs), jobs=selected))
        _need(not failures, "; ".join(failures))
        report["jobs"] = copy.deepcopy(report["attempts"][-1]["jobs"])
        for role, entry in producers["artifacts"].items():
            job_role = policy["artifact_roles"][role]
            _need(entry["job_id"] == report["jobs"][job_role]["id"], f"External producer job does not match mapped required role {role}/{job_role}")
            data, _ = api.get(f'{base}/actions/artifacts/{entry["artifact_id"]}')
            report["artifacts"][role] = _artifact(data, entry, expected, run, role)
        final_run, _ = api.get(f'{base}/actions/runs/{expected["run_id"]}')
        _run_identity(final_run, expected, expected["run_attempt"], workflow_id)
        _need(final_run["head_repository"]["id"] == run["head_repository"]["id"], "Run head repository changed")
        _need(_source_association(api, base, final_run, expected) == report["source_association"], "Source association changed")
        report.update(status="HOSTED_INPUTS_VERIFIED", verified_at=datetime.now(timezone.utc).isoformat())
        return report
    except HostedInputsError as error:
        report["errors"].append(str(error))
        raise
    finally:
        api.finish(report)


def _input(path):
    with path.open("rb") as stream:
        raw = stream.read(MAX_INPUT + 1)
    _need(len(raw) <= MAX_INPUT, "Input exceeds size limit")
    return _object(_json(raw), "input"), _sha(raw)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expected", type=Path, required=True, help="External expected hosted identity JSON")
    parser.add_argument("--policy", type=Path, required=True, help="Policy frozen before producers execute")
    parser.add_argument("--policy-sha256", required=True, help="Separately trusted policy file digest")
    parser.add_argument("--producers", type=Path, required=True, help="Controlled producer job outputs, not artifact contents")
    parser.add_argument("--output-dir", type=Path, required=True, help="New directory for API snapshots and summary")
    parser.add_argument("--token-env", default="GH_TOKEN", help="Environment variable containing a read-only GitHub token")
    args = parser.parse_args(argv)
    try:
        expected, expected_sha = _input(args.expected)
        policy, policy_sha = _input(args.policy)
        producers, producer_sha = _input(args.producers)
        _need(_matches(args.policy_sha256, DIGEST) and policy_sha == args.policy_sha256,
              "Policy file differs from externally locked digest")
        _validate_inputs(expected, policy, producers)
        api = GitHubAPI(os.environ.get(args.token_env), args.output_dir)
        _new_json(api.directory / "input-digests.json", dict(expected_sha256=expected_sha,
                  policy_sha256=policy_sha, producers_sha256=producer_sha))
        report = verify_hosted_inputs(expected, policy, producers, api)
        print(f'HOSTED INPUTS {report["status"]}: transport={report["transport"]}')
        print("Scope: hosted metadata only. Payloads, U1/U22 evidence, versions and preupload bytes remain NOT_VERIFIED.")
        return 0 if report["transport"] == "github" else 77
    except (HostedInputsError, OSError) as error:
        print(f"HOSTED INPUTS FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
