# RTX 5080 Laptop GPU reference measurements

The results card uses measurements from the maintainer’s NVIDIA GeForce RTX 5080 Laptop GPU on mains power, taken on 26 November 2026 with the 2.0.0.0 build. The Windows x64 machine reported PCI model 10DE:2C59 and display driver 32.0.16.2140. The desktop and some background tools stayed active; NVIDIA reported between 5 and 25% GPU utilization and about 1.9 GB in use around the runs.

Each reference is the first run of that protocol that passed every execution, timing and process-exit check. Only completed runs with 30 warm-up frames and all 180 measurements qualify. GPU time includes the clean film shot at 1280 × 720 output. It excludes viewer startup, preparation and presentation.

| Build | Protocol | Input | Output | Typical GPU frame (ms) | Worst GPU frame (ms) |
| --- | --- | ---: | ---: | ---: | ---: |
| 2.0.0.0 | RC1 raster | 1280 × 720 | 1280 × 720 | 0.6371 | 1.9685 |
| 2.0.0.0 | PT1 High path tracing | 1280 × 720 | 1280 × 720 | 4.3686 | 6.0994 |
| 2.0.0.0 | DLSS-Q1 Super Resolution Quality | 853 × 480 | 1280 × 720 | 0.9998 | 2.6600 |

Raster and DLSS completed on their first attempt. Path tracing timed out in preparation three times while the driver had not yet compiled the ray-traced shaders for this build, and completed on the first attempt after the film had played once. For all three reference runs, the benchmark checked which rendering path actually ran and waited for the worker process to exit successfully.

The app matches build, protocol, input and output sizes, native Windows/app architecture, stable power source and observed rendering path before displaying a reference. Both columns use the same precision and units. A reference comparison describes one shot on two systems. A lower time means the GPU rendered a frame faster.

The reference machine has an **Intel Core Ultra 9 275HX** CPU and an **Intel AI Boost** NPU (public PCI model `8086:AD1D`). The independent RTX 4060 Laptop GPU machine has an **Intel Core i9-13900H** CPU. Windows found no present NPU in its accelerator inventory. In all three protocols, the CPU supplies the scripted director and the NPU is off by preset.
