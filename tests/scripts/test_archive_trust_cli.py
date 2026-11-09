"""Executable CARC publisher-trust regressions using fresh packer identities.

Usage: python test_archive_trust_cli.py ENGINE_EXE CARC_PACK_EXE
Each engine process uses an isolated resource root and a separate launch CWD.
"""
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def execute(command: list[str], cwd: Path, input_text: str = "") -> subprocess.CompletedProcess[str]:
    env = os.environ.copy()
    env.pop("CAESURA_RESOURCE_ROOT", None)
    return subprocess.run(
        command, cwd=cwd, input=input_text, capture_output=True,
        text=True, encoding="utf-8", errors="replace", timeout=30, env=env,
    )


def publisher(packer: Path, launch: Path, name: str) -> tuple[bytes, bytes]:
    source = launch / (name + "_input")
    source.mkdir()
    (source / "publisher.txt").write_text(name, encoding="utf-8")
    archive = launch / (name + ".carc")
    key = launch / (name + ".pub")
    result = execute([str(packer), str(source), str(archive), str(key)], launch)
    if result.returncode != 0:
        raise RuntimeError("Fixture packing failed:\n" + result.stdout + result.stderr)
    key_bytes = key.read_bytes()
    if len(key_bytes) != 32:
        raise RuntimeError("Fixture public key is not exactly 32 raw bytes")
    return archive.read_bytes(), key_bytes


