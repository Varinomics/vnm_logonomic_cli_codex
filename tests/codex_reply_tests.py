"""Exercise native Stop bytes through the packaged helper and adapter, without an account."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import uuid


def launch_options():
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


class Adapter:
    def __init__(self, executable):
        self.process = subprocess.Popen([str(executable)], stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, **launch_options())
        self.serial = 0
        self.send({"type": "hello", "core_protocol_generations": [1], "package_id": "logonomic.cli.codex",
            "generation_digest": hashlib.sha256(executable.read_bytes()).hexdigest(),
            "generation_nonce": uuid.uuid4().hex,
            "limits": {"max_frame_bytes": 4194304, "max_json_depth": 32,
                "max_in_flight_requests": 64, "max_progress_frames_per_request": 100000},
            "host": {"product": "logonomic", "version": "1.0.0"}})
        assert self.read()["type"] == "hello"
        capabilities = self.read()
        assert capabilities["type"] == "capabilities"
        assert {"capability_id": "attention-kind", "version": 1} in capabilities["capabilities"]

    def send(self, frame):
        self.process.stdin.write((json.dumps(frame) + "\n").encode("utf-8"))
        self.process.stdin.flush()

    def read(self):
        frame = self.process.stdout.readline()
        assert frame, "adapter closed before its response"
        return json.loads(frame)

    def classify(self, delivery, *, attention=True, pending=None, epoch=2, model=False,
                 ledger=None, title=None, capability=None):
        payload = {"event": delivery["event"], "transition_nonce": delivery["transition_nonce"],
            "ledger": ledger or {"current_epoch": epoch, "current_transition": {
                "kind": "start", "provider_session_id": "native-session", "nonce": "7" * 32}}}
        if pending is not None:
            payload["pending_conversational_request_id"] = pending
        if model:
            payload["model_state"] = {"version": 1}
        if title is not None:
            payload["terminal_identity"] = title
        self.serial += 1
        request_id = "r" + str(self.serial)
        self.send({"type": "invoke", "request_id": request_id,
            "capability_id": capability or ("attention-kind" if attention else "helper-ingress"),
            "version": 1 if attention else 2, "operation": "classify_event", "deadline_ms": 5000,
            "payload": payload})
        while True:
            response = self.read()
            if response["type"] == "result":
                assert response["request_id"] == request_id and response["status"] == "success", response
                return response["payload"]

    def close(self):
        self.send({"type": "shutdown", "grace_ms": 5000})
        self.process.stdin.close()
        assert self.process.wait(timeout=10) == 0


class Model_host:
    """Return the running adapter's classification receipt to the synchronous helper."""

    def __init__(self, executable, directory, title_first=True):
        self.directory = Path(directory)
        self.directory.mkdir(exist_ok=True)
        self.adapter = Adapter(executable)
        self.title_first = title_first
        self.title = None
        self.epoch = 0
        self.session = None
        self.nonce = None
        self.kind = None
        self.updates = []
        self.deliveries = []
        self.stopped = threading.Event()
        self.failure = None
        self.thread = threading.Thread(target=self.serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_):
        self.stopped.set()
        self.thread.join(5)
        self.adapter.close()
        if self.failure:
            raise self.failure

    def ledger(self):
        transition = {"provider_session_id": self.session, "nonce": self.nonce, "kind": self.kind}
        return {"current_epoch": self.epoch, "current_transition": transition if self.epoch else None}

    def select(self, session):
        self.title = {"version": 1, "context": {"version": 1, "native_version": "0.169.0",
            "native_store": {"version": 1, "codex_home": str(self.directory), "sqlite_home": str(self.directory)}},
            "terminal_event": {"terminal_id": "1", "generation": "1", "source": "logonomic_terminal_worker",
                "stream": "terminal", "stream_index": 0, "timestamp": 1,
                "payload": {"kind": "terminal_title_changed", "title_sequence": str(self.epoch + 1),
                    "title": session[:29] + "... | " + session}}}
        delivery = {"event": {"name": "NativeSignal", "body": {"origin": "host_terminal"}},
            "transition_nonce": uuid.uuid4().hex}
        result = self.adapter.classify(delivery, ledger=self.ledger(), title=self.title,
            capability="terminal-identity")
        assert result["terminal_identity"]["provider_session_id"] == session, result
        self.commit(delivery, result)

    def commit(self, delivery, result):
        if result["classification"] == "transition":
            self.session = result["observations"][0]["provider_session_id"]
            self.nonce = delivery["transition_nonce"]
            self.kind = result["transition"]["kind"]
            self.epoch += 1

    def serve(self):
        try:
            while not self.stopped.is_set():
                for path in self.directory.glob("*.request"):
                    delivery = json.loads(path.read_text(encoding="utf-8"))
                    path.unlink()
                    event = delivery["event"]
                    if self.title_first and not self.epoch and event["name"] == "SessionStart":
                        self.select(event["body"]["session_id"])
                    result = self.adapter.classify(delivery, model=True, title=self.title, ledger=self.ledger())
                    self.commit(delivery, result)
                    self.deliveries.append((delivery, result))
                    self.updates.extend(result.get("model_state", {}).get("updates", []))
                    response = {"accepted": True, "epoch": self.epoch}
                    if result.get("helper_response") is not None:
                        response["helper_response"] = result["helper_response"]
                    pending = path.with_suffix(".replying")
                    pending.write_text(json.dumps(response), encoding="utf-8")
                    pending.replace(path.with_suffix(".response"))
                self.stopped.wait(0.005)
        except Exception as error:
            self.failure = error


