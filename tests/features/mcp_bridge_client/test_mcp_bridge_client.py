#!/usr/bin/env python3
"""tests/features/mcp_bridge_client — see spec.md."""
import importlib.util, json, os, socket, subprocess, sys, tempfile
from typing import Any

HERE = os.path.dirname(os.path.abspath(__file__))
BRIDGE = os.path.join(HERE, "..", "..", "..", "tools", "mcp-bridge.py")
spec = importlib.util.spec_from_file_location("mcp_bridge", BRIDGE)
assert spec is not None and spec.loader is not None, BRIDGE
bridge: Any = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)

failures = 0
def check(ok, label, detail=""):
    global failures
    print(("[ ok ] " if ok else "[FAIL] ") + label + ("" if ok else f": {detail}"))
    if not ok:
        failures += 1

# INV-1
with tempfile.TemporaryDirectory() as d:
    real = os.path.join(d, "real.sock")
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(real); srv.listen(1)
    link = os.path.join(d, f"ants-terminal-mcp-{os.getpid()}")
    os.symlink(real, link)
    bridge.SOCK_GLOB = os.path.join(d, "ants-terminal-mcp-*")
    os.environ.pop("ANTS_MCP_SOCKET", None)
    picked = bridge.pick_socket()
    check(picked == "", "INV-1 picker skips a symlink named like an Ants socket", repr(picked))
    srv.close()

# INV-2
for bad in ("nan", "inf", "-1", "0"):
    os.environ["ANTS_T_PROBE"] = bad
    v = bridge._env_float("ANTS_T_PROBE", 7.0)
    check(v == 7.0, f"INV-2 timeout override {bad!r} falls back to the default", repr(v))
os.environ.pop("ANTS_T_PROBE", None)

# INV-3
env = dict(os.environ, ANTS_MCP_SOCKET="/nonexistent/ants-terminal-mcp-0")
p = subprocess.run([sys.executable, BRIDGE], input='[1,2]\n5\n', text=True,
                   capture_output=True, env=env, timeout=30)
lines = [l for l in p.stdout.splitlines() if l.strip()]
codes = []
for l in lines:
    try:
        codes.append(json.loads(l).get("error", {}).get("code"))
    except Exception:
        codes.append(None)
check(p.returncode == 0 and codes == [-32600, -32600],
      "INV-3 non-object requests get -32600 and the bridge keeps running",
      f"rc={p.returncode} codes={codes} stderr={p.stderr.strip()[-200:]}")

sys.exit(failures)
