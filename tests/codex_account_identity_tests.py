"""Verify packaged Codex account discovery against native directory lookup."""

import hashlib
import os
from pathlib import Path
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


def check_profiles(adapter, root):
    upper = root / "Work"
    lower = root / "work"
    if os.name == "nt":
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
