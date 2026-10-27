#!/usr/bin/env python3
"""A development shell for the shell integration protocol (docs/integration.md).

It launches the keyboard in hosted mode on a socket pair, greets it, keeps the
lease alive with pings, acknowledges sink events, and prints what arrives.
Type a request as JSON, or one of the shortcuts below; `id` is added for you.

    tools/shell-test.py [--keyboard PATH] [--sink] [-- extra keyboard arguments]

Shortcuts: show [view], hide, state, recenter, sink on|off, context NAME [purpose],
pointer forwarded|native, quit. Sink text is printed only with --print-text:
it is what the user typed.
"""
import argparse
import json
import os
import select
import socket
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--keyboard", default=os.path.expanduser("~/.local/bin/framekeyboard"))
    parser.add_argument("--sink", action="store_true", help="turn the text sink on after hello")
    parser.add_argument("--print-text", action="store_true", help="print committed sink text")
    parser.add_argument("--lease-ms", type=int, default=2000)
    parser.add_argument("extra", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    shell, keyboard = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
    extra = args.extra[1:] if args.extra[:1] == ["--"] else args.extra
    command = [args.keyboard, "--vr", "--hosted", str(keyboard.fileno()), *(extra or ["--input", "ei", "--start-enabled"])]
    process = subprocess.Popen(command, pass_fds=(keyboard.fileno(),))
    keyboard.close()
    print("launched:", " ".join(command), flush=True)
    next_id = 0
    buffer = b""

    def send(message):
        nonlocal next_id
        if "id" not in message and message.get("op") not in ("pointer", "ack", "reject", "quiesced"):
            next_id += 1
            message["id"] = next_id
        shell.sendall(json.dumps(message).encode() + b"\n")
        return message.get("id")

    def shortcut(line):
        words = line.split()
        if not words:
            return None
        if words[0] == "show":
            return {"op": "show", "view": words[1] if len(words) > 1 else "keyboard"}
        if words[0] in ("hide", "state", "recenter", "profiles.list", "settings.get", "reload"):
            return {"op": words[0]}
        if words[0] == "sink":
            return {"op": "sink", "on": len(words) < 2 or words[1] == "on"}
        if words[0] == "context":
            hints = {"purpose": words[2]} if len(words) > 2 else {}
            return {"op": "context", "context": words[1] if len(words) > 1 else None, "hints": hints}
        if words[0] == "pointer":
            return {"op": "pointer_mode", "mode": words[1] if len(words) > 1 else "native"}
        return json.loads(line)

    greeted = False
    last_ping = time.monotonic()
    try:
        while process.poll() is None:
            readable, _, _ = select.select([shell, sys.stdin], [], [], .25)
            if shell in readable:
                data = shell.recv(65536)
                if not data:
                    print("the keyboard closed the connection", flush=True)
                    break
                buffer += data
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    message = json.loads(line)
                    event = message.get("event")
                    if event == "ready" and not greeted:
                        greeted = True
                        send({"op": "hello", "protocol": 1, "client": "Test shell", "client_version": "0",
                              "lease_ms": args.lease_ms})
                        send({"op": "subscribe", "events": ["visibility", "geometry", "theme", "selection",
                                                             "settings", "dictation", "target", "haptic"]})
                        if args.sink:
                            send({"op": "sink", "on": True})
                            send({"op": "context", "context": "test", "hints": {"purpose": "text"}})
                    if event in ("commit", "key"):
                        send({"op": "ack", "context": message["context"], "seq": message["seq"]})
                        if event == "commit" and not args.print_text:
                            message["text"] = f"<{len(message['text'])} characters>"
                    if message.get("id") is not None and message.get("ok") and set(message) == {"id", "ok"}:
                        continue  # bare acknowledgements, pings included
                    print("<", json.dumps(message), flush=True)
            if sys.stdin in readable:
                line = sys.stdin.readline()
                if not line or line.strip() == "quit":
                    break
                try:
                    request = shortcut(line.strip())
                    if request:
                        print(">", json.dumps(request), "id", send(request), flush=True)
                except (ValueError, IndexError) as error:
                    print("not understood:", error, flush=True)
            if greeted and time.monotonic() - last_ping > args.lease_ms / 3000:
                send({"op": "ping"})
                last_ping = time.monotonic()
    except KeyboardInterrupt:
        pass
    finally:
        shell.close()  # the keyboard shuts down as Settings → Quit does
        try:
            print("keyboard exited with", process.wait(timeout=10), flush=True)
        except subprocess.TimeoutExpired:
            process.terminate()
            print("keyboard terminated", flush=True)


if __name__ == "__main__":
    main()
