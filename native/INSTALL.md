# gtts for macOS

Requires an Apple Silicon Mac, macOS 13 or later, and Xcode Command Line Tools.
On macOS 26.4 or later the signed helper attempts to run converted Mach-O
libraries in-process; older versions and systems where the startup check fails
use Hypervisor.framework.
From the extracted source directory:

```sh
./native/install.sh
# If ~/.local/bin is not already on PATH, add it to your shell configuration.
gtts fetch
gtts voices
gtts speak --text 'Hello there.' --out hello.wav
```

The installer defaults to `~/.local`; set `PREFIX=/some/directory` to choose
another prefix. To uninstall the CLI, use the same prefix with
`./native/install.sh --uninstall`. This leaves downloaded data and any separately
installed LaunchAgent in place.

`gtts fetch` downloads and verifies Google's ChromeOS TTS package and pinned
Ubuntu arm64 C++ runtime packages, then converts and ad-hoc signs Mach-O dylibs
locally. It defaults to every supported language; use
`--lang en-us` (repeat for more) to select packs. Data lives in
`~/Library/Application Support/google-tts-local` by default. Set `GTTS_DATA` or
pass `--data-dir PATH` to change it; the flag takes precedence. The fetch needs
network access, but synthesis is local after downloading. The Linux runtime
libraries are used inside the compatibility layer; see `native/licenses/NOTICE.md`.

Other commands include `batch` (one JSON job per stdin line) and `serve` (HTTP
`/health`, `/voices`, `/speak`). Use `gtts COMMAND --help` for options. The server
binds to `127.0.0.1` by default, limits concurrent clients and request size, and
has no authentication. `--host 0.0.0.0` explicitly exposes it on the network.
Worker operations time out after 10 minutes; set `GTTS_WORKER_TIMEOUT_MS` to a
value from 100 to 3600000 to change that limit. One response is limited to
128 MiB of float audio (about 23 minutes); split longer text across requests.
To run it as a user LaunchAgent:

```sh
gtts-service install                 # after gtts fetch
gtts-service uninstall               # stop and remove the service, retain data
```

`gtts-service generate --output /path/to/service.plist` writes a plist without
loading it. `--data-dir`, `--gtts`, `--host`, and `--port` override service settings. Service
logs are in `~/Library/Logs/google-tts-local`.

The engine helper is ad-hoc signed with Apple's custom x18 ABI and hypervisor
entitlements; it is not notarized. The CLI itself needs neither entitlement.
`GTTS_NATIVE_BACKEND=macho` requires the converted libraries and the custom ABI
startup check; `GTTS_NATIVE_BACKEND=inprocess` selects the in-process ELF
fallback, and `GTTS_NATIVE_BACKEND=vm` selects the hypervisor fallback. When
the converted files are missing, changed, or no longer match the installed
helper or the downloaded sources, automatic selection uses the in-process ELF fallback. Run
`gtts fetch` again to regenerate the converted set. The original downloads remain
available for the fallbacks.

The compatibility layer is experimental, and long inputs are split
into bounded segments, which can alter pauses. Some uncommon format strings and
fixed-address guest mappings are not implemented. In-process guest requests for
new executable mappings are rejected. The Mach-O backend maps signed code with
dyld; the in-process ELF fallback prepares executable pages itself. An unsupported
import stops the worker instead of passing an
incompatible call to macOS. The user-installed
script copies third-party notices into `$PREFIX/share/google-tts-local/licenses`.
