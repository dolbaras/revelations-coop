#!/usr/bin/env python3
"""Network tests of the co-op transport and protocol: two headless bots (coop_bot.exe, the real ENet transport and session code) talk
over real UDP on this PC, with a netem.py relay between them that adds delay, jitter, loss, bursts, reordering and duplication.

    python -I tests/run_net_tests.py [--profiles clean,wan,...] [--slow]

Each profile runs a full session: connect, lobby (client asks for the partner role), start, simulated load, 12 s of 20 Hz state in both
directions, a mid-game level load started by the client (load barrier), an event, leave. The bots measure the position error of
the interpolated remote character against its known path. --slow adds the cases that wait for ENet's timeouts (a bot that dies
without a goodbye).  Exit code 0 = everything passed.
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

# name: (netem args, max error units, min share of the sequence that arrives, max mean error)
PROFILES = {
    "clean": (["--delay", "0"], 0.5, 0.99, 0.1),
    "lan": (["--delay", "2", "--jitter", "4"], 1.0, 0.99, 0.1),
    "wan": (["--delay", "40", "--jitter", "15", "--loss", "5", "--reorder", "5", "--dup", "5"], 4.0, 0.85, 0.3),
    # the maximum is one outage of several lost states in a row (the puppet holds still after 250 ms of extrapolation): a loose bound; the mean is the real criterion
    "loss15": (["--delay", "30", "--jitter", "10", "--loss", "15", "--burst", "3", "--dup", "3"], 90.0, 0.70, 1.5),
    "harsh": (["--delay", "80", "--jitter", "40", "--loss", "20", "--burst", "2", "--reorder", "10", "--dup", "10"], 150.0, 0.55, 5.0),
}


def run_pair(name, port, netem_args, host_extra=(), client_extra=(), via_relay=True, timeout=60, kill_after=None):
    relay = None
    join_port = port
    procs = []
    try:
        if via_relay:
            join_port = port + 1
            relay = subprocess.Popen([sys.executable, "-I", NETEM, "--listen", "127.0.0.1:%d" % join_port, "--to", "127.0.0.1:%d" % port,
                                      "--seconds", str(timeout + 10), "--stats", "0", "--seed", "11"] + netem_args,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            procs.append(relay)
            time.sleep(0.6)
        host = subprocess.Popen([BOT, "host", "--port", str(port), "--seconds", "15", "--event-at", "5"] + list(host_extra),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(host)
        time.sleep(0.5)
        client = subprocess.Popen([BOT, "join", "127.0.0.1:%d" % join_port, "--char", "partner", "--seconds", "14", "--load-at", "5", "--event-at", "10"] + list(client_extra),
                                  stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(client)
        results = {}
        t0 = time.time()
        for label, p in (("client", client), ("host", host)):
            try:
                out, err = p.communicate(timeout=max(1, timeout - (time.time() - t0)))
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                results[label] = {"error": "timeout", "stderr": err[-300:]}
                continue
            line = [l for l in out.splitlines() if l.startswith("RESULT ")]
            if line:
                results[label] = json.loads(line[-1][7:])
                results[label]["exit"] = p.returncode
            else:
                results[label] = {"error": "no result", "exit": p.returncode, "stderr": err[-300:]}
        return results
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
        for p in procs:
            try:
                p.wait(timeout=3)
            except Exception:
                pass


def check_profile(name, res, max_err, min_share, max_mean):
    problems = []
    h, c = res.get("host", {}), res.get("client", {})
    for label, r in (("host", h), ("client", c)):
        if "error" in r or r.get("exit") != 0:
            problems.append("%s: %s" % (label, r))
            continue
        if r["phase"] != 3:
            problems.append("%s ended in phase %s" % (label, r["phase"]))
        if r["started"] != 1:
            problems.append("%s: START count %s" % (label, r["started"]))
        if r["go"] < 2:
            problems.append("%s: barrier opened %s times (expected 2: start + mid-game load)" % (label, r["go"]))
        if r["max_epoch"] != 2:
            problems.append("%s: epochs reached %s (expected 2)" % (label, r["max_epoch"]))
        if r["err_n"] < 150:
            problems.append("%s: only %s error samples" % (label, r["err_n"]))
        elif r["err_max"] > max_err:
            problems.append("%s: position error max %.2f > %.1f" % (label, r["err_max"], max_err))
        elif r["err_mean"] > max_mean:
            problems.append("%s: position error mean %.3f > %.2f" % (label, r["err_mean"], max_mean))
        if r["bad"]:
            problems.append("%s: %s malformed packets" % (label, r["bad"]))
    if not problems:
        # delivered share of the states that were sent while both sides were in the level: accepted / (accepted + gaps in the sequence)
        for label, me in (("host", h), ("client", c)):
            got = me["rx_state"] / max(1, me["rx_state"] + me["gap"])
            if got < min_share:
                problems.append("%s received only %.0f%% of the sequence (< %.0f%%)" % (label, 100 * got, 100 * min_share))
        if c.get("events") != 1 or h.get("events") != 1:
            problems.append("events: host got %s, client got %s (expected 1 each)" % (h.get("events"), c.get("events")))
        if c.get("character") != "partner" or h.get("character") != "lead":
            problems.append("characters: host %s client %s" % (h.get("character"), c.get("character")))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profiles", default=",".join(PROFILES))
    ap.add_argument("--slow", action="store_true")
    ap.add_argument("--port", type=int, default=28420)
    args = ap.parse_args()
    if not os.path.exists(BOT):
        print("build the bot first: sh tests/build.sh")
        return 2
    failed = 0
    port = args.port
    print("%-8s %-6s %7s %7s %8s %8s %6s %6s %6s %6s %s" % ("profile", "side", "tx", "rx", "gap", "dup/old", "errmax", "errmean", "rtt", "events", "verdict"))
    for name in args.profiles.split(","):
        netem_args, max_err, share, max_mean = PROFILES[name]
        res = run_pair(name, port, netem_args)
        port += 4
        problems = check_profile(name, res, max_err, share, max_mean)
        for label in ("host", "client"):
            r = res.get(label, {})
            if "tx_state" in r:
                print("%-8s %-6s %7d %7d %8d %4d/%-4d %6.2f %7.3f %6d %6d" % (name, label, r["tx_state"], r["rx_state"], r["gap"], r["dup"], r["old"], r["err_max"], r["err_mean"], r["rtt"], r["events"]))
        print("%-8s %s" % (name, "OK" if not problems else "FAIL: " + "; ".join(problems)))
        failed += bool(problems)

    # refusal over the real transport: a different game build
    res = run_pair("build", port, [], host_extra=["--exe-tag", "a0a1a2a3a4a5a6a7"], client_extra=["--exe-tag", "1111111111111111"], via_relay=False, timeout=15)
    port += 4
    c = res.get("client", {})
    ok = c.get("refused") == 1 and c.get("code") == 2
    print("%-8s %s" % ("build-gate", "OK (client refused: %s)" % c.get("text") if ok else "FAIL: %s" % res))
    failed += not ok
    res = run_pair("sametag", port, [], host_extra=["--exe-tag", "a0a1a2a3a4a5a6a7"], client_extra=["--exe-tag", "a0a1a2a3a4a5a6a7"], via_relay=False, timeout=40)
    port += 4
    problems = check_profile("sametag", res, 1.0, 0.99, 0.1)
    print("%-8s %s" % ("tag-ok", "OK" if not problems else "FAIL: " + "; ".join(problems)))
    failed += bool(problems)

    if args.slow:
        # a client that dies without a goodbye: the host must notice through the transport timeout (ENet: 10-30 s)
        t0 = time.time()
        res = run_pair("kill", port, [], host_extra=["--seconds", "60"], client_extra=["--die-at", "6"], via_relay=False, timeout=70)
        port += 4
        h = res.get("host", {})
        ok = h.get("phase") == 3 and h.get("exit") == 0 and "error" not in h
        print("%-8s %s (%.0f s)" % ("kill", "OK: host survived and ended normally" if ok else "FAIL: %s" % res, time.time() - t0))
        failed += not ok
    print("RESULT: %s" % ("all passed" if not failed else "%d FAILED" % failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
