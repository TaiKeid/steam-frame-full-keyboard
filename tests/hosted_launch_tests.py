"""A hosted launch's descriptor checks (docs/integration.md, Launch), run
against the real binary. Each case fails before the instance lock, VR or
input are touched, so nothing here can reach a running keyboard."""
import os
import socket
import subprocess
import sys
import unittest

BINARY = sys.argv.pop(1)


def run(arguments, pass_fds=()):
    return subprocess.run([BINARY, *arguments], pass_fds=pass_fds, capture_output=True, text=True, timeout=20)


class HostedLaunch(unittest.TestCase):
    def test_protocols_are_listed(self):
        result = run(["--protocols"])
        self.assertEqual((result.returncode, result.stdout.split()), (0, ["1"]))

    def test_invalid_descriptors_exit_with_status_10(self):
        self.assertEqual(run(["--vr", "--hosted", "nope"]).returncode, 10)
        self.assertEqual(run(["--vr", "--hosted", "2"]).returncode, 10, "never a standard stream")
        self.assertEqual(run(["--vr", "--hosted", "77"]).returncode, 10, "a descriptor that is not open")
        read, write = os.pipe()
        try:
            self.assertEqual(run(["--vr", "--hosted", str(read)], pass_fds=(read,)).returncode, 10, "a pipe")
        finally:
            os.close(read)
            os.close(write)
        datagram, other = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            result = run(["--vr", "--hosted", str(datagram.fileno())], pass_fds=(datagram.fileno(),))
            self.assertEqual(result.returncode, 10, "a datagram socket")
        finally:
            datagram.close()
            other.close()

    def test_hosted_needs_vr(self):
        keyboard, shell = socket.socketpair()
        try:
            for mode in (["--preview"], ["--vr", "--autostart"]):
                result = run([*mode, "--hosted", str(keyboard.fileno())], pass_fds=(keyboard.fileno(),))
                self.assertEqual(result.returncode, 10, mode)
        finally:
            keyboard.close()
            shell.close()


if __name__ == "__main__":
    unittest.main()
