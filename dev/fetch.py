#!/usr/bin/env python3
"""Download a release tarball and keep it only if its sha256 matches.

    fetch.py <url> <sha256> <output>
"""
import hashlib
import sys
import time
import urllib.request
from pathlib import Path


def download(url):
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=120) as response:
                return response.read()
        except OSError as error:
            if attempt == 3:
                raise SystemExit(f"cannot fetch {url}: {error}")

            time.sleep(2 ** attempt)


def main():
    url, expected, output = sys.argv[1:4]
    data = download(url)
    actual = hashlib.sha256(data).hexdigest()

    if actual != expected:
        raise SystemExit(f"{url}: sha256 {actual}, expected {expected}")

    target = Path(output)
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + ".part")
    partial.write_bytes(data)
    partial.replace(target)


if __name__ == "__main__":
    main()
