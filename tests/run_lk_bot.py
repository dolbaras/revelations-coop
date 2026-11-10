#!/usr/bin/env python3
"""The lobby -> start -> load barrier scenario of the network core over REAL UDP (ENet) on this PC, with a netem.py relay that adds delay, loss,
duplication and reordering between two headless bots in --lk mode (coop_bot.exe --lk: the same lobby_link.c the game's `net` command and lobby rows use).

    python -I tests/run_lk_bot.py [--profiles clean,wan,loss15]

Each profile: the host bot plays the PARTNER (role pick), sends its settings (episode 1, difficulty 2, flags 3, area, seed); the joining bot is ready, the host
starts by itself, three loads follow (the section start and two checkpoint loads started by the joiner), then states flow. Checks on both sides: roles swapped
as picked, settings equal on both, three barrier openings (go = 3, last epoch 3), samples of the other player received. Exit code 0 = everything passed.
(The same scenario on the in-process sim transport is tests/test_netcore.c; the game's side is tried live with `net host` / `net join` and these bots.)
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BOT = os.path.join(HERE, "build", "coop_bot.exe")
NETEM = os.path.join(HERE, "netem.py")

PROFILES = {
    "clean": ["--delay", "0"],
    "wan": ["--delay", "40", "--jitter", "15", "--loss", "5", "--reorder", "5", "--dup", "5"],
    "loss15": ["--delay", "30", "--jitter", "10", "--loss", "15", "--burst", "3", "--dup", "3"],
}


def run(name, port, netem_args, timeout=60):
    procs = []
    try:
        relay = subprocess.Popen([sys.executable, "-I", NETEM, "--listen", "127.0.0.1:%d" % (port + 1), "--to", "127.0.0.1:%d" % port, "--seconds", str(timeout + 10),
                                  "--stats", "0", "--seed", "11"] + netem_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(relay)
        time.sleep(0.6)
        host = subprocess.Popen([BOT, "host", "--lk", "--port", str(port), "--role", "partner", "--episode", "1", "--difficulty", "2", "--flags", "3", "--area", "91010100", "--seed", "beef",
                                 "--seconds", "5", "--loads", "3"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(host)
        time.sleep(0.5)
        client = subprocess.Popen([BOT, "join", "127.0.0.1:%d" % (port + 1), "--lk", "--seconds", "5", "--loads", "3", "--load-gap", "1.5"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(client)
        res = {}
        t0 = time.time()
        for label, p in (("client", client), ("host", host)):
            try:
                out, err = p.communicate(timeout=max(1, timeout - (time.time() - t0)))
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                res[label] = {"error": "timeout", "stderr": err[-300:]}
                continue
            line = [l for l in out.splitlines() if l.startswith("RESULT ")]
            res[label] = json.loads(line[-1][7:]) if line else {"error": "no result", "stderr": err[-300:]}
            res[label]["exit"] = p.returncode
        return res
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
        for p in procs:
            try:
                p.wait(timeout=3)
            except Exception:
                pass


def check(res):
    problems = []
    h, c = res.get("host", {}), res.get("client", {})
    for label, r in (("host", h), ("client", c)):
        if "error" in r or r.get("exit") != 0:
            problems.append("%s: %s" % (label, r))
    if problems:
        return problems
    if (h["role"], h["remote_role"], c["role"], c["remote_role"]) != ("partner", "lead", "lead", "partner"):
        problems.append("roles: host %s/%s client %s/%s" % (h["role"], h["remote_role"], c["role"], c["remote_role"]))
    for label, r in (("host", h), ("client", c)):
        if (r["started"], r["episode"], r["difficulty"], r["flags"], r["area"], r["seed"]) != (1, 1, 2, 3, "91010100", "0000beef"):
            problems.append("%s: settings %s" % (label, r))
        if r["go"] != 3 or r["last_go_epoch"] != 3 or r["epoch"] != 3 or r["phase"] != 3:
            problems.append("%s: barrier go %s last epoch %s epoch %s phase %s (expected 3, 3, 3, 3)" % (label, r["go"], r["last_go_epoch"], r["epoch"], r["phase"]))
        if r["samples"] < 100:
            problems.append("%s: only %s samples of the other player" % (label, r["samples"]))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profiles", default=",".join(PROFILES))
    ap.add_argument("--port", type=int, default=28440)
    args = ap.parse_args()
    if not os.path.exists(BOT):
        print("build the bot first: sh tests/build.sh")
        return 2
    failed = 0
    port = args.port
    for name in args.profiles.split(","):
        res = run(name, port, PROFILES[name])
        port += 4
        problems = check(res)
        print("%-8s %s" % (name, "OK" if not problems else "FAIL: " + "; ".join(problems)))
        failed += bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
