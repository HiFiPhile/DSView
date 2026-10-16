#!/usr/bin/env python3
"""Summarize dsldecode output (JSON lines) without printing all of it.

  dsl_ann.py OUT.jsonl                     counts per decoder and annotation
  dsl_ann.py OUT.jsonl -a ANN[,ANN...]     those annotations, one line each
  dsl_ann.py OUT.jsonl -a data --bytes     their values as one byte string

--from/--to keep annotations starting in a time window (seconds from the
start of the capture), --pd keeps one decoder of a --all run, -n limits the
lines printed and -w their width. Exit status 1 if the decode failed.
"""
import argparse
import collections
import json
import sys


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("file", help="dsldecode output, one JSON object per line")
    p.add_argument("-a", "--ann", help="comma-separated annotation ids to list")
    p.add_argument("--pd", help="decoder id to keep")
    p.add_argument("--from", dest="t0", type=float, help="start of the time window, s")
    p.add_argument("--to", dest="t1", type=float, help="end of the time window, s")
    p.add_argument("-n", "--limit", type=int, default=50, help="lines to print (default 50)")
    p.add_argument("-w", "--width", type=int, default=160, help="text width (default 160)")
    p.add_argument("--bytes", action="store_true", help="print the values of -a as bytes")
    a = p.parse_args()
    if a.bytes and not a.ann:
        p.error("--bytes needs -a")

    anns = set(a.ann.split(",")) if a.ann else None
    counts = collections.Counter()
    first, last = {}, {}
    done, errors, values = None, [], []
    matched = 0
    with open(a.file, encoding="utf-8") as f:
        for line in f:
            if not line.strip():
                continue
            r = json.loads(line)
            if "error" in r:
                errors.append(r)
                continue
            if r.get("done"):
                done = r
                continue
            t = r.get("t", 0.0)
            if (a.pd and r["pd"] != a.pd) or (a.t0 is not None and t < a.t0) or \
                    (a.t1 is not None and t > a.t1):
                continue
            if anns is None:
                key = (r["pd"], r["ann"])
                counts[key] += 1
                first.setdefault(key, t)
                last[key] = t
            elif r["ann"] in anns:
                matched += 1
                if a.bytes:
                    if "value" in r:
                        values.append(r["value"])
                elif matched <= a.limit:
                    text = (r.get("texts") or [""])[0]
                    if len(text) > a.width:
                        text = text[:a.width - 3] + "..."
                    print(f"{t:.9f} {r['pd']} {r['ann']}: {text}")

    for e in errors:
        print("ERROR", json.dumps(e))
    if anns is None and counts:
        print(f"{'count':>8} {'first_s':>12} {'last_s':>12}  decoder/annotation")
        for (pd, ann), n in sorted(counts.items(), key=lambda kv: first[kv[0]]):
            print(f"{n:8d} {first[(pd, ann)]:12.6f} {last[(pd, ann)]:12.6f}  {pd}/{ann}")
    elif a.bytes:
        if all(0 <= v < 256 for v in values):
            print(bytes(values))
        else:
            print(values)
        print(f"({len(values)} values)")
    elif matched > a.limit:
        print(f"... {matched - a.limit} more (of {matched}); narrow with --from/--to or raise -n")
    if done:
        print(f"done: {done['samples']} samples at {done['samplerate']} Hz, "
              f"{done['annotations']} annotations")
    else:
        print("incomplete: no done line, the decode failed or was cut short")
    return 1 if errors or not done else 0


if __name__ == "__main__":
    sys.exit(main())
