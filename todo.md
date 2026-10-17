# TODO

## Native Metal renderer (replace Cemu's GPU layer)

Phase 1 is done: design in `docs/native-renderer.md`, shader translator in `tools/shaders/`,
renderer in `runtime/src/gpu_native/` (build option `BOTW_NATIVE_GPU`, run with `--renderer native`).
The title menu renders natively; in-game only the HUD shows so far.

Remaining work, by the agent's estimate:

- [ ] 1–2 weeks of performance work.
- [ ] 2–3 weeks for the full 3D world: depth-buffer copies, streamout, occlusion queries and render scale.
- [ ] About 2 weeks comparing its output against Cemu's.
- [ ] 2–3 weeks to convert every shader ahead of time.
- [ ] About a week to make it the default and remove Cemu's layer.
