# RTX 5080 Laptop GPU reference measurements

The results card uses measurements from the maintainer’s NVIDIA GeForce RTX 5080 Laptop GPU on mains power. The Windows x64 machine reported PCI model 10DE:2C59 and display driver 32.0.16.2140. Testing began after Comfy’s rendering process exited. The Windows desktop remained active. Before the raster run, NVIDIA reported 13–19% GPU utilization and 1,554 MiB in use. Driver-cache warmth is uncontrolled.

The reference for each mode is the first run of that protocol after the machine became available for dedicated testing that passed every execution, timing and process-exit check. Only completed runs with 30 warm-up frames and all 180 measurements qualify. GPU time includes the clean film shot at 1280 × 720 output. It excludes viewer startup, preparation and presentation.

| Build | Protocol | Input | Output | Typical GPU frame (ms) | Worst GPU frame (ms) |
| --- | --- | ---: | ---: | ---: | ---: |
| 1.1.14.0 | RC1 raster | 1280 × 720 | 1280 × 720 | 0.6160 | 1.6506 |
| 1.1.14.0 | PT1 High path tracing | 1280 × 720 | 1280 × 720 | 3.5541 | 5.1108 |
| 1.1.14.0 | DLSS-Q1 Super Resolution Quality | 853 × 480 | 1280 × 720 | 0.9220 | 2.0432 |

The first dedicated DLSS attempt timed out during preparation. The first path-tracing attempt timed out after 116 measured frames. Each produced an interrupted result. The subsequent completed PT1 used the same High path-tracing shaders as ordinary playback, with the driver cache already warmed by the viewer. For all three reference runs, the benchmark checked which rendering path actually ran and waited for the worker process to exit successfully.

The app matches build, protocol, input and output sizes, native Windows/app architecture, stable power source and observed rendering path before displaying a reference. Both columns use the same precision and units. A reference comparison describes one shot on two systems. A lower time means the GPU rendered a frame faster.

The reference machine has an **Intel Core Ultra 9 275HX** CPU and an **Intel AI Boost** NPU (public PCI model `8086:AD1D`). The independent RTX 4060 Laptop GPU machine has an **Intel Core i9-13900H** CPU. Windows found no present NPU in its accelerator inventory. In all three protocols, the CPU supplies the scripted director and the NPU is off by preset.
