#!/usr/bin/env python3
"""Bind a single-tar Pages payload to an already verified Web proof bundle.

The caller supplies an in-memory verify_bundle result and an independently
selected manifest digest. This module does not authenticate GitHub metadata or
accept arbitrary saved results as provenance, and never authorizes deployment.
"""
from __future__ import annotations

import copy
import json
import os
from pathlib import Path
import re
import stat
import tarfile

from ci_package_lane import _new_work
from package_verification import (MAX_ENTRIES, _component, _sha256_file,
                                  _signature, prepare_package, verify_stable)
from verify_execution_bundle import _no_links
from verify_package_bundle import SCHEMA as BUNDLE_SCHEMA, verify_bundle_stable

SCHEMA = 'caesura.pages-artifact-verification.v1'
# The pinned official upload-pages-artifact README requires tar size < 10GB.
# https://github.com/actions/upload-pages-artifact/blob/56afc609e74202658d3ffba0e8f6dda462b719fa/README.md
# 1GiB is a recommendation, not this acceptance threshold.
MAX_PAGES_BYTES = 10_000_000_000


class PagesArtifactError(ValueError):
    pass


def _need(condition, message):
    if not condition:
        raise PagesArtifactError(message)


def _tar_name(value):
    _need(type(value) is str and '/' not in value and '\\' not in value,
          'Pages tar name must be one explicit filename')
    _component(value)
    return value


def _payload(root, name):
    root = _no_links(root)
    _need(root.is_dir(), 'Pages payload root must be a directory')
    entries = list(root.iterdir())
    _need(len(entries) == 1 and entries[0].name == name,
          'Pages payload must contain exactly the selected tar file')
    path = _no_links(entries[0])
    info = path.lstat()
    _need(stat.S_ISREG(info.st_mode) and info.st_nlink == 1,
          'Pages tar must be an ordinary file without hardlinks')
    return root, path


def _binding(package, manifest_sha256, name):
    _need(type(package) is dict and package.get('schema') == BUNDLE_SCHEMA
          and package.get('status') == 'BUNDLE_VERIFIED' and package.get('platform') == 'web'
          and package.get('release_ready') is False, 'A verified Web package result is required')
    _need(type(manifest_sha256) is str and re.fullmatch('[0-9a-f]{64}', manifest_sha256),
          'Independent Pages manifest SHA256 required')
    verify_bundle_stable(package)
    manifest = package['manifest']
    _need(manifest.get('name') == 'upload-manifest.json' and manifest.get('kind') == 'file'
          and manifest.get('sha256') == manifest_sha256,
          'Pages manifest digest differs from the verified Web proof')
    _need(sum(lock == manifest for lock in package['locks']) == 1,
          'Pages proof manifest must have one exact lock')
    matches = [item for item in package['files'] if item.get('name') == name]
    _need(len(matches) == 1 and matches[0].get('kind') == 'file',
          'Pages tar must be exactly one verified final file')
    final = matches[0]
    _need(sum(lock == final for lock in package['locks']) == 1,
          'Pages final tar must have one matching verified byte lock')
    return final, manifest


def _plain_tar(path, expected):
    """Apply Pages' strict member subset before shared safe extraction."""
    _need(_sha256_file(path) == expected, 'Pages tar bytes/digest changed before inspection')
    before = path.lstat()
    _need(0 < before.st_size < MAX_PAGES_BYTES, 'Pages tar must be smaller than 10GB')
    try:
        with path.open('rb') as stream:
            _need(_signature(os.fstat(stream.fileno())) == _signature(before),
                  'Pages tar changed while opening')
            with tarfile.open(fileobj=stream, mode='r:') as archive:
                total = 0
                for count, member in enumerate(archive, 1):
                    _need(count <= MAX_ENTRIES, 'Pages tar member count exceeds limit')
                    _need(member.type in (tarfile.REGTYPE, tarfile.AREGTYPE, tarfile.DIRTYPE)
                          and member.sparse is None
                          and not any(key.startswith('GNU.sparse') for key in member.pax_headers),
                          'Pages tar rejects all links, sparse and special members')
                    _need(0 <= member.size < MAX_PAGES_BYTES, 'Pages tar member exceeds size limit')
                    total += member.size
                    _need(total < MAX_PAGES_BYTES, 'Pages tar members exceed total size limit')
            _need(_signature(os.fstat(stream.fileno())) == _signature(before),
                  'Pages tar changed during member inspection')
    except (tarfile.TarError, EOFError) as error:
        raise PagesArtifactError('Pages payload must be a plain uncompressed tar') from error
    _need(_signature(path.lstat()) == _signature(before) and _sha256_file(path) == expected,
          'Pages tar bytes changed during inspection')


