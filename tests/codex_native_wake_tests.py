"""Opt-in native queue gate with a disposable profile and loopback-only model.

Exercises the original embedded App Server engine used by the TUI, not terminal
rendering or the host mailbox path. No live user profile or model credential is
used. Run with --codex <native executable> --driver <codex_session_wake_tests>.
"""

import argparse
import json
import os
import pathlib
import queue
import subprocess
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

class Native:
    def __init__(self, program, home, cwd, args=(), *, environment_overrides=None):
        environment = {k: v for k, v in os.environ.items() if not k.upper().startswith(('CODEX_', 'OPENAI_', 'LOGONOMIC_', 'VNM_CONTROL', 'OTEL_'))}
        environment['CODEX_HOME'] = str(home)
        if environment_overrides:
            environment.update(environment_overrides)
        self.process = subprocess.Popen([program, *args, 'app-server'], cwd=cwd, env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding='utf-8',
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
        self.events = queue.Queue()
        self.history = []
        self.errors = []
        self.next_id = 1
        threading.Thread(target=self.read, daemon=True).start()
        threading.Thread(target=lambda: self.errors.extend(self.process.stderr), daemon=True).start()
        try:
            self.call('initialize', {'clientInfo': {'name': 'isolated-wake-test', 'version': '1'}, 'capabilities': {'experimentalApi': True}})
            self.send({'method': 'initialized'})
        except Exception:
            self.close()
            raise

    def read(self):
        try:
            for line in self.process.stdout:
                event = json.loads(line)
                self.history.append(event)
                self.events.put(event)
        except Exception as error:
            self.events.put({'read_error': str(error)})

    def send(self, value):
        self.process.stdin.write(json.dumps(value) + '\n')
        self.process.stdin.flush()

    def wait(self, predicate, timeout=30):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                event = self.events.get(timeout=min(0.1, end-time.monotonic()))
            except queue.Empty:
                if self.process.poll() is not None:
                    raise RuntimeError('Native exited: ' + ''.join(self.errors)[-1500:])
                continue
            if predicate(event):
                return event
        raise RuntimeError('Native timeout; recent methods=' + repr([e.get('method', str(e.get('id'))) for e in self.history[-20:]]) + '; diagnostics=' + ''.join(self.errors)[-1500:])

    def call(self, method, params, timeout=30):
        identity = self.next_id
        self.next_id += 1
        self.send({'id': identity, 'method': method, 'params': params})
        result = self.wait(lambda event: event.get('id') == identity, timeout)
        if 'error' in result:
            raise RuntimeError(method + ': ' + json.dumps(result['error']))
        return result['result']

    def close(self):
        if self.process.poll() is None:
            self.process.stdin.close()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(5)

class Model(BaseHTTPRequestHandler):
    requests = queue.Queue()
    gates = queue.Queue()
    def log_message(self, *_): pass
    def do_POST(self):
        value = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.requests.put(value)
        try: gate = self.gates.get_nowait()
        except queue.Empty: gate = None
        if gate is not None: gate.wait(45)
        response = {'id': 'resp_wake', 'object': 'response', 'status': 'completed', 'output': [
            {'id': 'msg_wake', 'type': 'message', 'role': 'assistant', 'status': 'completed',
             'content': [{'type': 'output_text', 'text': 'ISOLATED_WAKE_OK', 'annotations': []}]}],
            'usage': {'input_tokens': 1, 'output_tokens': 1, 'total_tokens': 2}}
        events = [
            {'type': 'response.created', 'response': {'id': 'resp_wake', 'status': 'in_progress', 'output': []}},
            {'type': 'response.output_item.done', 'output_index': 0, 'item': response['output'][0]},
            {'type': 'response.completed', 'response': response}]
        body = ''.join('event: '+event['type']+'\ndata: '+json.dumps(event)+'\n\n' for event in events).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except OSError: pass  # An interrupted native turn closes its HTTP request.

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--codex', required=True)
    parser.add_argument('--driver', required=True)
    parser.add_argument('--scratch-root')
    options = parser.parse_args()
    version = subprocess.run([options.codex, '--version'], capture_output=True, text=True, check=True).stdout.strip()
    print('native:', version, flush=True)
    with tempfile.TemporaryDirectory(prefix='codex-native-wake-', dir=options.scratch_root, ignore_cleanup_errors=True) as temporary:
        root = pathlib.Path(temporary)
        home = root / 'profile'
        home.mkdir()
        store = root / 'original-store'
        store.mkdir()
        other = root / 'other-store'
        other.mkdir()
        server = ThreadingHTTPServer(('127.0.0.1', 0), Model)
        server.daemon_threads = True
        threading.Thread(target=server.serve_forever, daemon=True).start()
        config = f'''model = "wake-fixture"
model_provider = "fixture"
sqlite_home = {json.dumps(str(other))}
[model_providers.fixture]
name = "Isolated fixture"
base_url = "http://127.0.0.1:{server.server_port}/v1"
wire_api = "responses"
requires_openai_auth = false
supports_websockets = false
[profiles.wake]
sqlite_home = {json.dumps(str(store))}
'''
        (home/'config.toml').write_text(config, encoding='utf-8')
        processes = []
        try:
            pinned = ['-c', 'sqlite_home='+json.dumps(str(store))]
            original = Native(options.codex, home, root, pinned)
            processes.append(original)
            thread = original.call('thread/start', {'cwd': str(root), 'model': 'wake-fixture', 'modelProvider': 'fixture',
                'approvalPolicy': 'never', 'sandbox': 'read-only'})['thread']['id']
            print('created isolated original thread', flush=True)
            original.call('turn/start', {'threadId': thread, 'input': [{'type': 'text', 'text': 'Seed isolated saved conversation', 'text_elements': []}]})
            original.wait(lambda event: event.get('method') == 'turn/completed', 35)
            print('seed completed', flush=True)
            writer = Native(options.codex, home, root, pinned)
            processes.append(writer)
            notification = 'isolated-idle-notification'
            ack = writer.call('thread/queue/add', {'threadId': thread, 'clientUserMessageId': notification,
                'input': [{'type': 'text', 'text': 'Respond with ISOLATED_WAKE_OK', 'text_elements': []}]})
            assert ack['queuedSubmission']['clientUserMessageId'] == notification
            writer.close()
            original.wait(lambda event: event.get('method') == 'turn/completed', 35)
            assert any('ISOLATED_WAKE_OK' in json.dumps(event) for event in original.history)
            print('PASS: external writer closed; original idle process completed queued turn', flush=True)
            production = subprocess.run([options.driver, '--native', options.codex,
                str(home), str(root), thread, str(store)], capture_output=True, text=True, timeout=35)
            assert production.returncode == 0, production.stdout + production.stderr
            print('production receipt:', production.stdout.strip(), flush=True)
            original.wait(lambda event: event.get('method') == 'turn/completed', 35)
            print('PASS: production preparation/inspect/notify woke original process', flush=True)
            while not Model.requests.empty(): Model.requests.get_nowait()
            decoy = Native(options.codex, home, root)
            processes.append(decoy)
            assert not decoy.call('thread/queue/list', {'threadId': thread})['data']
            writer = Native(options.codex, home, root, pinned)
            processes.append(writer)
            gate = threading.Event()
            Model.gates.put(gate)
            original.call('turn/start', {'threadId': thread, 'input': [{'type': 'text', 'text': 'Held busy turn', 'text_elements': []}]})
            Model.requests.get(timeout=15)
            busy = writer.call('thread/queue/add', {'threadId': thread, 'clientUserMessageId': 'isolated-busy-notification',
                'input': [{'type': 'text', 'text': 'Queued after busy', 'text_elements': []}]})
            time.sleep(1)
            assert any(item['id'] == busy['queuedSubmission']['id'] for item in writer.call('thread/queue/list', {'threadId': thread})['data'])
            assert Model.requests.empty(), 'busy recipient was steered'
            assert not decoy.call('thread/queue/list', {'threadId': thread})['data'], 'queue crossed store boundary'
            gate.set()
            original.wait(lambda event: event.get('method') == 'turn/completed', 20)
            original.wait(lambda event: event.get('method') == 'turn/completed', 20)
            Model.requests.get(timeout=5)
            print('PASS: busy waits; original continues after completion; unrelated store untouched', flush=True)
            gate = threading.Event()
            Model.gates.put(gate)
            active = original.call('turn/start', {'threadId': thread, 'input': [{'type': 'text', 'text': 'Held interrupted turn', 'text_elements': []}]})['turn']['id']
            Model.requests.get(timeout=15)
            paused = writer.call('thread/queue/add', {'threadId': thread, 'clientUserMessageId': 'isolated-paused-notification',
                'input': [{'type': 'text', 'text': 'Must remain paused', 'text_elements': []}]})
            original.call('turn/interrupt', {'threadId': thread, 'turnId': active})
            original.wait(lambda event: event.get('method') == 'turn/completed', 20)
            gate.set()
            time.sleep(12)
            assert Model.requests.empty(), 'interrupted conversation restarted'
            assert any(item['id'] == paused['queuedSubmission']['id'] for item in writer.call('thread/queue/list', {'threadId': thread})['data'])
            print('PASS: interruption preserves queued input and does not restart the turn', flush=True)
            print('request methods:', sorted({event.get('method', '') for event in original.history}), flush=True)
        finally:
            for process in reversed(processes): process.close()
            server.shutdown()

if __name__ == '__main__': main()
