#!/usr/bin/env python3
"""Strip the CCS-generated agent header from tracked instruction files."""

import re
import sys


CCS_AGENT_HEADER_RE = re.compile(
    br"\A<!-- DO NOT EDIT - This part is automatically generated\. -->\r?\n"
    br".*?"
    br"<!-- User instructions should be added below this line -->\r?\n(?:\r?\n)?",
    re.DOTALL,
)


def main() -> int:
    data = sys.stdin.buffer.read()
    sys.stdout.buffer.write(CCS_AGENT_HEADER_RE.sub(b"", data, count=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
