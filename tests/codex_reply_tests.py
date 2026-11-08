"""Exercise native Stop bytes through the packaged helper and adapter, without an account."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
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

    def classify(self, delivery, *, attention=True, pending=None, epoch=2):
        self.serial += 1
        request_id = "r" + str(self.serial)
        payload = {"event": delivery["event"], "transition_nonce": delivery["transition_nonce"],
            "ledger": {"current_epoch": epoch, "current_transition": {
                "kind": "start", "provider_session_id": "native-session", "nonce": "7" * 32}}}
        if pending is not None:
            payload["pending_conversational_request_id"] = pending
        self.send({"type": "invoke", "request_id": request_id,
            "capability_id": "attention-kind" if attention else "helper-ingress",
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


if __name__ == "__main__":
    helper, wrapper, executable = (Path(value).resolve() for value in sys.argv[1:4])
    with tempfile.TemporaryDirectory(prefix="codex-reply-") as temporary:
        reply_contract(helper, wrapper, executable, Path(temporary))
    print("Codex helper-to-adapter reply evidence checks passed.")
