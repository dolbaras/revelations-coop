#!/usr/bin/env python3
"""The puppet traffic (CP_PAD) over REAL UDP (ENet) on this PC between two headless bots (coop_bot.exe --lk --pad) with a netem.py relay that adds delay, jitter, loss, duplicates and
reordering. Each bot sends a scripted 60 Hz pad through puppet1.c (dedup, three records per packet, keepalive) and runs the receiver of puppet1.c with a counting sink; the second result
line "PAD {...}" has the counters. Scenarios: link profiles (delivery share, order, delay) and the holds (the client says PAUSED for 2 s; the client goes silent for 2 s = a cut link).

    python -I tests/run_pad_bot.py [--profiles clean,wan,loss20,hold,silent]

Exit code 0 = everything passed. (The pure-logic version with a simulated link is tests/test_puppet1.c; the game's side is tried live: notes\net-puppet-impl.md, list H4.)
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

# name: (netem args, client extra args, min delivery share, max mean delay ms)
PROFILES = {
    "clean": (["--delay", "0"], [], 0.985, 40),
    "wan": (["--delay", "40", "--jitter", "15", "--loss", "5", "--reorder", "5", "--dup", "5"], [], 0.96, 150),
    "loss20": (["--delay", "30", "--jitter", "10", "--loss", "20", "--dup", "5", "--reorder", "5"], [], 0.90, 200),
    "hold": (["--delay", "0"], ["--pad-hold-at", "3", "--pad-hold-for", "2"], 0.985, 40),
    "silent": (["--delay", "0"], ["--pad-silent-at", "3", "--pad-silent-for", "2"], 0.0, 40),
}


def run(port, netem_args, client_extra, timeout=60):
    procs = []
    try:
        relay = subprocess.Popen([sys.executable, "-I", NETEM, "--listen", "127.0.0.1:%d" % (port + 1), "--to", "127.0.0.1:%d" % port, "--seconds", str(timeout + 10),
                                  "--stats", "0", "--seed", "11"] + netem_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(relay)
        time.sleep(0.6)
        host = subprocess.Popen([BOT, "host", "--lk", "--pad", "--port", str(port), "--role", "partner", "--seconds", "9", "--loads", "1"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        procs.append(host)
        time.sleep(0.5)
        client = subprocess.Popen([BOT, "join", "127.0.0.1:%d" % (port + 1), "--lk", "--pad", "--seconds", "9", "--loads", "1"] + client_extra, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
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
            lines = [l for l in out.splitlines() if l.startswith("PAD ")]
            res[label] = json.loads(lines[-1][4:]) if lines else {"error": "no PAD line", "stderr": err[-300:], "stdout": out[-300:]}
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


def check(name, res, share, max_lat):
    problems = []
    h, c = res.get("host", {}), res.get("client", {})
    for label, r in (("host", h), ("client", c)):
        if "error" in r or r.get("exit") != 0:
            problems.append("%s: %s" % (label, r))
    if problems:
        return problems
    for label, me, other in (("host", h, c), ("client", c, h)):
        if me["order_bad"]:
            problems.append("%s: %d pushed records out of order" % (label, me["order_bad"]))
        if me["wrong_epoch"]:
            problems.append("%s: %d packets of another epoch" % (label, me["wrong_epoch"]))
        if share > 0:
            got = me["rx_records"] / max(1.0, other["tx_records"] - 20.0)
            if got < share:
                problems.append("%s: received %.1f %% of the other side's %d records (< %.1f %%)" % (label, 100 * got, other["tx_records"], 100 * share))
        if me["lat_n"] and me["lat_mean"] > max_lat:
            problems.append("%s: mean delay %.1f ms > %d" % (label, me["lat_mean"], max_lat))
    if name == "hold":   # the client said PAUSED: the HOST holds its puppet of the client neutral, the client side stays running
        if h["neutral"] < 1 or h["held_dropped"] < 1:
            problems.append("hold: host neutral %s held_dropped %s (expected both > 0)" % (h["neutral"], h["held_dropped"]))
        if c["neutral"] != 0:
            problems.append("hold: the client held too (neutral %s)" % c["neutral"])
    if name == "silent":   # the client sent nothing for 2 s: the host stalls, goes neutral and recovers (2 s < the 10 s dead-man: no LOST)
        if h["stalls"] < 1 or h["neutral"] < 1 or h["link"] != "live" or h["losts"] != 0:
            problems.append("silent: host stalls %s neutral %s link %s lost %s" % (h["stalls"], h["neutral"], h["link"], h["losts"]))
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--profiles", default=",".join(PROFILES))
    ap.add_argument("--port", type=int, default=28460)
    args = ap.parse_args()
    if not os.path.exists(BOT):
        print("build the bot first: sh tests/build.sh")
        return 2
    failed = 0
    port = args.port
    for name in args.profiles.split(","):
        netem_args, extra, share, max_lat = PROFILES[name]
        res = run(port, netem_args, extra)
        port += 4
        problems = check(name, res, share, max_lat)
        for label in ("host", "client"):
            r = res.get(label, {})
            if "tx_records" in r:
                print("  %-6s %-6s tx %5d rec / %5d pk | rx %5d rec lost %4d late %4d rep %4d | neutral %3d held-dropped %4d stalls %d | delay mean %.1f max %.0f ms | session rx %d drop %d" %
                      (name, label, r["tx_records"], r["tx_packets"], r["rx_records"], r["lost"], r["late"], r["repeats"], r["neutral"], r["held_dropped"], r["stalls"], r["lat_mean"], r["lat_max"],
                       r["session_rx"], r["session_drop"]))
        print("%-8s %s" % (name, "OK" if not problems else "FAIL: " + "; ".join(problems)))
        failed += bool(problems)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
