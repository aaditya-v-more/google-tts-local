# Proposed additions to aaditya-v-more/homebrew-tap

Add this row under “What's here”:

```markdown
| [google-tts-local](https://github.com/aaditya-v-more/google-tts-local) (formula) | Runs Google's on-device text-to-speech voices locally on Apple Silicon, with a CLI and optional HTTP server. |
```

Add this paragraph under “Trust”:

```markdown
The `google-tts-local` formula builds from source and ad-hoc signs the engine
helper with Apple's custom x18 ABI and hypervisor entitlements. The helper is
not notarised. The formula does not include or download Google engine or voice
files at install time; run `gtts fetch` separately.
```
