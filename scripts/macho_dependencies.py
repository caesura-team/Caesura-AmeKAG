"""Bounded, read-only Mach-O dependency inspection for native package gates.

This verifies thin-image header/load-command framing and relocatable dependency
paths. It does not load code, verify ABI/signatures, or emulate dyld. Universal
images require a separate all-slices contract and are deliberately refused.
"""
from __future__ import annotations

from contextlib import ExitStack
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat
import struct


THIN = {b"\xce\xfa\xed\xfe": ("<", 28), b"\xfe\xed\xfa\xce": (">", 28),
        b"\xcf\xfa\xed\xfe": ("<", 32), b"\xfe\xed\xfa\xcf": (">", 32)}
FAT = {b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca", b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"}
DYLIB_COMMANDS = {0xC, 0x80000018, 0x8000001F, 0x80000023, 0x20}
KNOWN_REQUIRED_COMMANDS = DYLIB_COMMANDS | {0x8000001C, 0x80000022, 0x80000028,
                                          0x80000033, 0x80000034}
MAX_COMMAND_BYTES = 8 * 1024 * 1024
MAX_COMMANDS = 65536
MAX_IMAGES = 2048
MAX_STATES = 8192


def _need(value, message):
    if not value:
        raise ValueError(message)


def _identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def is_macho(path: Path) -> bool:
    """Recognize image framing, including unsupported universal images."""
    with path.open("rb") as stream:
        magic = stream.read(4)
    return magic in THIN or magic in FAT


def _path_text(value: str) -> str:
    _need(value and "\\" not in value and not any(ord(c) < 32 or ord(c) == 127 for c in value),
          "Invalid Mach-O dependency path spelling")
    return value


def is_system_library(value: str) -> bool:
    """Exact Apple OS namespaces, not arbitrary /usr or similarly named roots.

    dyld shared-cache libraries need not exist as individual on-disk files or
    appear separately in lsof. No third-party prefix such as Homebrew is OS.
    """
    # This classifier also sees ordinary lsof resources. A literal backslash
    # in their POSIX filename is not an OS namespace and must not be decoded
    # or rejected here. Mach-O command strings are validated separately.
    try:
        value = _path_text(value)
    except ValueError:
        return False
    path = PurePosixPath(value)
    return (path.is_absolute() and str(path) == value and ".." not in path.parts
            and (path.parts[:3] == ("/", "usr", "lib") and len(path.parts) > 3
                 or path.parts[:3] == ("/", "System", "Library") and len(path.parts) > 3))


def library_path_hint(value: str) -> bool:
    """Unresolved library-looking observations cannot hide as ordinary caches."""
    path = PurePosixPath(value.replace("\\", "/"))
    return (path.name.casefold().endswith(".dylib")
            or any(part.casefold().endswith(".framework") and path.name == part[:-10] for part in path.parts)
            or ("MacOS" in path.parts and any(part.casefold().endswith(".bundle") for part in path.parts)))


