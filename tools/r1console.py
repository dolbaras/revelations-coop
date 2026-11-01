"""Test console client for the Revelations proxy (REV1_CONSOLE=<port>, UDP 127.0.0.1).

  python -I r1console.py [-p 28200] [-t 3] ping version "readmem 400000 16"
  echo shot | python -I r1console.py -p 28200 -        (commands from stdin, one per line)
Prints the answer to each command. Exit code 2 when a command got no answer.
"""
import argparse
import socket
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", type=int, default=28200)
    ap.add_argument("-t", "--timeout", type=float, default=3.0)
    ap.add_argument("commands", nargs="+", help="commands; a single '-' reads them from stdin")
    a = ap.parse_args()
    cmds = [l.strip() for l in sys.stdin if l.strip()] if a.commands == ["-"] else a.commands
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(a.timeout)
    rc = 0
    for c in cmds:
        s.sendto(c.encode("utf-8"), ("127.0.0.1", a.port))
        try:
            data, _ = s.recvfrom(65535)
            print(data.decode("utf-8", "replace"))
        except socket.timeout:
            print("(no answer to %r)" % c)
            rc = 2
    return rc


if __name__ == "__main__":
    sys.exit(main())
