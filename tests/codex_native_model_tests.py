"""Exercise installed Codex hooks through the packaged helper and adapter on loopback.

Supply --codex, --helper, --adapter and the test --wrapper executable.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import threading
import uuid
from contextlib import closing
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from codex_native_wake_tests import Native
from codex_reply_tests import Model_host


ROOT_MODEL = "fixture-root"
SWITCHED_MODEL = "fixture-switched"
CHILD_MODEL = "gpt-6-luna"


class Model_endpoint(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        model = request["model"]
        self.server.models.append(model)
        if model == CHILD_MODEL:
            self.server.child_started.set()
            if not self.server.release_child.wait(45):
                self.server.child_timed_out.set()

        if self.server.models == [ROOT_MODEL]:
            namespace = next(tool["name"] for tool in request["tools"] if
                any(member.get("name") == "spawn_agent" for member in tool.get("tools", [])))
            item = {"type": "function_call", "name": "spawn_agent", "namespace": namespace,
                "call_id": "call_child", "arguments": json.dumps({
                    "message": "Reply with the fixture acknowledgement.", "model": CHILD_MODEL})}
        else:
            item = {"type": "message", "status": "completed", "role": "assistant",
                "content": [{"type": "output_text", "text": "Fixture acknowledged.", "annotations": []}]}
        item["id"] = "item_" + uuid.uuid4().hex
        response_id = "resp_" + uuid.uuid4().hex
        response = {"id": response_id, "object": "response", "status": "completed", "output": [item],
            "usage": {"input_tokens": 1, "output_tokens": 1, "total_tokens": 2}}
        events = [
            {"type": "response.created", "response": {"id": response_id, "status": "in_progress", "output": []}},
            {"type": "response.output_item.done", "output_index": 0, "item": item},
            {"type": "response.completed", "response": response}]
        data = "".join(f"event: {event['type']}\ndata: {json.dumps(event)}\n\n" for event in events).encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


def root_turn(native, session, model):
    result = native.call("turn/start", {"threadId": session, "model": model,
        "input": [{"type": "text", "text": "Run this fixture turn.", "text_elements": []}]})
    turn = result["turn"]["id"]
    event = native.wait(lambda event: event.get("method") == "turn/completed" and
        event["params"]["threadId"] == session and event["params"]["turn"]["id"] == turn, 35)
    assert event["params"]["turn"]["status"] == "completed", event


def check_models(codex, helper, adapter, wrapper, root):
    profile = root / "profile"
    profile.mkdir()
    bridge = root / "classifications"
    model_store = root / "modèles"
    model_store.mkdir()
    environment = {"LOGONOMIC_VNMCTL_PATH": str(wrapper), "LOGONOMIC_SESSION_CAPABILITY": "a" * 64,
        "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.codex", "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "b" * 64,
        "LOGONOMIC_PROVIDER_GENERATION_NONCE": "c" * 32, "LOGONOMIC_PROVIDER_HELPERS_DIR": str(helper.parent),
        "CODEX_TEST_CAPTURE": str(root / "deliveries.jsonl"), "CODEX_TEST_CLASSIFIER_DIRECTORY": str(bridge),
        "VNM_CODEX_MODEL_SOURCE": str(model_store / "binding.sqlite"), "NO_PROXY": "127.0.0.1,localhost"}
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY"):
        environment[name] = "http://127.0.0.1:1"
    command = subprocess.list2cmdline([str(helper)]) if os.name == "nt" else shlex.quote(str(helper))
    events = ("SessionStart", "UserPromptSubmit", "SubagentStart", "SubagentStop")
    hooks = []
    for event in events:
        hooks.extend(["-c", f"hooks.{event}=[{{hooks=[{{type='command',command={json.dumps(command)}}}]}}]"])

    with ThreadingHTTPServer(("127.0.0.1", 0), Model_endpoint) as server, Model_host(adapter, bridge) as host:
        server.models = []
        server.child_started = threading.Event()
        server.child_timed_out = threading.Event()
        server.release_child = threading.Event()
        threading.Thread(target=server.serve_forever, daemon=True).start()
        (profile / "config.toml").write_text(f"""
model = "{ROOT_MODEL}"
model_provider = "fixture"
web_search = "disabled"
[features]
plugins = false
apps = false
multi_agent = true
[model_providers.fixture]
name = "Loopback fixture"
base_url = "http://127.0.0.1:{server.server_port}/v1"
wire_api = "responses"
requires_openai_auth = false
supports_websockets = false
""", encoding="utf-8")
        try:
            with closing(Native(codex, profile, root, hooks, environment_overrides=environment)) as setup:
                declared = setup.call("hooks/list", {"cwds": [str(root)]})["data"][0]["hooks"]
                commands = [hook for hook in declared if hook.get("command") == command]
                assert len(commands) == len(events), declared
                with (profile / "config.toml").open("a", encoding="utf-8") as config:
                    for hook in commands:
                        config.write(f"\n[hooks.state.{json.dumps(hook['key'])}]\n"
                            f"trusted_hash = {json.dumps(hook['currentHash'])}\n")
            with closing(Native(codex, profile, root, hooks, environment_overrides=environment)) as native:
                session = native.call("thread/start", {"cwd": str(root), "model": ROOT_MODEL,
                    "modelProvider": "fixture", "approvalPolicy": "never", "sandbox": "read-only"})["thread"]["id"]
                try:
                    root_turn(native, session, ROOT_MODEL)
                    assert server.child_started.wait(10), ("Child model was never requested", server.models)
                    root_turn(native, session, SWITCHED_MODEL)
                    child = next(actor for event in native.history if event.get("method") == "item/completed"
                        for item in [event["params"]["item"]] if item.get("type") == "collabAgentToolCall"
                        for actor in item["receiverThreadIds"])
                    assert child != session
                    root_samples = [sample for sample in host.updates if not sample["agent_id"]]
                    assert [sample["model"] for sample in root_samples] == [ROOT_MODEL, SWITCHED_MODEL], host.updates
                    assert int(root_samples[0]["sequence"]) < int(root_samples[1]["sequence"])
                    child_samples = [sample for sample in host.updates if sample["agent_id"] == child]
                    assert child_samples and {sample["model"] for sample in child_samples} == {CHILD_MODEL}, host.updates
                    assert all(sample["provider_session_id"] == session and sample["transition_nonce"] == host.nonce
                        and sample["agent_id"] in ("", child) and not sample.get("provider") for sample in host.updates)
                    assert SWITCHED_MODEL in server.models
                    assert not server.child_timed_out.is_set(), "Child request expired before the root switched"
                    print("Native loopback models with simulated title-first admission:", json.dumps(host.updates))
                finally:
                    server.release_child.set()
        finally:
            server.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("codex", "helper", "adapter", "wrapper"):
        parser.add_argument("--" + name, type=Path, required=True)
    options = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="codex-native-model-", ignore_cleanup_errors=True) as temporary:
        check_models(options.codex.resolve(), options.helper.resolve(), options.adapter.resolve(),
            options.wrapper.resolve(), Path(temporary))
