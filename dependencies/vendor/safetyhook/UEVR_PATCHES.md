Upstream base
- Repository: `https://github.com/praydog/safetyhook`
- Pinned revision: `b62c7356f3e364209a2ae14489550156d7b58b8b`

UEVR patches
1. `src/allocator.cpp`
   - Prefer ordinary nearby `VirtualAlloc` results first.
   - Only scan executable INT3 sleds if ordinary nearby allocation fails.
   - Reason: some game executables expose writable/protected image padding that is unsafe to reuse for trampolines.

2. `src/inline_hook.cpp`
   - Fail cleanly if trampoline/intermediary memory cannot be unprotected.
   - Reason: avoid hard crashes while writing trampolines on unusual executable mappings.

Policy
- Do not patch SafetyHook only inside `_deps`.
- Any UEVR-specific hook compatibility fix should land here so builds are reproducible.
