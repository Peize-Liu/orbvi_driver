"""Verified-exit ADB transport for the board's legacy adbd."""
import os
import re
import shlex
import shutil
import subprocess
import uuid
ADB = os.environ.get('ADB') or shutil.which('adb')
if not ADB:
    raise RuntimeError('Install Android platform-tools and add adb to PATH, or set ADB')

def run(args, **kwargs):
    return subprocess.run([str(a) for a in args], check=True, **kwargs)


def device(serial):
    result = run([ADB, 'devices'], capture_output=True, text=True)
    entries = [line.split() for line in result.stdout.splitlines()[1:] if line.strip()]
    ready = [row[0] for row in entries if len(row) >= 2 and row[1] == 'device']
    if serial:
        if serial not in ready:
            raise RuntimeError(f'Device {serial!r} is not ready. Run x5 devices.')
        return serial
    if len(ready) != 1:
        raise RuntimeError('Expected exactly one ready device; run x5 devices, connect the board, '
                           'or select it with x5 --serial SERIAL / ANDROID_SERIAL.')
    return ready[0]


def remote(adb, command, timeout=120):
    # Legacy adbd on this board does not reliably handle stdin EOF or exit status.
    marker = '__X5_EXIT_' + uuid.uuid4().hex + '__'
    wrapped = ('sh -c ' + shlex.quote(command) + '; result=$?; '
               "printf '\\n" + marker + "%s\\n' \"$result\"")
    result = run(adb + ['shell', wrapped], capture_output=True, text=True, timeout=timeout)
    match = re.search(r'\n' + re.escape(marker) + r'(\d+)\s*$', result.stdout)
    if not match:
        raise RuntimeError('ADB command ended without a verified remote exit status: ' + result.stdout)
    output = result.stdout[:match.start()]
    if int(match.group(1)):
        raise RuntimeError(f'Board command failed ({match.group(1)}):\n{output}\n{result.stderr}')
    return output
