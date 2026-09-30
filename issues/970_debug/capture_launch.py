"""Run the exact launch.json profile through Microsoft's installed DAP adapter."""
import json
from pathlib import Path
import queue
import subprocess
import threading
import time

root = Path(__file__).resolve().parents[2]
work = root / 'issues/970_worklog/step2'
config = next(c for c in json.loads((root / '.vscode/launch.json').read_text())['configurations']
              if c['name'] == 'x264: Issue 970 - 1 frame FPA flow')
config = json.loads(json.dumps(config).replace('${workspaceFolder}', str(root)))
adapters = list((Path.home() / '.vscode-server/extensions').glob('ms-vscode.cpptools-*/debugAdapters/bin/OpenDebugAD7'))
if not adapters:
    raise SystemExit('Install the VS Code C/C++ extension (cppdbg) first.')
proc = subprocess.Popen([str(sorted(adapters)[-1])], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE, cwd=root)
messages = queue.Queue()
def read():
    while True:
        headers = {}
        while True:
            line = proc.stdout.readline()
            if not line: return
            if line == b'\r\n': break
            key, value = line.decode().split(':', 1)
            headers[key.lower()] = value.strip()
        messages.put(json.loads(proc.stdout.read(int(headers['content-length']))))
threading.Thread(target=read, daemon=True).start()
seq = 0
transcript = []
def send(command, arguments):
    global seq
    seq += 1
    msg = dict(seq=seq, type='request', command=command, arguments=arguments)
    transcript.append(msg)
    payload = json.dumps(msg).encode()
    proc.stdin.write(f'Content-Length: {len(payload)}\r\n\r\n'.encode() + payload)
    proc.stdin.flush()
send('initialize', dict(adapterID='cppdbg', clientID='970-capture', pathFormat='path',
    linesStartAt1=True, columnsStartAt1=True, supportsRunInTerminalRequest=False))
launched = False
exit_code = None
try:
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        msg = messages.get(timeout=max(0.1, deadline-time.monotonic()))
        transcript.append(msg)
        if msg['type'] == 'response':
            if not msg.get('success', False):
                raise RuntimeError(json.dumps(msg))
            if msg.get('command') == 'initialize': send('launch', config)
        if msg['type'] == 'event':
            event = msg['event']
            if event == 'initialized': send('configurationDone', {})
            elif event == 'stopped':
                reason = msg['body']['reason']
                if launched: raise RuntimeError('Unexpected stop: ' + json.dumps(msg))
                launched = True
                send('continue', {'threadId':msg['body']['threadId']})
            elif event == 'exited': exit_code = msg['body']['exitCode']
            elif event == 'terminated': break
    else: raise TimeoutError('DAP capture timed out')
finally:
    (work/'2.1.1_launch_dap.json').write_text(json.dumps(transcript, indent=2)+'\n')
    proc.terminate()
    try: proc.wait(timeout=5)
    except subprocess.TimeoutExpired: proc.kill()
    (work/'2.1.1_adapter.stderr').write_bytes(proc.stderr.read())
if exit_code != 0 or not launched: raise SystemExit(f'Inferior did not run successfully: entry={launched}, exit={exit_code}')
trace = (root/'x264.log').read_text().splitlines()
if not trace or '[PROCESS] STOP result=0' not in trace[-1]:
    raise SystemExit('Encoder trace incomplete; inspect x264.log')
print('DAP launch completed, inferior exit code 0; encoder produced x264.log')
