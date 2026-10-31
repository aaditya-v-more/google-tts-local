# Third-party notices for the Windows build

`gtts.exe` and `gttsw.exe` are built with MSYS2's MinGW-w64 GCC toolchain and
link the MinGW-w64 runtime and libgcc statically. The MinGW-w64 runtime's
copyright and licence notices are in `licenses/MinGW-w64-runtime.txt`; libgcc
is covered by the GCC Runtime Library Exception.

`gtts fetch` downloads these Ubuntu 24.04 amd64 packages directly from the
Ubuntu archive and verifies each package's SHA-256, and the SHA-256 of the
library taken from it:

- LLVM libc++, libc++abi and libunwind 18.1.3-1:
  [Ubuntu source package](https://packages.ubuntu.com/source/noble/llvm-toolchain-18).
  Each package's copyright and licence notice is saved next to the library in
  the `runtime` folder of the data directory. LLVM uses the Apache License 2.0
  with LLVM Exceptions.

Google's ChromeOS TTS package and voice files are downloaded by the user from
Google's mirror and are not part of this distribution. Google's terms apply to
them.
