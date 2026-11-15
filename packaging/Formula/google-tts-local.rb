class GoogleTtsLocal < Formula
  desc "Run Google's on-device text-to-speech voices locally on Apple Silicon"
  homepage "https://github.com/aaditya-v-more/google-tts-local"
  url "https://github.com/aaditya-v-more/google-tts-local/releases/download/v0.4.1/google-tts-local-0.4.1.tar.gz"
  sha256 "47d7977d3f81e8df283f3234d51b104a3485cb5863862f9eaa28970ef14976c0"
  license all_of: ["MIT", "BSD-3-Clause"]

  depends_on macos: :ventura
  depends_on arch: :arm64

  def install
    (buildpath/"native/tmp").mkpath
    ENV["TMPDIR"] = (buildpath/"native/tmp").to_s
    system "make", "-C", "native", "all"
    bin.install "native/build/gtts-native" => "gtts"
    bin.install "native/launchagent.sh" => "gtts-service"
    bin.install "native/earmark.sh" => "gtts-earmark"
    (libexec/"google-tts-local").install "native/build/gtts-engine"
    (libexec/"google-tts-local").install "native/build/libgtts-shim.dylib"
    (libexec/"google-tts-local").install "native/build/gtts-macho-convert.py"
    (share/"google-tts-local/licenses").install Dir["native/licenses/*"]

    # The helper can use the custom x18 ABI or Hypervisor.framework as a fallback.
    system "/usr/bin/codesign", "--force", "--sign", "-", "--entitlements",
           "native/hypervisor.entitlements", libexec/"google-tts-local/gtts-engine"
    system "/usr/bin/codesign", "--force", "--sign", "-", bin/"gtts"
  end

  def caveats
    <<~EOS
      Download the engine, runtime libraries and voice packs after installing:
        gtts fetch
        gtts voices

      To run the HTTP server as a user LaunchAgent:
        gtts-service install

      To add the voices to Earmark's voice menu:
        gtts-earmark install

      The engine helper is ad-hoc signed and is not notarized.
      macOS 26.4 or later can run the engine in-process; older versions use
      Hypervisor.framework.
    EOS
  end

  test do
    assert_match "gtts 0.4.1", shell_output("#{bin}/gtts --version")
    assert_match "Usage: gtts speak", shell_output("#{bin}/gtts speak --help")
    assert_match "Run gtts fetch", shell_output("#{bin}/gtts voices --data-dir #{testpath}/missing 2>&1", 1)
    system "/usr/bin/codesign", "--verify", "--strict", libexec/"google-tts-local/gtts-engine"
    system "/usr/bin/codesign", "--verify", "--strict", libexec/"google-tts-local/libgtts-shim.dylib"
    system "/usr/bin/codesign", "--verify", "--strict", bin/"gtts"
    assert_match "Usage: gtts-earmark", shell_output("#{bin}/gtts-earmark --help")
  end
end
