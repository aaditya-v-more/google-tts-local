#!/usr/bin/python3
"""Inspect signed converted images and their dyld metadata."""
import os
import pathlib
import plistlib
import shutil
import subprocess
import sys
import tempfile

data = pathlib.Path(os.environ.get('GTTS_DATA', str(pathlib.Path.home() /
                   'Library/Application Support/google-tts-local')))
directory = pathlib.Path(os.environ.get('GTTS_MACHO_DIR', str(data / 'macho')))
if not (directory / 'metadata.plist').is_file():
    if os.environ.get('GTTS_NATIVE_BACKEND') == 'macho':
        raise SystemExit('Mach-O check failed: no converted images; run gtts fetch')
    print('Mach-O image inspection skipped: no converted set is installed.')
    sys.exit(0)

names = ['libchrometts.dylib', 'libcxx.dylib', 'libcxxabi.dylib',
         'libgcc.dylib', 'libunwind.dylib', 'libgtts-shim.dylib']
for name in names:
    path = directory / name
    if not path.is_file():
        raise SystemExit('Mach-O check failed: missing ' + str(path))
    subprocess.run(['/usr/bin/codesign', '--verify', '--strict', str(path)], check=True)
    load = subprocess.run(['/usr/bin/otool', '-l', str(path)], capture_output=True,
                          text=True, check=True).stdout
    deps = subprocess.run(['/usr/bin/otool', '-L', str(path)], capture_output=True,
                          text=True, check=True).stdout
    if name == 'libgtts-shim.dylib':
        if 'libSystem' not in deps:
            raise SystemExit('Mach-O shim is missing its system dependency')
        continue
    if ('LC_ID_DYLIB' not in load or 'LC_DYLD_INFO_ONLY' not in load or
        '__mod_init_func' in load or '__mod_term_func' in load or 'LC_ROUTINES' in load or
        'libgtts-shim.dylib' not in deps or 'libSystem' in deps):
        raise SystemExit('Mach-O load commands are incorrect: ' + name)
    info = subprocess.run(['/usr/bin/xcrun', 'dyld_info', '-segments', '-exports',
                           '-fixups', str(path)], capture_output=True, text=True, check=True).stdout
    if '_gtts_image_base' not in info or '_gtts_explicit_init' not in info or '_gtts_tls_key' not in info:
        raise SystemExit('Mach-O exports or TLS key bind are missing: ' + name)
    if name == 'libchrometts.dylib' and ('_gtts_c_GoogleTtsInit' not in info or
                                         '_native_tlsdesc' not in info):
        raise SystemExit('Mach-O engine entry or TLS descriptor is missing')
print('Mach-O image inspection: five signed dylibs and shim passed otool, dyld_info, and codesign.')

if os.environ.get('GTTS_NATIVE_BACKEND') == 'macho':
    native = pathlib.Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix='bad-macho-', dir=native / 'tmp') as temporary:
        bad = pathlib.Path(temporary)
        shutil.copyfile(directory / 'libgtts-shim.dylib', bad / 'libgtts-shim.dylib')
        with (directory / 'metadata.plist').open('rb') as stream:
            metadata = plistlib.load(stream)
        metadata['objects'][0]['name'] = 7
        with (bad / 'metadata.plist').open('wb') as stream:
            plistlib.dump(metadata, stream)
        env = dict(os.environ, GTTS_DATA=str(data), GTTS_MACHO_DIR=str(bad),
                   GTTS_NATIVE_BACKEND='', GTTS_NATIVE_TRACE='1')
        command = [str(native / 'build/gtts-engine'), 'inspect']
        automatic = subprocess.run(command, env=env, capture_output=True, text=True)
        if automatic.returncode or 'in-process ELF fallback' not in automatic.stderr or 'ELF objects' not in automatic.stdout:
            raise SystemExit('Invalid Mach-O metadata did not fall back safely: ' + automatic.stderr)
        env['GTTS_NATIVE_BACKEND'] = 'macho'
        forced = subprocess.run(command, env=env, capture_output=True, text=True)
        if not forced.returncode or 'unavailable, stale, or invalid' not in forced.stderr:
            raise SystemExit('Forced Mach-O mode accepted invalid metadata')
        shutil.copyfile(directory / 'libchrometts.dylib', bad / 'libchrometts.dylib')
        metadata['objects'][0]['name'] = 'libchrometts.dylib'
        metadata['objects'][0]['dylib_sha256'] = '0' * 64
        with (bad / 'metadata.plist').open('wb') as stream:
            plistlib.dump(metadata, stream)
        env['GTTS_NATIVE_BACKEND'] = ''
        stale = subprocess.run(command, env=env, capture_output=True, text=True)
        if stale.returncode or 'in-process ELF fallback' not in stale.stderr:
            raise SystemExit('Mismatched Mach-O image did not fall back safely: ' + stale.stderr)
    print('Mach-O metadata and image mismatch fallback: passed.')
