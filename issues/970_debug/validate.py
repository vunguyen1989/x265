"""Validate the one-frame source-instrumented capture against bytes and FFmpeg."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
w = root / 'issues/970_worklog/step2'
commands = []
def run(argv, name, env=None):
    r = subprocess.run(argv, cwd=root, env=env, capture_output=True, text=True)
    commands.append(dict(argv=argv, exit_code=r.returncode))
    (w/name).write_text(r.stdout+r.stderr)
    (w/'2.1.1_validation_commands.json').write_text(json.dumps(commands, indent=2)+'\n')
    assert r.returncode == 0, (name, r.stderr)
    return r.stdout+r.stderr

def split_nals(data):
    starts = list(re.finditer(b'\x00\x00\x00\x01|\x00\x00\x01',data))
    return [data[m.start():(starts[i+1].start() if i+1<len(starts) else len(data))]
            for i,m in enumerate(starts)]

log = (root/'x264.log').read_text()
lines = log.splitlines()
assert '[PROCESS] STOP result=0' in lines[-1]
assert [int(re.search(r'event=(\d+)', x)[1]) for x in lines] == list(range(1,len(lines)+1))
stream = w/'2.1.1_launch.h264'
headers = run(['ffmpeg','-nostdin','-hide_banner','-i',str(stream),'-map','0:v:0','-c:v','copy',
               '-bsf:v','trace_headers','-f','null','-'], '2.1.1_launch_headers.log')
run(['ffmpeg','-nostdin','-v','error','-i',str(stream),'-f','null','-'], '2.1.1_launch_decode.log')
meta = json.loads(run(['ffprobe','-v','error','-count_frames','-select_streams','v:0',
    '-show_entries','stream=codec_name,width,height,pix_fmt,nb_read_frames','-of','json',str(stream)],'2.1.1_launch_decoded.json'))
assert meta['streams'][0] == dict(codec_name='h264',width=640,height=360,pix_fmt='yuv420p',nb_read_frames='1')
assert headers.count('Frame Packing Arrangement') == 1
fields = []
end = 0
for name, value, bit_end in re.findall(r'\[FPA-FIELD\] name=(\w+) value=(\d+) bit_end=(\d+)', log):
    parsed = re.findall(r'\b'+name+r'\s+([01]+) = (\d+)', headers)
    assert len(parsed)==1 and int(parsed[0][1])==int(value), name
    assert len(parsed[0][0]) == int(bit_end)-end, name
    fields.append(dict(name=name,value=int(value),bits=parsed[0][0],start=end,end=int(bit_end)))
    end=int(bit_end)
assert len(fields)==18 and end==50
payload = bytes.fromhex(re.search(r'stage=fpa-payload\].*hex=(.*)',log)[1])
assert payload == bytes.fromhex('81 81 00 00 00 01 20')
encoded_fields = ''.join(x['bits'] for x in fields)+'100000'
assert int(encoded_fields,2).to_bytes(7,'big') == payload
nals = split_nals(stream.read_bytes())
assert len(nals)==5
sizes = [int(n) for n in re.findall(r'ENCAPSULATE_DONE.*sizeBytes=(\d+)',log)]
assert [len(n) for n in nals] == sizes
assert sizes == [29,8,575,15,712]
assert nals[3] == bytes.fromhex('00 00 01 06 2d 07 81 81 00 00 03 00 01 20 80')
assert stream.stat().st_size == sum(sizes) == 1339
assert 'RETURN bytes=1339 nal_count=5' in log and 'WRITE_DONE sizeBytes=1339' in log
# Same binary/arguments without the runtime logger must give identical bytes.
config = json.loads((root/'issues/970_debug/launch.json').read_text())['configurations'][0]
config = json.loads(json.dumps(config).replace('${workspaceFolder}',str(root)))
args = config['args'][:]; args[args.index('-o')+1] = str(w/'2.1.1_trace_off.h264')
env = dict(os.environ); env.pop('X264_FLOW_LOG',None)
run([config['program'],*args], '2.1.1_trace_off_encode.log',env)
assert (w/'2.1.1_trace_off.h264').read_bytes()==stream.read_bytes()
assert (root/'x264.log').read_text()==log
before = split_nals((w/'2.1.1_before_instrumentation.h264').read_bytes())
assert len(before)==5 and all(before[i]==nals[i] for i in [0,1,3,4])
assert b'3192M' in nals[2] and b'3192M' not in before[2]
# Separate pixel check against the earlier unmodified encoder capture.
for path,name in [(stream,'2.1.1_launch.framemd5'),(w/'2.1.1_before_instrumentation.h264','2.1.1_before.framemd5')]:
    run(['ffmpeg','-nostdin','-v','error','-i',str(path),'-f','framemd5','-'],name)
assert (w/'2.1.1_launch.framemd5').read_bytes()==(w/'2.1.1_before.framemd5').read_bytes()
shutil.copy2(root/'x264.log',w/'2.1.1_x264.log')
result=dict(events=len(lines),thread_ids=sorted(set(re.findall(r'thread id=(\d+)',log))),
    fields=fields, payload=payload.hex(' '), nal_sizes=sizes,bytes=sum(sizes),
    sha256=hashlib.sha256(stream.read_bytes()).hexdigest(),
    parser_decode='PASS',trace_on_off_identical=True,
    before_after='SPS/PPS/FPA/IDR identical; user SEI gains M in encoder version; decoded pixel identical')
(w/'2.1.1_launch_validation.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:v for k,v in result.items() if k!='fields'},indent=2))