def inspect_macho(path: Path) -> dict:
    """Parse a stable ordinary endpoint; callers resolve permitted aliases first."""
    path = Path(path)
    # A regular-file check after a blocking open is too late if the path was
    # replaced by a FIFO. Keep ownership of the descriptor separate from the
    # stream, including when wrapping/validation raises before the first read.
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_NOFOLLOW", 0)
    with ExitStack() as cleanup:
        fd = os.open(path, flags)
        cleanup.callback(os.close, fd)
        stream = cleanup.enter_context(os.fdopen(fd, "rb", closefd=False))
        before = os.fstat(stream.fileno())
        path_before = path.stat()
        _need(stat.S_ISREG(before.st_mode), f"Mach-O image is not a regular file: {path}")
        # Windows Python 3.12 can expose change time through fstat but creation
        # time through stat. Compare ctime within each API, never across them.
        # Device/inode/size/mtime must still bind the opened file to this path.
        _need(_identity(before)[:-1] == _identity(path_before)[:-1],
              f"Opened Mach-O image does not match its path: {path}")
        magic = stream.read(4)
        _need(magic not in FAT, f"Universal Mach-O dependency slices are not verified: {path}")
        _need(magic in THIN, f"Missing Mach-O image signature: {path}")
        endian, header_size = THIN[magic]
        header = magic + stream.read(header_size - 4)
        _need(len(header) == header_size, f"Truncated Mach-O header: {path}")
        fields = struct.unpack(endian + ("8I" if header_size == 32 else "7I"), header)
        _, cpu, subtype, file_type, count, table_size, flags, *reserved = fields
        allowed_cpu = (0x01000007, 0x0100000C) if header_size == 32 else (7, 12)
        _need(cpu in allowed_cpu and file_type in (2, 6, 8), f"Unsupported Mach-O CPU/file type: {path}")
        _need(not reserved or reserved == [0], f"Nonzero reserved Mach-O header: {path}")
        _need(count <= MAX_COMMANDS and table_size <= MAX_COMMAND_BYTES
              and count * 8 <= table_size <= before.st_size - header_size,
              f"Invalid Mach-O load-command bounds: {path}")
        table = stream.read(table_size)
        _need(len(table) == table_size, f"Truncated Mach-O load commands: {path}")
        dependencies, rpaths, identifiers = [], [], []
        position = 0
        alignment = 8 if header_size == 32 else 4
        for _ in range(count):
            _need(position + 8 <= len(table), f"Missing Mach-O command header: {path}")
            command, size = struct.unpack_from(endian + "II", table, position)
            _need(size >= 8 and size % alignment == 0 and position + size <= len(table),
                  f"Invalid Mach-O command size: {path}")
            _need(not command & 0x80000000 or command in KNOWN_REQUIRED_COMMANDS,
                  f"Unknown required Mach-O loader command {command:#x}: {path}")
            _need(command not in (0x6, 0x7, 0x9, 0x10, 0x27),
                  f"Unsupported legacy/environment Mach-O loader command {command:#x}: {path}")
            if command in DYLIB_COMMANDS or command in (0xD, 0xE, 0x8000001C):
                minimum = 12 if command in (0xE, 0x8000001C) else 24
                _need(size >= minimum, f"Truncated Mach-O dependency command: {path}")
                start = struct.unpack_from(endian + "I", table, position + 8)[0]
                _need(minimum <= start < size, f"Invalid Mach-O dependency name offset: {path}")
                end = table.find(b"\0", position + start, position + size)
                _need(end >= 0, f"Unterminated Mach-O dependency name: {path}")
                value = _path_text(table[position + start:end].decode("utf-8", errors="strict"))
                if command == 0xE:
                    _need(value == "/usr/lib/dyld", f"Non-system Mach-O dynamic loader: {value}")
                elif command == 0x8000001C:
                    rpaths.append(value)
                elif command == 0xD:
                    identifiers.append(value)
                else:
                    dependencies.append({"name": value, "command": command,
                                         "weak": command == 0x80000018})
            position += size
        _need(position == table_size, f"Unused/truncated Mach-O load-command region: {path}")
        _need(len(identifiers) <= 1, f"Duplicate Mach-O dylib identifier: {path}")
        stream.seek(0)
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
        after, path_after = os.fstat(stream.fileno()), path.stat()
        _need(_identity(before) == _identity(after)
              and _identity(path_before) == _identity(path_after)
              and _identity(after)[:-1] == _identity(path_after)[:-1],
              f"Mach-O image changed during inspection: {path}")
    return {"path": str(path), "sha256": digest, "size": before.st_size,
            "cpu_type": cpu, "cpu_subtype": subtype, "file_type": file_type,
            "header_size": header_size, "command_count": count,
            "dependencies": dependencies, "rpaths": rpaths, "identifiers": identifiers}


def _contained(path: Path, package: Path, *, must_exist: bool) -> Path:
    # Check containment even for a dangling alias before strict resolution can
    # turn an escape into an ordinary missing-rpath candidate.
    resolved = path.resolve(strict=False)
    _need(resolved.is_relative_to(package), f"Mach-O dependency escapes package: {path}")
    if must_exist:
        resolved = path.resolve(strict=True)
    # Match actual spelling even on a case-insensitive audit host. Follow only
    # contained relative aliases already checked by the portable inventory.
    if must_exist:
        current = package
        for part in path.relative_to(package).parts:
            if part == "..":
                current = current.parent
                _need(current.is_relative_to(package), f"Mach-O dependency traversal escapes package: {path}")
                continue
            _need(part in {child.name for child in current.iterdir()}, f"Missing exact Mach-O path spelling: {path}")
            current = (current / part).resolve(strict=True)
            _need(current.is_relative_to(package), f"Mach-O dependency alias escapes package: {path}")
    return resolved


def _expand_local(value: str, owner: Path, executable: Path, package: Path) -> Path:
    _path_text(value)
    for prefix, base in (("@loader_path", owner.parent), ("@executable_path", executable.parent)):
        if value == prefix or value.startswith(prefix + "/"):
            suffix = value[len(prefix):].lstrip("/")
            candidate = base.joinpath(*PurePosixPath(suffix).parts)
            _contained(candidate, package, must_exist=False)
            return candidate
    raise ValueError(f"Non-relocatable Mach-O dependency/search path: {value} in {owner.name}")