def verify_pages_artifact_stable(result):
    """Reopen both tar copies, the exact payload layout and all original proof locks."""
    _need(type(result) is dict and result.get('schema') == SCHEMA
          and result.get('status') == 'PAGES_ARTIFACT_VERIFIED'
          and result.get('release_ready') is False, 'Verified Pages result required')
    name = _tar_name(result['tar_name'])
    final, manifest = _binding(result['package_result'], result['manifest_sha256'], name)
    root, tar = _payload(result['payload_root'], name)
    digest = result['tar_sha256']
    expected_locks = [dict(path=str(tar), name=name, kind='file', sha256=digest), final, manifest]
    _need(result['locks'] == expected_locks and final['sha256'] == digest,
          'Pages fixed input selections changed')
    _need(_sha256_file(tar) == digest, 'Pages payload tar digest changed')
    prepared = result['prepared']
    _need(prepared['input']['path'] == str(tar)
          and prepared['input']['archive_sha256'] == prepared['expected']['archive_sha256'] == digest,
          'Pages preparation does not bind the selected tar')
    verify_stable(prepared)
    _binding(result['package_result'], result['manifest_sha256'], name)
    _, observed = _payload(root, name)
    _need(observed == tar and _sha256_file(tar) == digest, 'Pages payload changed during recheck')
    return dict(status='PAGES_ARTIFACT_STABLE', release_ready=False, tar_sha256=digest)


def verify_pages_artifact(payload_dir, *, package_result, manifest_sha256,
                          tar_name='artifact.tar', work_dir):
    """Consume caller-owned verified results, preserving each new attempt's failure.

    work_dir must be fresh, outside every checkout and separate from both input
    trees. A saved result needs an external lock before reuse by another layer.
    Outer ZIP identity, artifact ID and producer job authentication belong to the
    controlled caller; no Pages service acceptance or deployment is performed.
    """
    payload = Path(payload_dir).absolute()
    proof = Path(package_result['bundle_root']).absolute()
    requested = Path(work_dir).absolute()
    _need(not any(requested.is_relative_to(root) or root.is_relative_to(requested)
                  for root in (payload, proof)), 'Pages work must be separate from both input trees')
    work = _new_work(requested)
    receipt = work / 'pages.json'
    result = dict(schema=SCHEMA, status='FAIL', release_ready=False, stage='inputs',
                  hosted_provenance='CALLER_MUST_AUTHENTICATE', deployment='NOT_RUN',
                  receipt_path=str(receipt), locks=[], errors=[])
    try:
        name = _tar_name(tar_name)
        package = copy.deepcopy(package_result)
        final, manifest = _binding(package, manifest_sha256, name)
        root, tar = _payload(payload, name)
        _need(_sha256_file(tar) == final['sha256'], 'Pages payload tar bytes/digest differ from accepted final tar')
        result.update(payload_root=str(root), tar_name=name, tar_sha256=final['sha256'],
                      manifest_sha256=manifest_sha256, package_result=package,
                      source_sha=package['source_sha'], version=package['version'],
                      locks=[dict(path=str(tar), name=name, kind='file', sha256=final['sha256']),
                             copy.deepcopy(final), copy.deepcopy(manifest)])
        result['stage'] = 'plain-tar'
        _plain_tar(tar, final['sha256'])
        result['stage'] = 'safe-extraction'
        result['prepared'] = prepare_package(tar, work / 'prepared', expected_sha256=final['sha256'])
        result['stage'] = 'stability'
        result['status'] = 'PAGES_ARTIFACT_VERIFIED'
        verify_pages_artifact_stable(result)
        result['stage'] = 'complete'
    except Exception as error:
        result['status'] = 'FAIL'
        result['errors'].append(str(error))
        raise
    finally:
        with receipt.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(result, stream, ensure_ascii=False, indent=2)
            stream.write('\n')
    return result
