# Architecture review request: a community "universe" platform built on a GPU demo

You are reviewing a draft architecture. Be critical and independent. Your job is to find what is wrong, missing or over-built, and to say what the best architecture would be, even if it is not the one proposed. Do not rubber-stamp it. Separate facts from opinions, and say how confident you are in each conclusion.

If you can read the repository at `D:\boo-sah-sah`, read `docs/community_platform_framework.md` first, then check its claims against the code. Start with `include/boo/api.hpp`, `include/boo/render.hpp`, `include/boo/cues.hpp`, `include/boo/health.hpp`, `include/boo/ui_text.hpp`, `src/app/main.cpp`, `src/render/trilogy_renderer.cpp`, `PRIVACY.md`, `packaging/msix/AppxManifest.xml` and `tools/refresh_asset_hashes.py`. If you cannot read the repository, rely on the summary below and say which conclusions depend on it.

## The product today

Boo Sah Sah is a Windows 11, C++20, Direct3D 12 GPU demo distributed through the Microsoft Store: a six-minute real-time film plus an interactive Explore mode. It is about 35,000 lines of C++ and 9,700 lines of HLSL, built by one maintainer.

Its signature is an honest hardware readout. A HUD shows live GPU and CPU numbers (frame times, per-pass GPU timings, NVML temperature, power and clocks, CPU load, VRAM) and labels five engines (ray tracing, CUDA, tensor cores, Intel NPU, video codec) as Accelerated, Substituted, Degraded, Unavailable or Faulted. It separates "capability detected" from "work actually executed and verified". It also has an in-house neural rendering suite (neural shading, neural texture compression, an online radiance cache), DLSS and frame generation, and a small NPU "stage director" model.

## Problem statement

Turn this into a platform where:

1. People with little graphics experience use AI to create their own "universes" (environments plus scenes) in minutes.
2. Every universe runs inside the same Host app, under the same GPU and CPU readout HUD, and produces the same performance receipt.
3. Universes are published to a shared network and can link to each other. The long-term vision is "Second Life for the AI age": shared presence, a hub, maybe VR. A first network could be a small local client that pulls universes from GitHub.

Example universes the owner has in mind: the existing spy-noir city film, and a "Planet Zoo in space" with living creatures that behave on their own.

Success looks like this: a second universe built without changing Host code; a one-line prompt that produces a valid universe; outside creators publishing; and the Store app staying trustworthy.

## Verified facts that constrain the design

- **Scenes are code today.** Cue sheets (JSON) give each scene sample-exact timing, a seed, a preferred engine, a fallback visual and director permission. But each scene's content (camera paths, actor motion, lighting, effects) is C++ and HLSL, and a scene's `visual` id selects hard-coded code paths. The mesh list is a fixed array in the renderer.
- **The HUD lives inside the film's main effect** (pass 4 of a 4-pass pipeline), not in the app. Its labels are a compile-time enum shared with the shader, and they fill 71 of 72 atlas cells. Pass 3 already produces a HUD-free "clean" frame, which is sent to other apps through Spout.
- **Frames are a function of (seed, sample position).** Seeking to sample S gives the same bytes as playing to S on one machine. There is no cross-GPU determinism claim.
- **The Store app has no networking code**, declares no `internetClient` capability, and its privacy policy says it sends nothing. It declares `runFullTrust` (a restricted capability) and `webcam`.
- **Store policy, from search summaries and not verified:** 10.2.2 forbids changing an app's described functionality through dynamically downloaded code; 11.12 requires content guidelines, reporting and takedown for user-generated content.
- **The tensor-core path uses a preview SDK** and is excluded from Store builds.
- **Existing quality gates:** deterministic capture suites, a stress gate, cue and manifest validation, and playback on WARP (a software GPU).

## The proposed architecture (a draft by another AI)

- **One Host, many plugins.** Environment and Scene packages are the plugins. A scene renders HDR plus depth; the Host owns post-processing, neural services, upscaling, the HUD and capture.
- **The HUD moves into the Host**, as an overlay fed by a "Readout Bus". The bus separates Measured rows (Host timestamps, sensors, verified engines) from Declared rows (numbers a plugin reports about itself). Plugins cannot label themselves Accelerated.
- **A standard folder layout:** `universe.json`, `budget.json`, `readout.json`, `environments/` (meshes, textures, lighting, actors), `scenes/` (a superset of today's cue sheets), `director/`, `audio/`, `neural/`, `strings/`, `credits.json`, `tests/`.
- **Trust tiers.**
  - Tier 0 is data only.
  - Tier 1 adds a curve and expression language for motion, plus HLSL fragments in fixed slots, compiled and validated at publish time.
  - Tier 2 is native DLLs and never ships in the Store build.
  - The Host chooses dispatch sizes, and a GPU time budget with a watchdog falls back to the scene's fallback visual.
- **AI authoring.**
  - An LLM planner fills a template's typed slots.
  - "Recipes" make the media, with ComfyUI as the first provider: 360° panoramas, panorama to mesh, image to 3D props, music.
  - A cooker converts glTF or USD into the engine's mesh and texture formats.
  - The existing gates judge the result, then the creator publishes.
- **Network.**
  - v0 is a GitHub repo that holds only pointers (hash, signature, URL, receipt). Publishing is a pull request with CI checks.
  - A separate small client downloads, verifies and launches the Host, so the Store app keeps no network code.
  - v1 is a hosted registry. v2 adds shared-clock watch parties and a hub. Parcels, VR and an economy come much later.
- **Roadmap.**
  - P0: package the existing film and move the HUD into the Host, proven with golden capture hashes.
  - P1: a Studio CLI and MCP server, and the AI loop.
  - P2: the network.
  - P3: watch parties.
  - P4: the hub.
  - P5: parcels, VR, economy.

## What I want from you

1. **Is the problem statement right?** What is misframed or missing (users, motivation, business model, legal)?
2. **Is this the best architecture?** Compare it against at least two real alternatives. Examples:
   - building on an existing engine or platform (Omniverse with OpenUSD, Unreal, Godot, Unity)
   - a browser host using WebGPU
   - glTF or OpenUSD as the package format instead of custom formats
   - WASM or Lua instead of a motion DSL, which matters for behaving creatures
   - networking inside the Store app instead of a separate client
   - a federated registry

   Say which you would choose and why.
3. **The five biggest risks or blind spots**, ranked, with what would make each one fail.
4. **What to cut, merge or re-sequence for one maintainer.** What is the smallest version worth shipping?
5. **Claims you think are wrong or overstated.**
6. **What should happen in the next two weeks?**

Answer with:

- a one-paragraph verdict
- a ranked issue list (severity, why it matters, fix)
- an alternatives table (option, pros, cons, verdict)
- recommended changes to the plan
- open questions for the owner

Be concrete and skip generic advice.
