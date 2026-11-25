#!/usr/bin/env python3
"""Download pinned GitHub files, verify SHA-256 and run the C++ language detector corpus test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True, help='Directory for downloaded sources (outside the repository)')
    parser.add_argument('--test-binary', type=Path, required=True, help='Built test_code_language executable')
    args = parser.parse_args()
    manifest = Path(__file__).with_name('code_language_corpus.json').read_bytes()
    entries = json.loads(manifest)
    args.cache.mkdir(parents=True, exist_ok=True)
    for entry in entries:
        name = entry['file']
        if Path(name).name != name:
            raise ValueError(f'Invalid corpus basename: {name}')
        target = args.cache / name
        if target.exists():
            data = target.read_bytes()
        else:
            url = entry['url']
            if not url.startswith('https://raw.githubusercontent.com/'):
                raise ValueError(f'Unexpected corpus URL: {url}')
            print(f'Download {url}', flush=True)
            request = urllib.request.Request(url, headers={'User-Agent': 'utun-code-language-test'})
            with urllib.request.urlopen(request, timeout=60) as response:
                data = response.read()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise ValueError(f'SHA-256 mismatch: {target}')
        if not target.exists():
            target.write_bytes(data)
    manifest_path = args.cache / 'manifest.json'
    manifest_path.write_bytes(manifest)
    environment = dict(os.environ, CODE_LANGUAGE_CORPUS=str(manifest_path.resolve()))
    print(f'Verified {len(entries)} files; running full files and three 20-line windows each.', flush=True)
    return subprocess.run([str(args.test_binary.resolve()), 'corpus'], env=environment).returncode


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f'Corpus failed: {error}', file=sys.stderr)
        sys.exit(1)
