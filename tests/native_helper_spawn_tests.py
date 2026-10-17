"""Exercise the production spawn/health descriptors with a private fake helper."""
import json
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

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
        assert len(data)%16==0
        for offset in range(0,len(data),16):
            sent,slow_until=struct.unpack("=dd",data[offset:offset+16])
            assert 0<=time.monotonic()-sent<1
            assert slow_until==0
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

    # Use the real recovery loop and real C++ spawn/health/instance ownership.
    # Only this disposable process may be signalled; CDP and OpenVR stay fake.
    (root / "bridge/result.json").unlink()
    source = str(Path(sys.argv[2]).resolve())
    (root / "bridge/native_bridge.py").write_text('''import importlib.util,json,os,socket
from pathlib import Path
spec=importlib.util.spec_from_file_location("production_bridge", SOURCE)
bridge=importlib.util.module_from_spec(spec);spec.loader.exec_module(bridge)
class FakeCDP:
    def evaluate(self,expression):
        if "const bridge =" in expression: return True
        if ".bridge.step(" in expression:
            return dict(version=1,request=1,open=False,eligible=False,active=False)
        return None
    def close(self): pass
def no_guard(): raise OSError("isolated fixture")
bridge.run(socket.socket(fileno=3),FakeCDP,os.getppid(),no_guard,4)
result=Path(__file__).with_name("result.tmp")
result.write_text(json.dumps(dict(closed=True)))
result.replace(result.with_name("result.json"))
'''.replace("SOURCE", repr(source)))
    started = time.monotonic()
    result = subprocess.run([str(binary), "--stalled-main-fixture", str(root / "instance")],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == -signal.SIGTERM, result.stdout + result.stderr
    assert 3 <= time.monotonic() - started < 8, "a live watchdog must not postpone main-loop recovery"
    deadline = time.monotonic() + 2
    while not (root / "bridge/result.json").is_file() and time.monotonic() < deadline:
        time.sleep(.01)
    assert json.loads((root / "bridge/result.json").read_text()) == dict(closed=True)
    result = subprocess.run([str(binary), "--instance-fixture", str(root / "instance")],
                            capture_output=True, text=True, timeout=6)
    assert result.returncode == 0, result.stdout + result.stderr
    print("Stalled main loop recovered with a live watchdog; helper exited and relaunch acquired the instance lock.")
