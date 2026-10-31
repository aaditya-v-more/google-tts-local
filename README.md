<div align="center">

<h1>google-tts-local</h1>

<p>
  <b>The Android Google voice, on your Mac, at 3× speed.</b><br>
  Google's own on-device TTS engine, running locally on macOS.
</p>

<p>
  <a href="https://github.com/aaditya-v-more/google-tts-local/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/aaditya-v-more/google-tts-local?style=for-the-badge&logo=github&logoColor=white&labelColor=111827&color=0D9488"></a>
  <a href="https://github.com/aaditya-v-more/google-tts-local/releases"><img alt="Total downloads" src="https://img.shields.io/github/downloads/aaditya-v-more/google-tts-local/total?style=for-the-badge&logo=github&logoColor=white&label=Downloads&labelColor=111827&color=0D9488"></a>
  <img alt="macOS Apple Silicon" src="https://img.shields.io/badge/macOS-Apple%20Silicon-0D9488?style=for-the-badge&logo=apple&logoColor=white&labelColor=111827">
  <img alt="Windows x64" src="https://img.shields.io/badge/Windows-x64-0D9488?style=for-the-badge&logo=windows&logoColor=white&labelColor=111827">
  <img alt="Docker arm64 and amd64" src="https://img.shields.io/badge/Docker-arm64%20%7C%20amd64-0D9488?style=for-the-badge&logo=docker&logoColor=white&labelColor=111827">
  <a href="https://github.com/aaditya-v-more/google-tts-local/stargazers"><img alt="Stars" src="https://img.shields.io/github/stars/aaditya-v-more/google-tts-local?style=for-the-badge&logo=github&logoColor=white&labelColor=111827&color=0D9488"></a>
  <a href="LICENSE"><img alt="MIT licence" src="https://img.shields.io/github/license/aaditya-v-more/google-tts-local?style=for-the-badge&labelColor=111827&color=0D9488"></a>
  <a href="https://ko-fi.com/aadityavmore"><img alt="Support this on Ko-fi" src="https://img.shields.io/badge/Ko--fi-support-FF5E5B?style=for-the-badge&logo=kofi&logoColor=white&labelColor=111827"></a>
</p>

<p>
  <a href="#install"><b>Install</b></a>
  &nbsp;·&nbsp;
  <a href="#windows"><b>Windows</b></a>
  &nbsp;·&nbsp;
  <a href="#speaking"><b>Speaking</b></a>
  &nbsp;·&nbsp;
  <a href="#docker"><b>Docker</b></a>
  &nbsp;·&nbsp;
  <a href="https://github.com/aaditya-v-more/google-tts-local/releases"><b>Releases</b></a>
</p>

</div>

If you listen to long books with text-to-speech, you probably know the US
English female voice from Android's *Speech Services by Google* (`sfg`).
It stays clear when you speed it up, where most voices turn to mush.
Google ships its engine for Android and ChromeOS. This runs the ChromeOS engine
locally on a Mac or a Windows PC and gives you a command line and a small HTTP
server. Everything is offline once the engine and voices are downloaded. A Docker
option is also available for Linux and Intel machines.

## Install

The native version needs an Apple Silicon Mac, macOS 13 or later, and Xcode
Command Line Tools. On macOS 26.4 or later it runs in-process when the startup
check passes; older macOS versions use Hypervisor.framework as a fallback.

Install with Homebrew:

```sh
brew install aaditya-v-more/tap/google-tts-local
```

Or install from source:

```sh
git clone https://github.com/aaditya-v-more/google-tts-local
cd google-tts-local
./native/install.sh
# Add ~/.local/bin to your PATH if it is not already there.
```

Then fetch the engine and voices:

```sh
gtts fetch
gtts voices
```

**The repository and release contain no Google engine or voice files.**
`fetch` downloads the ChromeOS TTS package (about 110 MB) from Google's mirror,
verifies it, and downloads pinned Ubuntu runtime libraries. It converts the
libraries locally and stores them with the voice packs in
`~/Library/Application Support/google-tts-local`. Google's terms apply to its
files. Use `gtts fetch --lang en-us --lang ja-jp` to fetch selected packs, and
set `GTTS_DATA` or pass `--data-dir` to choose another data directory.