def model_source_contract(executable, directory, environment, deliver):
    root = "019933a1-6e43-7102-a520-b3c8294cc763"
    child = "019944b2-7f54-7203-b631-c4d93a5dd874"
    other = "019955c3-8065-7304-a742-d5ea4b6ee985"
    turns = [f"019955c3-8065-7304-a742-{index:012x}" for index in range(1, 6)]

    def rollout(actor):
        return str(directory / f"rollout-session-{actor}.jsonl")

    start = {"hook_event_name": "SessionStart", "session_id": root, "source": "startup",
        "transcript_path": rollout(root), "model": "gateway/start"}
    prompt = {"hook_event_name": "UserPromptSubmit", "session_id": root, "transcript_path": rollout(root)}
    child_start = {"hook_event_name": "SubagentStart", "session_id": root, "agent_id": child,
        "agent_type": "explorer", "transcript_path": rollout(child), "turn_id": turns[0], "model": "gateway/child"}
    for title_first in (True, False):
        carrier = directory / ("title-first" if title_first else "hook-first")
        scratch = directory / "Modèles-工作" / carrier.name
        scratch.mkdir(parents=True)
        environment["CODEX_TEST_CLASSIFIER_DIRECTORY"] = str(carrier)
        environment["VNM_CODEX_MODEL_SOURCE"] = str(scratch / "receipt.sqlite")
        with Model_host(executable, carrier, title_first) as host:
            initial = deliver(start)
            assert host.epoch == 1 and host.session == root
            assert host.deliveries[-1][1]["helper_response"] == {"model_activation": host.nonce}
            assert (initial["transition_nonce"] != host.nonce) == title_first
            assert [update["model"] for update in host.updates] == ([] if title_first else [start["model"]])
            expected = len(host.updates)

            first = deliver({**prompt, "turn_id": turns[0], "model": "gateway/first"})
            deliver(child_start)
            deliver({**prompt, "agent_id": child, "turn_id": turns[1],
                "transcript_path": rollout(child), "model": "gateway/child-next"})
            deliver({**prompt, "turn_id": turns[2], "model": "gateway/next"})
            actual = [(u["agent_id"], u["model"], u["sequence"]) for u in host.updates[expected:]]
            assert actual == [("", "gateway/first", "1"), (child, "gateway/child", "1"),
                (child, "gateway/child-next", "2"), ("", "gateway/next", "2")], actual
            assert all(u["provider_session_id"] == root and u["transition_nonce"] == host.nonce
                and not u.get("provider") for u in host.updates)
            expected = len(host.updates)
            for event in (
                {**prompt, "turn_id": turns[1], "model": "older"},
                {**prompt, "turn_id": turns[2], "model": "duplicate"},
                {**prompt, "turn_id": turns[3], "model": "foreign-rollout", "transcript_path": rollout(other)},
                {**prompt, "turn_id": turns[3], "model": "foreign-actor", "agent_id": child},
                {**prompt, "turn_id": turns[3], "model": "foreign-root", "session_id": other},
                {**prompt, "turn_id": turns[3], "model": "ambiguous-actor", "agent_id": root},
            ):
                deliver(event)
            assert len(host.updates) == expected
            old_nonce = host.nonce
            host.select(other)
            host.select(root)
            assert host.nonce != old_nonce
            replay = host.adapter.classify(first, ledger=host.ledger(), model=True)
            assert "model_state" not in replay
            deliver(start)
            assert not host.deliveries[-1][1].get("helper_response")
            delivered = deliver({**prompt, "turn_id": turns[4], "model": "retired"})
            assert delivered["event"]["body"]["model_state"]["updates"][0]["transition_nonce"] == old_nonce
            assert len(host.updates) == expected


