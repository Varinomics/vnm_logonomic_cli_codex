"""Run both standalone process proposals with an installed Codex and a local Responses server."""

import argparse
import base64
import json
import os
import shlex
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from codex_account_identity_tests import Adapter, discover, invoke


class Local_model(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        self.server.requests.append(request)
        turn = len(self.server.requests)
        if turn == 1:
            tools = {tool.get("name") for tool in request.get("tools", [])}
            if os.name == "nt":
                script = "'" + str(self.server.probe).replace("'", "''") + "'"
            else:
                script = shlex.quote(str(self.server.probe))
            command = Path(sys.executable).name + " " + script
            if "exec_command" in tools:
                tool, arguments = "exec_command", {"cmd": command}
            elif "shell_command" in tools:
                tool, arguments = "shell_command", {"command": command}
            else:
                raise RuntimeError(f"Codex did not advertise a shell command tool: {tools}")
            item = {"type": "function_call", "id": "fc_probe", "call_id": "call_probe",
                    "name": tool, "arguments": json.dumps(arguments), "status": "completed"}
        else:
            item = {"type": "message", "id": "msg_finish", "role": "assistant", "status": "completed",
                    "content": [{"type": "output_text", "text": "NATIVE_STRUCTURED_OK", "annotations": []}]}
        identifier = f"resp_{turn}"
        response = {"id": identifier, "object": "response", "status": "completed", "output": [item],
                    "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2}}
        events = [
            {"type": "response.created", "response": {"id": identifier, "status": "in_progress", "output": []}},
            {"type": "response.output_item.done", "output_index": 0, "item": item},
            {"type": "response.completed", "response": response},
        ]
        body = "".join(f"event: {event['type']}\ndata: {json.dumps(event)}\n\n" for event in events).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def run(purpose, package, executable, root):
    home = root / "profile"
    workspace = root / "repository"
    temporary = root / "temp"
    if os.name == "nt":
        # Model a normal repository with its parent directory's inherited
        # access rules. Python 3.13 otherwise creates a private temporary ACL.
        subprocess.run(["icacls.exe", str(root), "/inheritance:e"],
                       capture_output=True, check=True)
    home.mkdir(mode=0o700)
    workspace.mkdir()
    temporary.mkdir()
    subprocess.run(["git", "init", "-q", str(workspace)], check=True)
    (workspace / "read-control").write_text("VISIBLE", encoding="utf-8")
    (workspace / "probe.py").write_text(
        "from pathlib import Path\nimport errno\n"
        "workspace = Path(__file__).parent\n"
        "print('READ_' + (workspace / 'read-control').read_text(), flush=True)\n"
        "try:\n    (workspace / 'written').write_text('native tool executed')\n"
        "except OSError as error:\n"
        "    if error.errno not in (errno.EACCES, errno.EPERM, errno.EROFS): raise\n"
        "    print('WRITE_DENIED')\nelse:\n    print('WRITE_SUCCEEDED')\n", encoding="utf-8")

    server = ThreadingHTTPServer(("127.0.0.1", 0), Local_model)
    server.probe = workspace / "probe.py"
    server.requests = []
    threading.Thread(target=server.serve_forever, daemon=True).start()
    (home / "config.toml").write_text(
        'model="fixture-model"\nmodel_provider="local"\nsandbox_mode="workspace-write"\napproval_policy="never"\n'
        f'[projects.{json.dumps(str(workspace))}]\ntrust_level="trusted"\n'
        '[windows]\nsandbox="unelevated"\n'
        '[model_providers.local]\nname="test"\n'
        f'base_url="http://127.0.0.1:{server.server_port}/v1"\n'
        'wire_api="responses"\nrequires_openai_auth=false\nsupports_websockets=false\n', encoding="utf-8")
    env = {key: os.environ[key] for key in ("PATH", "PATHEXT", "SystemRoot", "WINDIR")
           if key in os.environ}
    env.update({"HOME": str(root), "USERPROFILE": str(root), "CODEX_HOME": str(home),
                "TEMP": str(temporary), "TMP": str(temporary),
                "PATH": str(executable.parent) + os.pathsep + str(Path(sys.executable).parent) +
                        os.pathsep + env.get("PATH", "")})
    platform = "windows-x86_64" if os.name == "nt" else ("macos-arm64" if sys.platform == "darwin" else "linux-x86_64")
    suffix = ".exe" if os.name == "nt" else ""
    adapter = Adapter(package / "adapter" / ("codex_adapter" + suffix))
    try:
        selected = discover(adapter, root, home)
        grant = {"version": 2, "adapter_account_key": selected["adapter_account_key"], "profile_path": selected["profile_path"]}
        prompt = "Check the local file · preserve this prompt.\nUse the requested shell command."
        request = {"purpose": purpose, "launcher": {"script_body": "codex", "runner_key": "direct", "environment_text": ""},
                   "launcher_argv": ["codex"], "working_directory": str(workspace), "declared_assets": [],
                   "launch_environment": {"platform": platform, "effective_environment": env},
                   "prompt_base64": base64.b64encode(prompt.encode()).decode()}
        if purpose == "implementation":
            request["account_context"] = grant
        preparation = invoke(adapter, "structured-session", 3, "prepare", request)
        assert preparation["status"] == "success", preparation
        proposal = preparation["payload"]
        process = proposal["process"]
        assert proposal.get("account_context") == request.get("account_context") and process["stdin_payload"] == prompt, proposal
        assert process["working_directory"] == "session_workspace" and process["stdin"] == "payload", process
        assert not any("mcp_servers.logonomic_" in arg for arg in process["argv"]), process["argv"]
        child_environment = {key: env[key] for key in process.get("environment_names", []) if key in env}
        child_environment.update(process.get("environment", {}))
        child = subprocess.run([package / process["executable"], *process["argv"]], cwd=workspace,
                               env=child_environment, input=process["stdin_payload"].encode(), capture_output=True, timeout=90)
        assert child.returncode == 0, (child.stdout, child.stderr)
        output = invoke(adapter, "structured-session", 3, "inspect", {"purpose": purpose, "inspection": {
            "protocol_state_base64_chunks": proposal["inspection"]["protocol_state_base64_chunks"],
            "stdout_base64_chunks": [base64.b64encode(child.stdout).decode()],
            "stderr_base64_chunks": [base64.b64encode(child.stderr).decode()] if child.stderr else [],
            "final": True, "timed_out": False, "exit_code": child.returncode}})
        assert output["status"] == "success" and output["payload"].get("complete"), output
        assert b"".join(base64.b64decode(chunk) for chunk in output["payload"]["result_base64_chunks"]) == b"NATIVE_STRUCTURED_OK"
        assert len(server.requests) == 2, len(server.requests)
        assert prompt in json.dumps(server.requests[0]["input"], ensure_ascii=False).replace("\\n", "\n")
        receipt = next(item for item in server.requests[1]["input"] if item.get("type") == "function_call_output")
        expected = "WRITE_DENIED" if purpose == "read_only_review" else "WRITE_SUCCEEDED"
        assert "READ_VISIBLE" in str(receipt) and expected in str(receipt), receipt
        assert (workspace / "written").exists() == (purpose == "implementation")
        print(f"{purpose}: native tool read the control, {expected}, final result verified", flush=True)
    finally:
        adapter.close()
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package", required=True, type=Path)
    parser.add_argument("--codex", required=True, type=Path)
    parser.add_argument("--scratch-root", type=Path)
    options = parser.parse_args()
    version = subprocess.run([options.codex, "--version"], capture_output=True, text=True, check=True)
    print("native:", version.stdout.strip(), flush=True)
    for purpose in ("read_only_review", "implementation"):
        with tempfile.TemporaryDirectory(dir=options.scratch_root) as directory:
            run(purpose, options.package.resolve(), options.codex.absolute(), Path(directory))
