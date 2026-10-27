"""Run Google's on-device TTS engine (from the ChromeOS googletts package) on Linux.

Commands:
  fetch                  download the engine and voices from Google's ChromeOS mirror into /data
  speak --out x.wav      speak --text (or stdin) to a WAV file
  batch                  one JSON object per stdin line: {"text": ..., "out": ...}
  serve                  HTTP server: POST /speak returns audio
"""
import argparse
import array
import ctypes
import hashlib
import json
import os
import platform
import re
import shutil
import struct
import sys
import tarfile
import threading
import urllib.request
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PACKAGE_URL = 'https://storage.googleapis.com/chromeos-localmirror/distfiles/googletts-35.3.tar.xz'
PACKAGE_SHA256 = '51785a17918e2c4d9aa2f4cc17ed8ee672b0127501a58b457fdd7876755d046a'
DATA = os.environ.get('GTTS_DATA', '/data')
SAMPLE_RATE = 24000
LIB_NAMES = {'aarch64': 'libchrometts_arm64.so', 'x86_64': 'libchrometts_x86_64.so'}


# ---- fetch -----------------------------------------------------------------

def add_relr_verneed(src, dst):
    """glibc >= 2.36 refuses DT_RELR objects that lack a GLIBC_ABI_DT_RELR version need; add it."""
    import lief
    elf = lief.ELF.parse(src)
    reqs = {r.name: r for r in elf.symbols_version_requirement}
    libc = reqs['libc.so.6']
    if not any(a.name == 'GLIBC_ABI_DT_RELR' for a in libc.get_auxiliary_symbols()):
        used = max(a.other for r in reqs.values() for a in r.get_auxiliary_symbols())
        aux = lief.ELF.SymbolVersionAuxRequirement()
        aux.name = 'GLIBC_ABI_DT_RELR'
        aux.hash = 0x0fd0e42
        aux.other = used + 1
        libc.add_auxiliary_requirement(aux)
    elf.write(dst)


def fetch(langs):
    os.makedirs(os.path.join(DATA, 'voices'), exist_ok=True)
    tarball = os.path.join(DATA, os.path.basename(PACKAGE_URL))
    if not os.path.exists(tarball):
        print(f'Downloading {PACKAGE_URL}', file=sys.stderr)
        urllib.request.urlretrieve(PACKAGE_URL, tarball + '.part')
        os.replace(tarball + '.part', tarball)
    digest = hashlib.sha256(open(tarball, 'rb').read()).hexdigest()
    if digest != PACKAGE_SHA256:
        os.remove(tarball)
        sys.exit(f'Checksum mismatch for {tarball}; deleted it, run fetch again.')

    lib_name = LIB_NAMES[platform.machine()]
    wanted = {lib_name: os.path.join(DATA, 'libchrometts.orig.so')}
    for lang in langs:
        wanted[f'{lang}-x-multi.zvoice'] = os.path.join(DATA, 'voices', f'{lang}-x-multi.zvoice')
    with tarfile.open(tarball) as tar:
        for member in tar.getmembers():
            if member.isfile() and member.name in wanted:
                with tar.extractfile(member) as f, open(wanted.pop(member.name), 'wb') as out:
                    shutil.copyfileobj(f, out)
    if wanted:
        sys.exit(f'Not in the package: {", ".join(sorted(wanted))}')

    add_relr_verneed(os.path.join(DATA, 'libchrometts.orig.so'), os.path.join(DATA, 'libchrometts.so'))
    os.remove(os.path.join(DATA, 'libchrometts.orig.so'))
    os.remove(tarball)
    print(f'Ready: {DATA}/libchrometts.so and {", ".join(langs)} voices', file=sys.stderr)


# ---- engine ----------------------------------------------------------------

def varint(n):
    out = bytearray()
    while n >= 0x80:
        out.append((n & 0x7F) | 0x80)
        n >>= 7
    out.append(n)
    return bytes(out)


def field_bytes(num, payload):
    return varint((num << 3) | 2) + varint(len(payload)) + payload


def field_float(num, value):
    return varint((num << 3) | 5) + struct.pack('<f', value)


def text_request(text, rate, pitch):
    # Field 3 scales duration, so speed 2.5x is sent as 0.4.
    params = field_float(2, pitch) + field_float(3, 1.0 / rate)
    body = field_bytes(1, text.encode('utf-8')) + field_bytes(20, params)
    return field_bytes(1, field_bytes(1, body))


def speaker_params(name, gender):
    return field_bytes(1, name.encode()) + field_bytes(2, gender.encode())


