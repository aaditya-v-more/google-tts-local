# Third-party runtime libraries

`gtts fetch` downloads these Ubuntu 24.04 arm64 packages directly from the
Ubuntu ports archive and verifies the package SHA-256 before extracting the
libraries. The corresponding Ubuntu source packages are available from the same
archive and the package pages linked below.

- LLVM libc++, libc++abi, and libunwind 18.1.3-1ubuntu1: [Ubuntu source package](https://packages.ubuntu.com/source/noble-updates/llvm-toolchain-18).
  Each package's copyright and license notice is extracted into the runtime data
  directory. LLVM uses the Apache License 2.0 with LLVM Exceptions and other
  notices in those copyright files.
- GCC libgcc-s1 14.2.0-4ubuntu2~24.04.1: [Ubuntu source package](https://packages.ubuntu.com/source/noble-updates/gcc-14).
  The accompanying `GCC-14-COPYRIGHT.txt` includes the GCC Runtime Library
  Exception; `GPL-3.0.txt` contains the GNU General Public License version 3.

Google's separate ChromeOS TTS package and voice files are downloaded by the
user and are not included in this source distribution.

The source distribution includes the single-file Zstandard 1.5.7 decoder,
generated from [the upstream v1.5.7 release](https://github.com/facebook/zstd/releases/tag/v1.5.7)
using `build/single_file_libs/create_single_file_decoder.sh`. It is used under
the BSD license in `ZSTD-BSD.txt`.
