# Nonblocking D3D12 hitch observations

Hitch Diagnostics must not change frame behavior just to collect state.
Previously its game-thread sampler directly read render-owned D3D12 fields,
called the engine/RHI UI texture getter and took the runtime swapchain mutex
with a blocking lock. The runtime mutex alone also did not cover creation's
map writes, which use the D3D12 OpenXR mutex.

The D3D12 frame callback now publishes a copied, pointer-free observation only
while existing Hitch Diagnostics collection is enabled. Neither publisher nor
reader waits for the diagnostic slot. Swapchain statistics are copied only if
both existing XR mutexes can be acquired immediately, in their established
D3D12-then-runtime order. A busy map is explicitly marked unavailable; a busy
diagnostic slot leaves the previous packet intact and its timestamp unchanged.

The game-thread sampler reads only that packet. It does not wait for XR or call
engine texture/RHI getters. Reports include availability, contention and sample
age, so missing observations are not mistaken for zero-sized render targets.
`ui_extent` now describes the last rendered UI allocation, not a new request:
OpenXR uses its published UI swapchain dimensions; OpenVR uses the owned UI
allocation on the render side. No native engine texture pointer is retained.

No rendering mutex, image acquisition/wait/release, copy, GPU fence, eye identity,
frame cadence, camera, player/stadium policy or profile setting is changed.
An unavailable diagnostic sample never controls rendering. The generic slot's
tests cover unavailable/busy/success, skipped publication, retirement and
concurrent coherent reads without adding a new CI test target.

This is a demonstrated diagnostic safety issue, not proof that it explains all
ProSpi hit/replay stalls. In particular the stable captured interval did not
show swapchain recreation on every cut. Normal acquisition uses the D3D12 XR
mutex; teardown can hold the runtime map mutex while waiting for copy retirement.
The larger transition stalls still require wait/engine-work attribution.
