#!/usr/bin/env python3
"""Summarize IWM32.OS2 WRND32$ diagnostics without third-party modules."""

from __future__ import print_function

import collections
import re
import sys

GENMAC = re.compile(
    r"genmac fn ([0-9a-f]+) oid ([0-9a-f]+) len (\d+) "
    r"rc (-?\d+) done ([0-9a-f]+) st ([0-9a-f]+)", re.I
)
INIT = re.compile(
    r"init load rc (-?\d+) stage (\d+) sections (\d+) bytes (\d+) "
    r"dst ([0-9a-f]+) aliveirq (\d+)(?: csr ([0-9a-f]+) "
    r"fh ([0-9a-f]+) load ([0-9a-f]+))?", re.I
)


def main(path):
    by_oid = collections.Counter()
    outcomes = collections.Counter()
    functions = collections.Counter()
    init_lines = []
    total = 0

    with open(path, "r") as source:
        for line in source:
            total += 1
            match = GENMAC.search(line)
            if match:
                function, oid, length, rc, done, status = match.groups()
                functions[function.lower()] += 1
                by_oid[oid.lower()] += 1
                outcomes[(oid.lower(), int(rc), status.lower())] += 1
            match = INIT.search(line)
            if match:
                init_lines.append(match.groups())

    print("lines:", total)
    print("genmac requests:", sum(functions.values()))
    print("functions:", " ".join("%s=%d" % item for item in sorted(functions.items())))
    print("\nOID outcomes:")
    for (oid, rc, status), count in sorted(
            outcomes.items(), key=lambda item: (-item[1], item[0])):
        print("%5d  oid=%s rc=%d status=%s" % (count, oid, rc, status))

    print("\nINIT loads:")
    if not init_lines:
        print("(none)")
    for values in init_lines:
        rc, stage, sections, byte_count, dst, alive, csr, fh, load = values
        print("rc=%s stage=%s sections=%s bytes=%s dst=%s alive=%s "
              "csr=%s fh=%s load=%s" %
              (rc, stage, sections, byte_count, dst, alive,
               csr or "unavailable", fh or "unavailable", load or "unavailable"))

    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("usage: iwm-log-summary.py iwm.txt", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