class Engine:
    def __init__(self, lib_path, pack_path, verbose=False):
        if not os.path.exists(lib_path):
            sys.exit(f'{lib_path} is missing. Run the fetch command first.')
        lib = ctypes.CDLL(lib_path)
        self.lib = lib
        self.lock = threading.Lock()
        log_type = ctypes.CFUNCTYPE(None, ctypes.c_int, ctypes.c_char_p)
        self.speakers = set()

        def log(sev, msg):
            text = msg.decode(errors='replace')
            self.speakers.update(re.findall(r'speaker: "([^"]+)"', text))
            if verbose:
                sys.stderr.write(f'[engine {sev}] {text}\n')
        self._logger = log_type(log)
        lib.GoogleTtsSetLogger(self._logger)
        lib.GoogleTtsPreSandboxInit()

        lib.GoogleTtsInit.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        lib.GoogleTtsInit.restype = ctypes.c_bool
        lib.GoogleTtsInstallVoice.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        lib.GoogleTtsInstallVoice.restype = ctypes.c_bool
        lib.GoogleTtsInitBuffered.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int]
        lib.GoogleTtsInitBuffered.restype = ctypes.c_bool
        lib.GoogleTtsReadBuffered.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_size_t)]
        lib.GoogleTtsReadBuffered.restype = ctypes.c_int
        lib.GoogleTtsGetFramesInAudioBuffer.restype = ctypes.c_size_t

        stamp = os.stat(pack_path)
        name = os.path.splitext(os.path.basename(pack_path))[0]
        voice_dir = os.path.join(DATA, 'installed', f'{name}-{stamp.st_size}')
        if not os.path.exists(os.path.join(voice_dir, 'pipeline.pb')):
            os.makedirs(voice_dir, exist_ok=True)
            data = open(pack_path, 'rb').read()
            if not lib.GoogleTtsInstallVoice(voice_dir.encode(), data, len(data)):
                raise RuntimeError(f'GoogleTtsInstallVoice failed for {pack_path}')
        if not lib.GoogleTtsInit(os.path.join(voice_dir, 'pipeline.pb').encode(), voice_dir.encode()):
            raise RuntimeError(f'GoogleTtsInit failed for {voice_dir}')

    def synth(self, text, speaker='sfg', gender='female', rate=1.0, pitch=1.0):
        if self.speakers and speaker not in self.speakers:
            raise ValueError(f'No speaker {speaker!r} in this pack; it has {", ".join(sorted(self.speakers))}')
        lib = self.lib
        req = text_request(text, rate, pitch)
        spk = speaker_params(speaker, gender)
        with self.lock:
            if not lib.GoogleTtsInitBuffered(req, spk, len(req), len(spk)):
                raise RuntimeError('GoogleTtsInitBuffered failed')
            frames = lib.GoogleTtsGetFramesInAudioBuffer()
            buf = (ctypes.c_float * frames)()
            written = ctypes.c_size_t(0)
            audio = array.array('f')
            while True:
                status = lib.GoogleTtsReadBuffered(buf, ctypes.byref(written))
                audio.extend(buf[:written.value])
                if status <= 0:
                    break
        if status < 0:
            raise RuntimeError(f'GoogleTtsReadBuffered returned {status}')
        return audio


def pcm16(audio):
    return array.array('h', (max(-32768, min(32767, int(s * 32767))) for s in audio)).tobytes()


def wav_bytes(audio):
    import io
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(pcm16(audio))
    return buf.getvalue()


# ---- server ----------------------------------------------------------------

def serve(engine, args):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *a):
            pass

        def reply(self, code, body, ctype):
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path == '/health':
                self.reply(200, b'{"ok": true}', 'application/json')
            else:
                self.reply(404, b'', 'text/plain')

        def do_POST(self):
            if self.path != '/speak':
                return self.reply(404, b'', 'text/plain')
            try:
                job = json.loads(self.rfile.read(int(self.headers.get('Content-Length', 0))))
                audio = engine.synth(job['text'], job.get('speaker', args.speaker), job.get('gender', args.gender),
                                     float(job.get('rate', args.rate)), float(job.get('pitch', args.pitch)))
            except Exception as e:
                return self.reply(400, json.dumps({'error': str(e)}).encode(), 'application/json')
            if job.get('format') == 'f32':
                self.reply(200, audio.tobytes(), 'audio/L32; rate=24000')
            else:
                self.reply(200, wav_bytes(audio), 'audio/wav')

    server = ThreadingHTTPServer(('0.0.0.0', args.port), Handler)
    print(f'Listening on :{args.port}', file=sys.stderr)
    server.serve_forever()


# ---- cli -------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['fetch', 'speak', 'batch', 'serve'])
    ap.add_argument('--lang', action='append', help='for fetch: en-us, es-es, fr-fr, hi-in, ja-jp, nl-nl (default en-us)')
    ap.add_argument('--lib', default=os.path.join(DATA, 'libchrometts.so'))
    ap.add_argument('--pack', default=os.path.join(DATA, 'voices', 'en-us-x-multi.zvoice'))
    ap.add_argument('--speaker', default='sfg')
    ap.add_argument('--gender', default='female')
    ap.add_argument('--rate', type=float, default=1.0, help='speed, 2.5 = 2.5x faster')
    ap.add_argument('--pitch', type=float, default=1.0)
    ap.add_argument('--text')
    ap.add_argument('--out')
    ap.add_argument('--port', type=int, default=5591)
    ap.add_argument('--verbose', action='store_true')
    args = ap.parse_args()

    if args.command == 'fetch':
        return fetch(args.lang or ['en-us'])

    engine = Engine(args.lib, args.pack, args.verbose)
    if args.command == 'serve':
        return serve(engine, args)
    if args.command == 'batch':
        for line in sys.stdin:
            if not line.strip():
                continue
            job = json.loads(line)
            audio = engine.synth(job['text'], job.get('speaker', args.speaker), job.get('gender', args.gender),
                                 job.get('rate', args.rate), job.get('pitch', args.pitch))
            open(job['out'], 'wb').write(wav_bytes(audio))
            print(json.dumps({'out': job['out'], 'seconds': round(len(audio) / SAMPLE_RATE, 2)}), flush=True)
        return
    if not args.out:
        sys.exit('speak needs --out')
    text = args.text if args.text is not None else sys.stdin.read()
    audio = engine.synth(text, args.speaker, args.gender, args.rate, args.pitch)
    open(args.out, 'wb').write(wav_bytes(audio))
    print(json.dumps({'out': args.out, 'seconds': round(len(audio) / SAMPLE_RATE, 2)}))


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError) as e:
        sys.exit(f'error: {e}')
