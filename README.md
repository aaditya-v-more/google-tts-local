<div align="center">

<h1>google-tts-local</h1>

<p>
  <b>The Android Google voice, on your Mac, at 3× speed.</b><br>
  Google's own on-device TTS engine, in a small Docker container.
</p>

<p>
  <a href="https://github.com/aaditya-v-more/google-tts-local/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/aaditya-v-more/google-tts-local?style=for-the-badge&logo=github&logoColor=white&labelColor=111827&color=0D9488"></a>
  <a href="https://github.com/aaditya-v-more/google-tts-local/releases"><img alt="Total downloads" src="https://img.shields.io/github/downloads/aaditya-v-more/google-tts-local/total?style=for-the-badge&logo=github&logoColor=white&label=Downloads&labelColor=111827&color=0D9488"></a>
  <img alt="Docker arm64 and amd64" src="https://img.shields.io/badge/Docker-arm64%20%7C%20amd64-0D9488?style=for-the-badge&logo=docker&logoColor=white&labelColor=111827">
  <a href="https://github.com/aaditya-v-more/google-tts-local/stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/aaditya-v-more/google-tts-local?style=for-the-badge&logo=github&logoColor=white&labelColor=111827&color=0D9488"></a>
  <a href="LICENSE"><img alt="MIT licence" src="https://img.shields.io/github/license/aaditya-v-more/google-tts-local?style=for-the-badge&labelColor=111827&color=0D9488"></a>
  <a href="https://ko-fi.com/aadityavmore"><img alt="Support this on Ko-fi" src="https://img.shields.io/badge/Ko--fi-support-FF5E5B?style=for-the-badge&logo=kofi&logoColor=white&labelColor=111827"></a>
</p>

<p>
  <a href="#install"><b>Install</b></a>
  &nbsp;·&nbsp;
  <a href="#speaking"><b>Speaking</b></a>
  &nbsp;·&nbsp;
  <a href="https://github.com/aaditya-v-more/google-tts-local/releases"><b>Releases</b></a>
</p>

</div>

If you listen to long books with text-to-speech, you probably know the US
English female voice from Android's *Speech Services by Google* (`sfg`).
It stays clear when you speed it up, where most voices turn to mush.
Google only ships its engine for Android and ChromeOS, so there has been no
way to use it on a Mac.

This runs that engine, the same one ChromeOS uses, in a Linux container and
gives you a command line and a small HTTP server. Everything is offline once
the voices are downloaded.

## Install

You need Docker (Docker Desktop, OrbStack or Colima all work). Apple Silicon
and Intel are both supported.

Download the image for your machine from
[Releases](https://github.com/aaditya-v-more/google-tts-local/releases/latest)
and load it:

```sh
docker load -i google-tts-local-0.1.0-arm64.tar.gz   # Apple Silicon
docker load -i google-tts-local-0.1.0-amd64.tar.gz   # Intel
```

Or build it yourself:

```sh
git clone https://github.com/aaditya-v-more/google-tts-local
cd google-tts-local
docker build -t google-tts-local .
```

Then fetch the engine and voices:

```sh
./gtts fetch
```

**The image contains none of Google's files.** `fetch` downloads Google's
ChromeOS TTS package (about 110 MB) straight from Google's ChromeOS mirror,
checks its SHA-256, and keeps only the engine and the English voices in a
Docker volume called `gtts-data`. Google's terms apply to those files.

`gtts` is a short script that runs the container with that volume and your
current folder mounted. Copy it somewhere on your `PATH` if you like.

## Speaking

```sh
./gtts speak --out hello.wav --text "Hello there."
./gtts speak --rate 2.5 --out fast.wav < chapter.txt
```

| Option | Default | |
|---|---|---|
| `--speaker` | `sfg` | Voice name, see below |
| `--rate` | `1.0` | Speed. `2.5` is two and a half times faster |
| `--pitch` | `1.0` | |
| `--out` | | WAV file, 24 kHz mono |

For many files at once, `batch` reads one JSON object per line and keeps the
engine loaded between them:

```sh
./gtts batch <<'EOF'
{"text": "Chapter one.", "out": "001.wav", "rate": 2.5}
{"text": "Chapter two.", "out": "002.wav", "rate": 2.5}
EOF
```

## Voices

The English pack has 17 speakers: `hol`, `iob`, `iod`, `iof`, `iog`, `ioh`,
`ioj`, `iol`, `iom`, `iop`, `jmp`, `sfg`, `tfe`, `tme`, `tpc`, `tpd`, `tpf`.
`sfg` is the one Android uses as its default US English female voice.

Other languages in the package are Spanish, French, Hindi, Japanese and Dutch:

```sh
./gtts fetch --lang en-us --lang fr-fr
./gtts speak --pack /data/voices/fr-fr-x-multi.zvoice --speaker fra --out bonjour.wav --text "Bonjour."
```

An unknown `--speaker` prints the names that pack has.

### A newer voice pack from your phone

Android's Google TTS app updates its voice packs more often than ChromeOS.
If you copy an `en-us-x-multi` `.zvoice` file off your phone, put it in the
current folder and pass it with `--pack`:

```sh
./gtts speak --pack my-phone-pack.zvoice --out hello.wav --text "Hello."
```

## HTTP server

```sh
docker run -d --name gtts -p 127.0.0.1:5591:5591 -v gtts-data:/data google-tts-local serve
curl -X POST localhost:5591/speak -d '{"text": "Hello.", "rate": 2}' -o hello.wav
```

`POST /speak` takes `text`, and optionally `speaker`, `gender`, `rate` and
`pitch`. It returns a WAV file, or raw 32-bit float samples at 24 kHz with
`"format": "f32"`. `GET /health` answers once the voices are loaded.
Requests are spoken one at a time.

## How it works

ChromeOS speaks with `libchrometts.so`, a Linux library that Chrome loads
through a small C API: install a voice pack, initialise a pipeline, then pass
text and read back float samples. The container loads that library with
Python's `ctypes` and calls the same functions.

The library is linked with compact relocations (`DT_RELR`). Current glibc
only loads such libraries if they declare the `GLIBC_ABI_DT_RELR` version,
which ChromeOS's build leaves out, so `fetch` adds that one entry with
[LIEF](https://lief.re). Nothing else in the library changes.

## Not affiliated with Google

This project is not made, endorsed or supported by Google. It ships no
Google code or voice data. "Google" and "Speech Services by Google" are
Google's trademarks.

## Licence

The code here is [MIT](LICENSE). The engine and voices you download with
`fetch` are Google's and come with Google's terms.

If this saves your ears, you can [buy me a coffee on Ko-fi](https://ko-fi.com/aadityavmore).