def _run_paths(image: dict, owner: Path, executable: Path, package: Path) -> tuple[str, ...]:
    # Only package-local search paths are provable without emulating dyld.
    # Apple shared-cache names remain supported as explicit absolute edges.
    return tuple(dict.fromkeys(str(_expand_local(value, owner, executable, package))
                               for value in image["rpaths"]))


def inspect_package_closure(package: Path, executables: list[Path], libraries: list[Path]) -> dict:
    """Resolve every non-system edge for each executable's run-path context.

    All package dependencies, including weak ones, must be present: an omitted
    optional third-party library is not proof of a closed final artifact.
    Every declared run path is constrained, including an otherwise unused one.
    """
    package = Path(package).resolve(strict=True)
    _need(executables, "Mach-O closure requires an explicit executable")
    first = _contained(Path(executables[0]), package, must_exist=True)
    first_image = inspect_macho(first)
    first_paths = _run_paths(first_image, first, first, package)
    queue = [(Path(path), Path(path), ()) for path in executables]
    queue += [(Path(path), first, first_paths) for path in libraries]
    images, states, edges, systems = {first:first_image}, set(), [], set()
    while queue:
        owner, executable, inherited = queue.pop()
        owner = _contained(owner, package, must_exist=True)
        executable = _contained(executable, package, must_exist=True)
        state = (owner, executable, inherited)
        if state in states:
            continue
        _need(len(states) < MAX_STATES, "Mach-O dependency contexts exceed bound")
        states.add(state)
        if owner not in images:
            _need(len(images) < MAX_IMAGES, "Mach-O dependency images exceed bound")
            images[owner] = inspect_macho(owner)
        image = images[owner]
        if owner == executable:
            _need(image["file_type"] == 2, f"Mach-O executable has another file type: {owner.name}")
        else:
            _need(image["file_type"] in (6, 8), f"Mach-O library has another file type: {owner.name}")
        for identifier in image["identifiers"]:
            _need(not identifier.startswith("/") and identifier.startswith(("@rpath/", "@loader_path/", "@executable_path/"))
                  and ".." not in PurePosixPath(identifier).parts,
                  f"Non-relocatable Mach-O dylib identifier: {identifier}")
        own_paths = _run_paths(image, owner, executable, package)
        rpaths = tuple(dict.fromkeys([*own_paths, *inherited]))
        _need(len(rpaths) <= 256, "Mach-O run-path context exceeds bound")
        for dependency in image["dependencies"]:
            _need(len(edges) < MAX_STATES * 32, "Mach-O dependency edge count exceeds bound")
            name = dependency["name"]
            edge = {"image": owner.relative_to(package).as_posix(), "name": name,
                    "executable": executable.relative_to(package).as_posix()}
            if is_system_library(name):
                systems.add(name)
                edges.append({**edge, "resolution": "APPLE_SYSTEM_SHARED_CACHE_ALLOWED"})
                continue
            if name.startswith("/"):
                raise ValueError(f"Non-system absolute Mach-O dependency: {name} in {owner.name}")
            candidates = []
            if name.startswith("@rpath/"):
                suffix = name[len("@rpath/"):]
                _need(suffix and ".." not in PurePosixPath(suffix).parts,
                      f"Invalid Mach-O @rpath dependency: {name}")
                for prefix in rpaths:
                    candidates.append(("file", Path(prefix).joinpath(*PurePosixPath(suffix).parts)))
            else:
                candidates.append(("file", _expand_local(name, owner, executable, package)))
            chosen = None
            for kind, candidate in candidates:
                # Refuse an escaping alias even when an earlier pathname did
                # not exist. A missing local candidate may try the next rpath.
                try:
                    resolved = _contained(candidate, package, must_exist=True)
                except FileNotFoundError:
                    continue
                _need(resolved.is_file(), f"Mach-O dependency is not a file: {name}")
                chosen = (kind, resolved)
                break
            _need(chosen is not None, f"Missing package Mach-O dependency: {name} in {owner.name}")
            kind, target = chosen
            if target not in images:
                _need(len(images) < MAX_IMAGES, "Mach-O dependency images exceed bound")
                images[target] = inspect_macho(target)
            _need(images[target]["cpu_type"] == image["cpu_type"], f"Mach-O dependency CPU mismatch: {name}")
            edges.append({**edge, "resolution": "PACKAGE", "resolved": target.relative_to(package).as_posix(),
                          "sha256": images[target]["sha256"]})
            _need(len(edges) <= MAX_STATES * 32, "Mach-O dependency edge count exceeds bound")
            queue.append((target, executable, rpaths))
    return {"status": "MACHO_DEPENDENCY_CLOSURE_VERIFIED", "scope": "thin-image dependency paths; not ABI or signature validation",
            "images": [{**value, "relative_path": path.relative_to(package).as_posix()} for path, value in sorted(images.items())],
            "edges": edges, "system_libraries": sorted(systems)}
