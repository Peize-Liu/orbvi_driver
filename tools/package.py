#!/usr/bin/env python3
"""Create a source/binary/evidence archive with a verified SHA256 manifest."""
import argparse
import hashlib
from pathlib import Path
import tarfile

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED = {'.git', '__pycache__', '.DS_Store', '.build', 'build', 'install'}


def digest(path):
    result = hashlib.sha256()
    with path.open('rb') as stream:
        for data in iter(lambda: stream.read(1024 * 1024), b''):
            result.update(data)
    return result.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path,
                        default=ROOT.parent / 'orbvi-driver-20261112.tar.gz')
    output = parser.parse_args().output.resolve()
    if ROOT == output or ROOT in output.parents:
        parser.error('Archive output must be outside the package directory')
    manifest = ROOT / 'MANIFEST.sha256'
    files = sorted(p for p in ROOT.rglob('*') if p.is_file()
                   and not any(part in EXCLUDED for part in p.relative_to(ROOT).parts)
                   and p != manifest)
    entries = [(digest(path), path) for path in files]
    manifest.write_text(''.join(f'{value}  {path.relative_to(ROOT).as_posix()}\n'
                                for value, path in entries))
    with tarfile.open(output, 'w:gz') as archive:
        for path in files + [manifest]:
            archive.add(path, arcname='orbvi_driver/' + path.relative_to(ROOT).as_posix(),
                        recursive=False)
    with tarfile.open(output, 'r:gz') as archive:
        for expected, path in entries:
            stream = archive.extractfile('orbvi_driver/' + path.relative_to(ROOT).as_posix())
            result = hashlib.sha256()
            for data in iter(lambda: stream.read(1024 * 1024), b''):
                result.update(data)
            if result.hexdigest() != expected:
                raise RuntimeError(f'Archive verification failed: {path}')
    checksum = digest(output)
    output.with_name(output.name + '.sha256').write_text(f'{checksum}  {output.name}\n')
    print(f'{output}\n{len(files)} files verified; {output.stat().st_size} bytes\nSHA256 {checksum}')


if __name__ == '__main__':
    main()
