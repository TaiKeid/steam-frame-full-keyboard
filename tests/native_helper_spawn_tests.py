"""Exercise the production spawn/health descriptors with a private fake helper."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="framekeyboard-helper-fixture-") as directory:
    root = Path(directory)
    binary = root / "native-replacement-tests"
    shutil.copy2(sys.argv[1], binary)
    (root / "bridge").mkdir()
    (root / "bridge/native_bridge.py").write_text('''import json,os,select,socket,struct,sys,time
from pathlib import Path
assert sys.argv[1:] == ["--fd", "3", "--health-fd", "4"]
parent=socket.socket(fileno=3)
deadline=time.monotonic()+2
health=frame=replied=False
while time.monotonic()<deadline:
    readable,_,_=select.select([parent,4],[],[],.05)
    if 4 in readable:
        data=os.read(4,4096)
        assert len(data)%8==0
        for offset in range(0,len(data),8):
            sent,=struct.unpack("=d",data[offset:offset+8])
            assert 0<=time.monotonic()-sent<1
            health=True
    if parent in readable:
        data=parent.recv(32768)
        if not data or json.loads(data)["op"]=="stop":
            assert health and frame
            Path(__file__).with_name("result.json").write_text(json.dumps(dict(health=True,frame=True,closed=True)))
            break
        message=json.loads(data)
        assert message["op"]=="frame" and message["progress"]>0 and not message["ready"]
        frame=True
    if health and frame and not replied:
        parent.send(json.dumps(dict(op="state",version=1,request=1,open=False,eligible=False,active=False)).encode())
        replied=True
else:
    raise AssertionError("helper descriptor fixture timed out")
parent.close();os.close(4)
''')
    result = subprocess.run([str(binary), "--helper-fixture"], capture_output=True, text=True, timeout=8)
    assert result.returncode == 0, result.stdout + result.stderr
    assert json.loads((root / "bridge/result.json").read_text()) == dict(health=True, frame=True, closed=True)
    print(result.stdout.strip())
