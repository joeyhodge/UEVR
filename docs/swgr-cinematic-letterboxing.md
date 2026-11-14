# Galactic Racer cinematic letterboxing

This default-off compatibility option targets the UE5.7.4 DX12 Native path in
`SWGR-Win64-Shipping.exe`, with or without Native Fix. Enable it under
VR > Compatibility > Galactic Racer Cinematic Letterboxing Fix (Experimental),
or set:

```ini
VR_Compatibility_SWGRLetterboxingFix=true
```

## Evidence and scope

During a paused collision camera, the right-eye scene source was already black
before UEVR's final copy. Temporarily removing only
`M_PP_CameraLetterboxing` restored that eye; restoring the original chain brought
the blackout back. Disabling refraction or changing the camera-construction CVar
did not repair the existing camera.

The option filters only direct dynamic instances of this material from the
after-tonemap chain returned for eligible stereo views. Other materials retain
their order. The game's material objects, blendable settings, CVars, alpha,
camera/FOV and temporal histories are not changed. Removing this material also
removes its cinematic letterbox treatment from both eyes.

Each call validates the current material and parent. There is no retained MID
pointer or owned CVar value: a level load, replacement MID or game re-enable is
handled by inspecting the next returned chain. Disabling the option stops
filtering; the original getter still executes exactly once per call.

## Fail-closed contract

- Require the actual DX12 renderer, active HMD and Native rendering; Mono,
  sequential modes, flat-screen and split-screen compatibility are excluded.
- Validate the getter's complete 1005-byte function and unwind range before
  hooking. Only external relocation displacements are masked. The known RVA is
  a fast candidate, not permission to hook an unmatched function. A bounded
  fallback search must find exactly one validated candidate.
- Require validated eight-byte FNames and the observed UObject header layout.
  Resolve names on the game thread, never in the render callback.
- Accept only the observed ten-element inline, non-owning material array in a
  caller stack return buffer. Heap arrays, unreadable objects, unsupported
  layouts and unknown code keep the original path.
- Gate filtering across enable/mode changes with an epoch. No per-frame scan,
  resource allocation, GPU wait or repeated hook installation is added.

The code contract is derived from executable SHA-256
`8c53e41a7ba1d6ae8f7485d0f1a22d3230c8a4ccdd0a17e01df0e11f8565b014`.
The unmasked getter body SHA-256 is
`fd76fae14839357902f557639f1509684cbf42d7f0f158e52a47a2eb6f931ccf`.
Future updates with a different contract remain unsupported rather than using
unchecked offsets.

## Verification

`galactic-racer-letterboxing-tests` covers code/mask rejection, mode and view
scope, material identity, mixed-chain preservation, replacement MIDs, storage
bounds and the actual MSVC hidden-return-buffer ABI through SafetyHook.
The temporary exact-material bypass was HMD-confirmed; the compiled option
still needs its own collision, disable/re-enable and level-transition test.
OpenXR was used for the live confirmation; OpenVR is not separately confirmed.