def check_unicode_packer(packer: Path, launch: Path) -> None:
    source = launch / "资源 𠮷"
    source.mkdir()
    original = launch / "初版 𠮷.carc"
    updated = launch / "新版 𠮷.carc"
    patch = launch / "差分 𠮷.carc"
    applied = launch / "更新后 𠮷.carc"
    public = launch / "公钥 𠮷.pub"
    private = launch / "私钥 𠮷.key"
    extracted = launch / "解包 𠮷"

    def run(*arguments: Path | str) -> str:
        result = execute([str(packer), *(str(value) for value in arguments)], launch)
        if result.returncode != 0:
            raise RuntimeError(f"Unicode CARC command failed ({arguments[0]}):\n"
                               + result.stdout + result.stderr)
        return result.stdout

    payload = source / "story.bin"
    payload.write_bytes(b"original")
    run("pack", source, original, public, private)
    if public.stat().st_size != 32 or private.stat().st_size != 64:
        raise RuntimeError("Unicode key files have unexpected lengths")
    if len(run("list", original, public).splitlines()) != 1:
        raise RuntimeError("Unicode archive listing has an unexpected entry count")
    payload.write_bytes(b"updated")
    run("pack", source, updated)
    run("delta", original, updated, patch)
    run("verify-delta", patch)
    run("apply", original, patch, applied)
    run("extract", applied, extracted, "--path", "story.bin")
    if (extracted / "story.bin").read_bytes() != b"updated":
        raise RuntimeError("Unicode archive round-trip changed the payload")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    engine, packer = (Path(value).resolve(strict=True) for value in sys.argv[1:])
    failures: list[str] = []
    case_count = 0
    with tempfile.TemporaryDirectory(prefix="caesura_u8_cli_") as temporary:
        base = Path(temporary)
        launch = base / "launch"
        launch.mkdir()
        check_unicode_packer(packer, launch)
        archive_a, key_a = publisher(packer, launch, "publisher_a")
        archive_b, key_b = publisher(packer, launch, "publisher_b")
        if key_a == key_b:
            raise RuntimeError("Fixture publishers unexpectedly share a public key")
        (launch / "host.pub").write_bytes(key_a)
        (launch / "发行者公钥.pub").write_bytes(key_a)
        (launch / "攻击者公钥.pub").write_bytes(key_b)
        spaced_key = Path("publisher keys") / "发行者 公钥.pub"
        (launch / spaced_key).parent.mkdir()
        (launch / spaced_key).write_bytes(key_a)
        (launch / "empty.pub").write_bytes(b"")
        (launch / "short.pub").write_bytes(key_a[:-1])
        (launch / "long.pub").write_bytes(key_a + b"X")
        (launch / "hex.pub").write_text(key_a.hex(), encoding="ascii")
        (launch / "key_directory").mkdir()

        pinned = ["--carc-trust", "pinned", "--carc-public-key", "host.pub"]
        request = json.dumps({"id": 81, "method": "ping"}) + "\n"

        def check(
            name: str, options: list[str], archive: bytes | None = archive_a,
            *, accepted: bool, lua_verify: bool = False, error: str = "",
            archive_name: str = "game.carc",
        ) -> None:
            nonlocal case_count
            case_count += 1
            resource = base / ("resource_" + str(case_count))
            (resource / "assets").mkdir(parents=True)
            (resource / "scripts" / "kag").mkdir(parents=True)
            lua_value = "true" if lua_verify else "false"
            config_marker = "U8_CONFIG_LOADED=" + lua_value
            (resource / "scripts" / "config.lua").write_text(
                "config = {carc_verify_on_startup = " + lua_value + "}\n"
                "print('" + config_marker + "')\n", encoding="utf-8",
            )
            (resource / "scripts" / "kag" / "init.lua").write_text(
                "-- Isolated startup fixture.\n", encoding="utf-8",
            )
            if archive is not None:
                (resource / archive_name).write_bytes(archive)
            # Both untrusted same-name paths advertise B; only launch/host.pub is A.
            (resource / "host.pub").write_bytes(key_b)
            (resource / "game.carc.pub").write_bytes(key_b)
            command = [str(engine), "--headless", "--resource-root", str(resource), *options]
            try:
                result = execute(command, launch, request)
                output = result.stdout + result.stderr
                responses = []
                for line in result.stdout.splitlines():
                    try:
                        value = json.loads(line)
                    except ValueError:
                        continue
                    if isinstance(value, dict):
                        responses.append(value)
                ping_ok = any(item.get("id") == 81 and item.get("result") == "ok"
                              for item in responses)
                if accepted:
                    ok = result.returncode == 0 and ping_ok and config_marker in output
                    if archive is not None:
                        ok = ok and "Registered CARC" in output
                else:
                    ok = result.returncode != 0 and not ping_ok and config_marker not in output
                    if error:
                        ok = ok and error in output
                    if error.startswith("[main] ERROR:"):
                        ok = ok and "[Engine]" not in output
                if not ok:
                    failures.append(name)
                    print("FAIL " + name + " (exit=" + str(result.returncode) + ")")
                    print(output)
                else:
                    print("PASS " + name, flush=True)
            except subprocess.TimeoutExpired:
                failures.append(name)
                print("FAIL " + name + " (engine exceeded 30 seconds)", flush=True)

        for verify in (False, True):
            suffix = " Lua verify=" + str(verify).lower()
            check("pinned A accepts A; launch-relative key" + suffix, pinned,
                  accepted=True, lua_verify=verify)
            check("pinned A rejects complete B substitution" + suffix, pinned, archive_b,
                  accepted=False, lua_verify=verify, error="Failed to initialize engine.")
            check("pinned A rejects B with forged A trailer" + suffix, pinned,
                  archive_b[:-32] + key_a, accepted=False, lua_verify=verify,
                  error="Failed to initialize engine.")
            check("pinned A ignores A archive's replaced trailer" + suffix, pinned,
                  archive_a[:-32] + key_b, accepted=True, lua_verify=verify)
            check("default compatible accepts B" + suffix, [], archive_b,
                  accepted=True, lua_verify=verify)
        check("explicit compatible accepts B", ["--carc-trust", "compatible"], archive_b,
              accepted=True)
        check("pinned A permits no archives", pinned, None, accepted=True)
        check("duplicate identical trust options", pinned + pinned, accepted=True)
        check("host-selected absolute key", ["--carc-trust", "pinned", "--carc-public-key",
              str(launch / "host.pub")], accepted=True)
        check("host-selected non-ASCII key filename", ["--carc-trust", "pinned",
              "--carc-public-key", "发行者公钥.pub"], accepted=True)
        check("pinned engine mounts a Unicode archive filename", pinned, accepted=True,
              archive_name="dlc_剧情𠮷.carc")
        check("pinned engine rejects the wrong signer for a Unicode archive filename",
              pinned, archive_b, accepted=False, error="initialization rejected",
              archive_name="dlc_剧情𠮷.carc")
        check("Unicode key and spaces preserve argument mapping", ["--carc-trust", "pinned",
              "--export-dir", "unrelated quoted argument", "--carc-public-key", str(spaced_key)],
              accepted=True)
        check("duplicate identical Unicode key paths", ["--carc-trust", "pinned",
              "--carc-public-key", "发行者公钥.pub", "--carc-public-key", "发行者公钥.pub"],
              accepted=True)
        check("Unicode key conflicts compare original native names", ["--carc-trust", "pinned",
              "--carc-public-key", "发行者公钥.pub", "--carc-public-key", "攻击者公钥.pub"],
              accepted=False, error="[main] ERROR: Conflicting --carc-public-key options")
        check("unreadable Unicode key retains diagnostic path", ["--carc-trust", "pinned",
              "--carc-public-key", "不存在公钥.pub"], accepted=False,
              error="[main] ERROR: --carc-public-key is not a readable file: 不存在公钥.pub")
        check("pinned B accepts B", ["--carc-trust", "pinned", "--carc-public-key",
              "publisher_b.pub"], archive_b, accepted=True)

        invalid_options = [
            ("pinned without key", ["--carc-trust", "pinned"], "--carc-public-key is required"),
            ("key without explicit mode", ["--carc-public-key", "host.pub"],
             "--carc-public-key requires explicit --carc-trust pinned"),
            ("compatible with key", ["--carc-trust", "compatible", "--carc-public-key", "host.pub"],
             "--carc-public-key requires explicit --carc-trust pinned"),
            ("key before compatible", ["--carc-public-key", "host.pub", "--carc-trust", "compatible"],
             "--carc-public-key requires explicit --carc-trust pinned"),
            ("unknown trust mode", ["--carc-trust", "trust-anything"], "Invalid --carc-trust value"),
            ("missing trust value", ["--carc-trust"], "--carc-trust requires a value"),
            ("option is not trust value", ["--carc-trust", "--headless"], "--carc-trust requires a value"),
            ("missing key path", ["--carc-trust", "pinned", "--carc-public-key"],
             "--carc-public-key requires a value"),
            ("option is not key path", ["--carc-trust", "pinned", "--carc-public-key", "--headless"],
             "--carc-public-key requires a value"),
            ("unknown trust flag", ["--carc-trsut", "pinned"], "Unknown command-line option"),
            ("conflicting trust modes", pinned + ["--carc-trust", "compatible"],
             "Conflicting --carc-trust options"),
            ("reversed conflicting trust modes", ["--carc-trust", "compatible", *pinned],
             "Conflicting --carc-trust options"),
            ("conflicting key paths", pinned + ["--carc-public-key", "publisher_b.pub"],
             "Conflicting --carc-public-key options"),
        ]
        for name, options, error in invalid_options:
            check(name, options, accepted=False, error="[main] ERROR: " + error)
        for name in ("empty.pub", "short.pub", "long.pub", "hex.pub", "missing.pub", "key_directory"):
            check("invalid key " + name, ["--carc-trust", "pinned", "--carc-public-key", name],
                  accepted=False, error="[main] ERROR: --carc-public-key")
    print(str(case_count - len(failures)) + "/" + str(case_count) + " CARC CLI cases passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
