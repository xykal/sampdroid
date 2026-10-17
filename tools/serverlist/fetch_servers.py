#!/usr/bin/env python3
"""
serverlist — proof-of-concept "server browser" backend for the Android client.

Pulls the open.mp public server index, sorts by players online, then queries each
server's real state with tools/sampquery (SA-MP query protocol, opcode 'i').

This is exactly the data layer the launcher needs: no game, no client, no RakNet
session yet — just UDP + the query protocol.

Usage:
    python3 fetch_servers.py [--top N] [--query]

    --top N     how many servers from the index to show (default 10)
    --query     also query each server for live hostname/gamemode/player count
"""

import argparse
import concurrent.futures as futures
import json
import os
import subprocess
import sys
import urllib.request

API = "https://api.open.mp/servers"
HERE = os.path.dirname(os.path.abspath(__file__))
QUERY_BIN = os.path.join(HERE, "..", "sampquery", "sampquery")


def fetch_index():
    req = urllib.request.Request(API, headers={"User-Agent": "samp-android/0.1"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)


def query(addr):
    try:
        out = subprocess.run(
            [QUERY_BIN, addr, "i"],
            capture_output=True, text=True, timeout=6
        ).stdout
    except Exception as e:  # noqa: BLE001
        return addr, None, str(e)

    info = {}
    for line in out.splitlines():
        parts = line.strip().split(None, 1)
        if len(parts) == 2 and parts[0] in (
            "hostname", "gamemode", "language", "players", "password"
        ):
            info[parts[0]] = parts[1].strip()
    return addr, info, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--top", type=int, default=10)
    ap.add_argument("--query", action="store_true")
    args = ap.parse_args()

    servers = fetch_index()
    servers.sort(key=lambda s: s.get("pc", 0), reverse=True)
    shown = servers[: args.top]

    print(f"index: {len(servers)} servers | showing top {len(shown)}\n")

    results = {}
    if args.query:
        with futures.ThreadPoolExecutor(max_workers=8) as ex:
            futs = {ex.submit(query, s["ip"]): s for s in shown}
            for f in futures.as_completed(futs):
                addr, info, err = f.result()
                results[addr] = (info, err)

    for i, s in enumerate(shown, 1):
        addr = s["ip"]
        name = s.get("hn", "?")
        print(f"{i:2}. {addr:24} {s.get('pc', 0):4}/{s.get('pm', 0):<4} "
              f"{'omp' if s.get('omp') else 'samp':5} {name}")

        if args.query:
            info, err = results.get(addr, (None, None))
            if err or not info:
                print(f"    -> no answer ({err or 'timeout'})")
                continue
            print(f"    -> hostname:  {info.get('hostname', '?')}")
            print(f"    -> gamemode:  {info.get('gamemode', '?')}")
            print(f"    -> language:  {info.get('language', '?')}")
            print(f"    -> players:   {info.get('players', '?')}")
            print(f"    -> password:  {info.get('password', '?')}")


if __name__ == "__main__":
    sys.exit(main())
