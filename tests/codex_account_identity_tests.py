"""Verify packaged Codex account discovery against native directory lookup."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from codex_reply_tests import Adapter


def invoke(adapter, capability, version, operation, payload):
    adapter.serial += 1
    request = "r" + str(adapter.serial)
    adapter.send({"type": "invoke", "request_id": request, "capability_id": capability,
                  "version": version, "operation": operation, "deadline_ms": 15000, "payload": payload})
    while True:
        result = adapter.read()
        assert result["type"] in ("progress", "result"), result
        if result["type"] == "result":
            assert result["request_id"] == request, result
            return result


def discover(adapter, root, home):
    platform = "windows-x86_64" if os.name == "nt" else (
        "macos-arm64" if sys.platform == "darwin" else "linux-x86_64")
    payload = {"launcher": {"runner_key": "direct", "script_body": "codex", "environment_text": ""},
               "launch_environment": {"platform": platform, "effective_environment": {"CODEX_HOME": str(home)}},
               "default_home_fallback": str(root), "normalization_directory": str(root)}
    result = invoke(adapter, "accounts", 2, "discover", payload)
    assert result["status"] == "success", result
    account, = result["payload"]["accounts"]
    resolve = invoke(adapter, "accounts", 2, "resolve", payload)
    assert resolve["payload"]["adapter_account_key"] == account["adapter_account_key"], resolve
    return account


def check_win32_profile_selection(adapter, root):
    def extended(path):
        return Path("\\\\?\\" + str(path))

    for suffix, label in (("", "plain"), (" ", "space"), (".", "dot")):
        directory = extended(root / ("Alpha" + suffix))
        (directory / "sessions").mkdir(parents=True)
        (directory / "auth.json").write_text("{}", encoding="utf-8")
        history = [{"type": "session_meta", "payload": {"id": "native-alias"}},
                   {"type": "response_item", "payload": {"type": "message", "role": "user",
                                                        "content": "profile-marker-" + label}}]
        (directory / "sessions" / "rollout-native-alias.jsonl").write_text(
            "".join(json.dumps(row) + "\n" for row in history), encoding="utf-8")
    keys = {}
    for suffix, verbatim, expected in (("", False, "plain"), ("", True, "plain"),
                                       (" ", False, "plain"), (" ", True, "space"),
                                       (".", False, "plain"), (".", True, "plain")):
        selector = root / ("Alpha" + suffix)
        if verbatim:
            selector = extended(selector)
        account = discover(adapter, root, selector)
        key = account["adapter_account_key"]
        assert account["status"] == "available"
        assert keys.setdefault(expected, key) == key
        current = {"version": 2, "profile_path": account["profile_path"], "adapter_account_key": key}
        historical = {"version": 2, "profile_path": str(selector),
                      "adapter_account_key": "profile-" + hashlib.sha256(selector.as_posix().lower().encode()).hexdigest()}
        for grant in (current, historical):
            with tempfile.TemporaryDirectory(dir=root) as output:
                response = invoke(adapter, "conversation-export", 1, "export", {
                    "provider_session_id": "native-alias", "working_directory": str(root),
                    "launch_environment": {"platform": "windows-x86_64", "effective_environment": {}},
                    "format": "text", "output_directory": output, "account_context": grant})
                assert response["status"] == "success" and response["payload"]["account_context"] == grant, response
                text = (Path(output) / response["payload"]["file_name"]).read_text(encoding="utf-8")
                assert "profile-marker-" + expected in text, (selector, text)
    assert keys["plain"] != keys["space"]
    literal = extended(root / "Alpha ")
    assert discover(adapter, root, literal.as_posix())["adapter_account_key"] == keys["plain"]
    assert discover(adapter, root, literal.as_posix() + "/")["adapter_account_key"] == keys["space"]
    # The selected launcher must reproduce the same native profile after the
    # host parses its environment assignment, including its verbatim prefix.
    (root / "codex.exe").touch()
    discovered = invoke(adapter, "launcher-discovery", 1, "discover", {
        "platform": "windows-x86_64", "default_home_fallback": str(root),
        "ambient_environment": {"CODEX_HOME": str(literal), "PATH": str(root)}})
    selected, = (candidate for candidate in discovered["payload"]["candidates"] if candidate["default"])
    assert selected["environment_text"] == "CODEX_HOME=" + str(literal) + "\\", selected
    launcher_home = selected["environment_text"].split("=", 1)[1].strip()
    assert discover(adapter, root, launcher_home)["adapter_account_key"] == keys["space"]
    pending = root / "Pending"
    assert discover(adapter, root, str(pending) + " ")["adapter_account_key"] == discover(
        adapter, root, pending)["adapter_account_key"]


def check_profiles(adapter, root):
    upper = root / "Work"
    lower = root / "work"
    if os.name == "nt":
        alias_root = root.parent / "profile-selectors"
        alias_root.mkdir()
        try:
            check_win32_profile_selection(adapter, alias_root)
        finally:
            # The ordinary account tests need an empty directory to enable
            # per-directory case sensitivity. Remove literal names verbatim.
            shutil.rmtree("\\\\?\\" + str(alias_root))
        ordinary = root.parent / "Ordinary"
        ordinary.mkdir()
        expected = "profile-" + hashlib.sha256(ordinary.as_posix().lower().encode()).hexdigest()
        assert discover(adapter, root, ordinary)["adapter_account_key"] == expected
        # The per-directory flag is optional on Windows volumes.
        change = subprocess.run(["fsutil.exe", "file", "setCaseSensitiveInfo", str(root), "enable"],
                                capture_output=True, creationflags=subprocess.CREATE_NO_WINDOW)
        if change.returncode:
            print("The temporary volume does not support enabling case-sensitive directories.")
            return 77
    upper.mkdir()
    try:
        lower.mkdir()
    except FileExistsError:
        if sys.platform != "darwin":
            raise
    if not os.path.samefile(upper, lower):
        first = discover(adapter, root, upper)
        second = discover(adapter, root, lower)
        assert first["adapter_account_key"] != second["adapter_account_key"], (first, second)
    if os.name == "nt":
        retained = "profile-" + hashlib.sha256(upper.as_posix().lower().encode()).hexdigest()
        grant = {"version": 2, "adapter_account_key": retained, "profile_path": str(upper)}
        result = invoke(adapter, "account-pet", 1, "inspect", {"account_context": grant})
        assert result["status"] == "success" and result["payload"]["account_context"] == grant, result
    else:
        real = root / "real"
        (real / "child").mkdir(parents=True)
        (root / "alias").symlink_to(real / "child", target_is_directory=True)
        (real / "profile").mkdir()
        (root / "profile").mkdir()
        selected = discover(adapter, root, "alias/../profile")
        assert os.path.samefile(selected["profile_path"], real / "profile"), selected
        assert selected == discover(adapter, root, real / "profile")
        future = discover(adapter, root, "alias/../pending")
        assert Path(future["profile_path"]) == real / "pending" and future["status"] == "unavailable", future
    return 0


if __name__ == "__main__":
    adapter = Adapter(Path(sys.argv[1]).resolve())
    try:
        with tempfile.TemporaryDirectory() as directory:
            parent = Path(directory).resolve() / "profiles"
            parent.mkdir()
            sys.exit(check_profiles(adapter, parent))
    finally:
        adapter.close()
