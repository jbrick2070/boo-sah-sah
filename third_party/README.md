# Third-Party Dependencies

Milestone **M0 (`BASE_ONLY`)** intentionally ships with **zero external third-party source or binary dependencies** so that offline CMake configuration, compilation, unit testing, and baseline runtime execution succeed on a clean Windows 11 + Visual Studio 2022 + Windows SDK machine without internet access or proprietary SDK installations.

- **JSON parsing & serialization**: Implemented in-tree (`include/boo/json.hpp`, `src/core/json.cpp`) following RFC 8259.
- **TOML configuration subset**: Implemented in-tree (`src/app/settings.cpp`).
- **On-screen telemetry typography**: Implemented via an original procedural 5x7 pixel-matrix glyph table inside `assets/shaders/m0_test_visual.hlsl` (no third-party `.ttf`/`.otf` binaries distributed).

Future milestones (M3–M9) may attach optional plugins behind CMake options (`ENABLE_CUDA`, `ENABLE_TENSOR`, `ENABLE_OPENVINO`, `ENABLE_NVENC`, `ENABLE_SPOUT`) with reproducible version/hash manifests recorded here before use.
