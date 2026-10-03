"""Preserve exact U1 file and directory records across file-only artifact uploads."""
from __future__ import annotations

from pathlib import Path
import hashlib
import tarfile

from package_verification import _archive_plan, _sha256_file, inspect_inventory, prepare_package
from verify_execution_bundle import _need, _no_links

TRANSPORT_NAME = "execution-bundle.tar"


def create_execution_transport(evidence_dir, archive_path):
    """Pack an existing evidence tree without repairing or omitting any entry.

    Acceptance remains the strict U1 caller's responsibility. Directory records
    are carried as tar members, including empty diagnostic capture directories.
    The resulting single file survives Actions' file-only outer ZIP transport.
    """
    root = _no_links(evidence_dir)
    archive = Path(archive_path).absolute()
    _no_links(archive.parent)
    _need(archive.name == TRANSPORT_NAME and not archive.is_relative_to(root),
          "Execution transport must be the fixed archive outside evidence")
    before = inspect_inventory(root)
    _need(all(entry["type"] in ("file", "directory") for entry in before["entries"]),
          "Execution transport must not contain linked entries")
    with archive.open("xb") as raw, tarfile.open(fileobj=raw, mode="w") as stream:
        for entry in before["entries"]:
            info = tarfile.TarInfo(entry["path"])
            if entry["type"] == "directory":
                info.type, info.mode = tarfile.DIRTYPE, 0o755
                stream.addfile(info)
            else:
                info.size, info.mode = entry["size"], 0o644
                with _no_links(root / entry["path"]).open("rb") as source:
                    stream.addfile(info, source)
    _need(inspect_inventory(root) == before, "Execution evidence changed during transport packing")
    digest = _sha256_file(archive)
    records = []
    with _archive_plan(archive, digest) as (plan, opener):
        for name, entry in sorted(plan.items()):
            if not name:
                continue
            record = {"path":name, "type":entry.kind}
            if entry.kind == "file":
                with opener(entry.source) as source:
                    record.update(size=entry.size, sha256=hashlib.file_digest(source, "sha256").hexdigest())
            records.append(record)
    _need(records == before["entries"], "Execution archive differs from original evidence inventory")
    _need(inspect_inventory(root) == before, "Execution evidence changed during archive verification")
    return {"path":str(archive), "sha256":digest,
            "inventory_sha256":before["sha256"]}


def prepare_execution_transport(payload_dir, attempt_dir):
    """Open the sole nested tar through the maintained safe archive boundary.

    The caller authenticates and locks the outer artifact first. It must retain
    and recheck this preparation result as well as that outer transport, then
    pass the extracted original bytes to verify_execution_bundle unchanged.
    """
    root = _no_links(payload_dir)
    inventory = inspect_inventory(root)
    _need(len(inventory["entries"]) == 1 and
          inventory["entries"][0]["path"] == TRANSPORT_NAME and
          inventory["entries"][0]["type"] == "file",
          "Execution artifact must contain exactly " + TRANSPORT_NAME)
    entry = inventory["entries"][0]
    result = prepare_package(root / TRANSPORT_NAME, attempt_dir,
                             expected_sha256=entry["sha256"])
    _need(inspect_inventory(root) == inventory, "Execution transport changed during unpacking")
    return result