## Windows

The Windows version runs natively on 64-bit Windows 11 (Windows 10 1803 or
later should also work), with no Docker, WSL or administrator rights. It has the
same commands and the same HTTP API as the Mac version.

Download `google-tts-local-<version>-windows-x64.zip` from
[Releases](https://github.com/aaditya-v-more/google-tts-local/releases), extract
it, and run the installer from that folder in PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1 -Fetch -StartAtLogin
```

This copies `gtts.exe` to `%LOCALAPPDATA%\Programs\google-tts-local`, adds it to
your PATH, downloads the engine and voices, and starts the HTTP server now and
whenever you sign in. Leave out `-Fetch` or `-StartAtLogin` if you don't want
them, and use `-Uninstall` to remove it again (the downloaded voices are kept).

Then, in a new terminal:

```powershell
gtts voices
gtts speak --text "Hello there." --out hello.wav
Get-Content chapter.txt | gtts speak --rate 2.5 --out fast.wav
```

Text on stdin must be UTF-8. PowerShell 7 sends it that way; in Windows
PowerShell 5.1, use `--text` or `cmd /c "gtts speak --out fast.wav < chapter.txt"`.

`gtts fetch` downloads Google's package with the `curl` and `tar` that come with
Windows, checks its SHA-256, and keeps the x86-64 engine and the voice packs.
It also fetches LLVM's C++ runtime from Ubuntu's archive, again checked by
SHA-256. Data lives in `%LOCALAPPDATA%\google-tts-local`; set `GTTS_DATA` or pass
`--data-dir` to use another folder.

To manage the background server yourself:

```powershell
gtts service install     # start gtts serve at sign-in, and now
gtts service status
gtts service uninstall   # stop it and remove the sign-in entry
```

The server runs as `gttsw.exe`, a build without a console window, and logs to
`serve.log` in the data folder. It listens on `127.0.0.1:5591` by default
(`--host` and `--port` change that). On its own it uses about 2 MB of memory;
each voice pack in use adds a worker of about 140 MB, which stops after ten
idle minutes. A short sentence comes back in well under 0.1 s once the pack is
loaded, and loading a pack takes about 0.6 s. `speak` and `batch` use a running
server automatically, as on the Mac; set `GTTS_NO_SERVER=1` to always start a
local engine.

To build it yourself, install [MSYS2](https://www.msys2.org) with its UCRT64
GCC and make (`pacman -S mingw-w64-ucrt-x86_64-gcc make`), then run
`.\windows
elease.ps1`. The zip appears in `dist`.

## Speaking

```sh
gtts speak --out hello.wav --text "Hello there."
gtts speak --rate 2.5 --out fast.wav < chapter.txt
```

| Option | Default | |
|---|---|---|
| `--voice` | `en-us-x-multi/sfg` | Any id from `gtts voices` |
| `--rate` | `1.0` | Speed. `2.5` is two and a half times faster |
| `--pitch` | `1.0` | |
| `--out` | | WAV file, 24 kHz mono |

For many files at once, `batch` reads one JSON object per line and keeps the
engine loaded between them:

```sh
gtts batch <<'EOF'
{"text": "Chapter one.", "out": "001.wav", "rate": 2.5}
{"text": "Chapter two.", "out": "002.wav", "rate": 2.5}
EOF
```

## Voices

Google's package has six language packs, and each pack holds several
speakers: 51 voices in all.

| Pack | Language | Voices |
|---|---|---|
| `en-us-x-multi` | English (US) | 17, including `sfg` |
| `es-es-x-multi` | Spanish (Spain) | 8 |
| `fr-fr-x-multi` | French (France) | 7 |
| `hi-in-x-multi` | Hindi | 6 |
| `ja-jp-x-multi` | Japanese | 6 |
| `nl-nl-x-multi` | Dutch | 7 |

`gtts voices` lists every id with its language and gender. A voice id is
the pack and the speaker, such as `en-us-x-multi/sfg` or `fr-fr-x-multi/fra`:

```sh
gtts voices
gtts speak --voice fr-fr-x-multi/fra --out bonjour.wav --text "Bonjour."
```

`sfg` is the one Android uses as its default US English female voice.
To save space, fetch only the packs you want: `gtts fetch --lang en-us --lang ja-jp`.

### A newer voice pack from your phone

Android's Google TTS app updates its voice packs more often than ChromeOS.
If you copy a `.zvoice` file off your phone, either pass it with `--pack`, or
copy it into the data directory's `voices` folder under a new name
(for example `en-us-x-phone.zvoice`), where its speakers join the list as
`en-us-x-phone/...`:

```sh
gtts speak --pack my-phone-pack.zvoice --speaker sfg --out hello.wav --text "Hello."
```

## HTTP server

```sh
gtts serve
# In another terminal:
curl -X POST localhost:5591/speak -d '{"text": "Hello.", "rate": 2}' -o hello.wav
```

`GET /voices` lists every voice as `id`, `name`, `language` and `gender`.
`POST /speak` takes `text`, and optionally `voice` (default
`en-us-x-multi/sfg`), `rate` and `pitch`. It returns a WAV file, or raw 32-bit
float samples at 24 kHz with `"format": "f32"`. `GET /health` answers once the
server is up.

The engine holds one pack per process, so the server runs a worker process for
each pack the first time one of its voices is used, and stops it after ten idle
minutes. The native server listens on `127.0.0.1:5591` by default and has no
authentication; choose another address with `--host` only if you intend to
expose it.

To start the server automatically for your user after running `gtts fetch`:

```sh
gtts-service install
gtts-service uninstall   # stops the service; keeps downloaded data
```

[Earmark](https://github.com/aaditya-v-more/earmark), an EPUB reader for the
Mac, lists these voices in its voice menu when this server is running.

## Docker

For Linux and Intel Macs, use the Docker version. On macOS, Docker Desktop,
OrbStack and Colima all work. Build the container from the source checkout, or load an image from
[Releases](https://github.com/aaditya-v-more/google-tts-local/releases):

```sh
docker build -t google-tts-local .
./gtts fetch
./gtts speak --text "Hello there." --out hello.wav
```

The repository's `./gtts` script runs Docker with the `gtts-data` volume and
the current folder mounted. To expose the container's HTTP server on localhost:

```sh
docker run -d --name gtts -p 127.0.0.1:5591:5591 -v gtts-data:/data google-tts-local serve
```

The native installation keeps its data separately from the Docker volume; run
`gtts fetch` after switching to the native version.

## How it works

ChromeOS speaks with `libchrometts.so`, a Linux library that Chrome loads
through a small C API: install a voice pack, initialise a pipeline, then pass
text and read back float samples. On macOS, `fetch` converts the engine and
its Linux C++ runtime libraries into signed Mach-O dylibs. A compatibility layer
handles the Linux interfaces and loads the converted code with dyld. The
in-process ELF loader and the hypervisor remain available as fallbacks. The
Docker version loads the Linux library with Python's `ctypes`.

On Windows, `gtts.exe` loads the x86-64 engine and the C++ runtime into its own
memory and links them itself. The Linux C library calls they make are answered
by Windows code that uses the System V calling convention, Linux structure
layouts and Linux error numbers, so the engine's own code runs unchanged on the
processor. The only changes made to it are rewriting the stack-protector reads
from `%fs:0x28`, which Windows does not provide, to read `%gs:0x30` instead, and
making its five raw Linux system calls report that they are unsupported.

## Not affiliated with Google

This project is not made, endorsed or supported by Google. It ships no
Google code or voice data. "Google" and "Speech Services by Google" are
Google's trademarks.

## Licence

The project code is [MIT](LICENSE); see the [third-party notices](native/licenses/NOTICE.md)
for the bundled decoder and downloaded runtime libraries. The engine and voices
you download with `fetch` are Google's and come with Google's terms.

If this saves your ears, you can [buy me a coffee on Ko-fi](https://ko-fi.com/aadityavmore).
