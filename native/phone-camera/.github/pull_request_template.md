## What this changes and why

## Testing

- [ ] `gradlew testDebugUnitTest` and `gradlew lintDebug` pass (if Android code changed)
- [ ] `ctest --test-dir pc/build -C Release` passes (if PC code changed)
- [ ] Tested on a real device (specify which): Android phone / iPhone / PC — not just an emulator or build
- [ ] If the protocol changed: `protocol/PROTOCOL.md`, `protocol/golden.txt`, `Protocol.kt` and
      `pc/companion/protocol.h` were all updated together

## Notes for the reviewer

Anything that needs a second look, known limitations, or follow-up left for later.