def reply_contract(helper, wrapper, executable, directory):
    capture = directory / "requests.jsonl"
    environment = {**os.environ,
        "LOGONOMIC_VNMCTL_PATH": str(wrapper),
        "LOGONOMIC_SESSION_CAPABILITY": "a" * 64,
        "LOGONOMIC_PROVIDER_PACKAGE_ID": "logonomic.cli.codex",
        "LOGONOMIC_PROVIDER_GENERATION_DIGEST": "b" * 64,
        "LOGONOMIC_PROVIDER_GENERATION_NONCE": "c" * 32,
        "LOGONOMIC_PROVIDER_HELPERS_DIR": str(helper.parent),
        "CODEX_TEST_CAPTURE": str(capture)}

    def run(data):
        capture.write_text("", encoding="utf-8")
        result = subprocess.run([str(helper)], input=data, capture_output=True, timeout=15,
            env=environment, **launch_options())
        records = [json.loads(line) for line in capture.read_text(encoding="utf-8").splitlines()]
        return result, records

    def deliver(payload):
        result, records = run(json.dumps(payload, ensure_ascii=False).encode("utf-8"))
        assert result.returncode == 0 and result.stdout == b"", (result.returncode, result.stdout, result.stderr)
        return next(record["request"] for record in records if record["operation"] == "provider-event")

    stop = {"hook_event_name": "Stop", "session_id": "native-session", "turn_id": "native-turn"}
    question = "Which directory should I use?"
    private = "Earlier-detail-canary " * 180
    long_paragraph = "Please provide " + "the required configuration detail " * 120 + "for this task."
    unicode_paragraph = "Which " + "résumé🙂 " * 400 + "should I use？"
    cases = (
        ("long explanation", private + "\n\n" + question, True, True),
        ("long paragraph", long_paragraph, True, True),
        ("UTF-8 paragraph", unicode_paragraph, True, True),
        ("ordinary conclusion", private + "\n\nThe change is complete.", True, False),
        ("earlier question", question + "\n\nThe change is complete.", True, False),
        ("short question", question, True, True),
        ("open fence", "```text\n" + private + "\n\n" + question, False, False),
        ("closed fence", "~~~~text\n" + private + "\n\n" + question + "\n~~~~", False, False),
        ("prose after fence", "```\n" + private + "\n```\n\n" + question, True, True),
        ("lazy blockquote", "> Quoted text\n" + private + question, False, False),
        ("indented code", "    " + question, False, False),
        ("invalid native type", {"text": question}, False, False),
        ("embedded NUL", "Which \0directory?", False, False),
    )
    adapter = Adapter(executable)
    try:
        session = "019933a1-6e43-7102-a520-b3c8294cc763"
        environment["CODEX_HOME"] = str(directory / "profile")
        environment["VNM_CODEX_EXPORT_SQLITE_HOME"] = str(directory / "original-store")
        start = {"hook_event_name": "SessionStart", "session_id": session,
            "source": "clear", "cwd": str(directory / "non-git folder" / "nested"),
            "resume_record": {"native_store": "untrusted hook field"}}
        delivery = deliver(start)
        body = delivery["event"]["body"]
        assert body["cwd"] == start["cwd"]
        expected_record = {"provider_session_id": session, "native_store": {"version": 1,
            "codex_home": environment["CODEX_HOME"], "sqlite_home": environment["VNM_CODEX_EXPORT_SQLITE_HOME"]}}
        assert body["resume_record"] == expected_record
        classified = adapter.classify(delivery, attention=False)
        assert classified["observations"][0]["resume_record"] == expected_record
        assert adapter.classify(delivery, attention=False) == classified

        model_start = {**start, "model": "gateway/model-current"}
        model_delivery = deliver(model_start)
        model_result = adapter.classify(model_delivery, model=True)
        update = model_result["model_state"]["updates"][0]
        assert update["model"] == model_start["model"] and not update.get("provider")
        assert update["provider_session_id"] == session and update["agent_id"] == ""
        assert update["transition_nonce"] == model_delivery["transition_nonce"] and update["sequence"] == "0"
        assert "model_state" not in adapter.classify(model_delivery)
        for invalid_model in ("m" * (48 * 1024), {"nested": "m" * (48 * 1024)}, "", None, 17):
            malformed = deliver({**model_start, "model": invalid_model})
            body = json.dumps(malformed["event"]["body"], ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            assert len(body) <= 48 * 1024, "Optional model must fit the host's helper-ingress body limit"
            result = adapter.classify(malformed, model=True)
            assert result["classification"] == "transition" and len(result["observations"]) == 1
            assert result["model_state"]["failure"]["reason_code"] == "invalid_model_observation"
            assert "model_state" not in adapter.classify(malformed)

        del environment["VNM_CODEX_EXPORT_SQLITE_HOME"]
        assert "resume_record" not in deliver(start)["event"]["body"]
        compact = deliver({**start, "source": "compact"})
        ignored = adapter.classify(compact, attention=False)
        assert ignored["classification"] == "ignore" and ignored["observations"] == []
        for invalid in (b'{"hook_event_name":"Stop","last_assistant_message":"\xff?"}',
                        b'{"hook_event_name":"Stop","last_assistant_message":"Which directory?'):
            result, records = run(invalid)
            assert result.returncode == 2 and result.stdout == b"" and not records
        positive = None
        for label, reply, retained, hint in cases:
            delivery = deliver({**stop, "last_assistant_message": reply})
            body = delivery["event"]["body"]
            assert "last_assistant_message" not in body, label
            assert "Earlier-detail-canary" not in json.dumps(body), label
            assert len(json.dumps(body, ensure_ascii=False).encode("utf-8")) < 1024, label
            assert ("assistant_reply_evidence" in body) == retained, label
            if retained:
                evidence = body["assistant_reply_evidence"]
                assert set(evidence) == {"scope", "context", "prefix", "suffix"}, label
                assert evidence["scope"] == "final_paragraph_edges" and evidence["context"] == "prose", label
                assert all(0 < len(evidence[edge].encode("utf-8")) <= 128 for edge in ("prefix", "suffix")), label
                if reply == long_paragraph:
                    assert evidence["prefix"].startswith("Please provide ") and evidence["suffix"].endswith("for this task.")
                if reply == unicode_paragraph:
                    assert evidence["suffix"].endswith("should I use？")
            classified = adapter.classify(delivery)
            assert ("attention_kind" in classified) == hint, label
            observation = classified["observations"][0]
            assert observation["provider_event_name"] == "Stop", label
            assert observation["provider_session_id"] == "native-session" and observation["provider_turn_id"] == "native-turn", label
            assert observation["phase_transition"] == "awaiting_input" and observation["phase_evidence"] == "confirmed", label
            assert observation["outcome_transition"] == "none", label
            assert adapter.classify(delivery) == classified, label
            assert adapter.classify(delivery, attention=False)["observations"][0]["attention_transition"] == "none", label
            assert "assistant_reply_evidence" not in json.dumps(classified), label
            if hint:
                assert classified["attention_kind"] == {"version": 1, "observation_index": 0, "kind": "conversational_hint"}
                assert observation["attention_transition"] == "requested" and observation["attention_evidence"] == "inferred"
                assert observation["attention_request_id"]
                assert adapter.classify(delivery, epoch=3)["observations"][0]["attention_request_id"] != observation["attention_request_id"]
                positive = delivery
            else:
                assert observation["attention_transition"] == "none" and observation["attention_request_id"] == "", label

        assert positive is not None
        for extra in ({"agent_id": "child"}, {"agent_id": ""}, {"hook_event_name": "Interrupt"}):
            delivery = deliver({**stop, "last_assistant_message": question, **extra})
            assert "assistant_reply_evidence" not in delivery["event"]["body"]
            assert "last_assistant_message" not in delivery["event"]["body"]
            assert "attention_kind" not in adapter.classify(delivery)
        supplied = positive["event"]["body"]["assistant_reply_evidence"]
        delivery = deliver({**stop, "assistant_reply_evidence": supplied})
        assert "assistant_reply_evidence" not in delivery["event"]["body"]
        for malformed in (None, question, {**supplied, "scope": "complete_message"},
                          {**supplied, "context": "code"}, {**supplied, "prefix": "x" * 129},
                          {**supplied, "suffix": ""}, {**supplied, "extra": True}):
            delivery = json.loads(json.dumps(positive))
            delivery["event"]["body"]["assistant_reply_evidence"] = malformed
            assert "attention_kind" not in adapter.classify(delivery)
        delivery["event"]["body"].pop("assistant_reply_evidence")
        delivery["event"]["body"]["last_assistant_message"] = question
        assert "attention_kind" not in adapter.classify(delivery)

        request_id = adapter.classify(positive)["observations"][0]["attention_request_id"]
        answer = deliver({"hook_event_name": "UserPromptSubmit", "session_id": "native-session",
            "turn_id": "next-turn", "prompt": "Use the chosen directory."})
        resolved = adapter.classify(answer, pending=request_id)
        assert resolved["observations"][0]["attention_transition"] == "resolved"
        assert resolved["observations"][0]["attention_request_id"] == request_id
        assert "attention_kind" not in adapter.classify(answer)
    finally:
        adapter.close()
    model_source_contract(executable, directory, environment, deliver)


if __name__ == "__main__":
    helper, wrapper, executable = (Path(value).resolve() for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="codex-reply-") as temporary:
        reply_contract(helper, wrapper, executable, Path(temporary))
    print("Codex helper-to-adapter reply evidence checks passed.")
