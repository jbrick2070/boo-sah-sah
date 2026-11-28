cmake_minimum_required(VERSION 3.28)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" BOO_RELEASE_VERSION LIMIT_COUNT 1)
project(BooSahSah
    VERSION "${BOO_RELEASE_VERSION}"
    DESCRIPTION "BOO SAH SAH — The Glass Orchid Protocol"
    LANGUAGES CXX RC
)

# Windows x64 enforcement for baseline build (SPEC_v4 Section 1.1 & 3.1)
if(NOT WIN32)
    message(FATAL_ERROR "BOO SAH SAH targets Windows 11 x64 (Win32 + Direct3D 12).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Keep both Release and Debug install verification independent of a developer CRT.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Section 12.1: Build mode toggles (Baseline requires zero optional proprietary SDKs)
option(BASE_ONLY       "Build baseline D3D12+DXR+raster+audio with all optional SDKs excluded" ON)
option(ENABLE_DXR      "Enable DirectX Raytracing runtime module (Windows SDK headers)" ON)
option(ENABLE_CUDA     "Enable optional CUDA plugin build (not required for M1)" OFF)
option(ENABLE_TENSOR   "Enable optional cuBLASLt Tensor plugin build (not required for M1)" OFF)
option(ENABLE_OPENVINO "Enable optional Intel OpenVINO NPU/CPU plugin build (not required for M1)" OFF)
option(ENABLE_NVENC    "Enable optional NVIDIA Video Codec SDK plugin build (default OFF)" OFF)
option(ENABLE_SPOUT    "Enable optional Spout2 D3D12 sender build (default OFF)" OFF)
option(BOO_BUILD_TESTS "Build unit and hardware integration tests" ON)
option(ENABLE_DLSS "Use the official NVIDIA NGX SDK when installed by tools/fetch_dlss.py" ON)
set(BOO_DLSS_SDK "${CMAKE_SOURCE_DIR}/build/dependencies/nvidia_dlss" CACHE PATH "Pinned NVIDIA DLSS SDK")

if(BASE_ONLY)
    set(ENABLE_CUDA OFF CACHE BOOL "" FORCE)
    set(ENABLE_TENSOR OFF CACHE BOOL "" FORCE)
    set(ENABLE_OPENVINO OFF CACHE BOOL "" FORCE)
    set(ENABLE_NVENC OFF CACHE BOOL "" FORCE)
    set(ENABLE_SPOUT OFF CACHE BOOL "" FORCE)
endif()

# Locate Windows SDK DirectX Shader Compiler (dxc.exe)
find_program(BOO_DXC_EXECUTABLE
    NAMES dxc dxc.exe
    HINTS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64"
)
if(NOT BOO_DXC_EXECUTABLE)
    message(FATAL_ERROR "Could not find Windows SDK DirectX Shader Compiler (dxc.exe). Ensure Windows 10/11 SDK is installed.")
endif()
message(STATUS "Found DXC: ${BOO_DXC_EXECUTABLE}")

# Compile HLSL shaders at build time into signed DXIL .cso binaries + embedded safe fallback C++ headers
set(BOO_GEN_SHADER_DIR "${CMAKE_BINARY_DIR}/generated_shaders")
set(BOO_OUT_SHADER_DIR "${CMAKE_BINARY_DIR}/assets/shaders")
file(MAKE_DIRECTORY "${BOO_GEN_SHADER_DIR}")
file(MAKE_DIRECTORY "${BOO_OUT_SHADER_DIR}")

set(M0_VISUAL_HLSL "${CMAKE_SOURCE_DIR}/assets/shaders/m0_test_visual.hlsl")
set(M0_BENCH_HLSL  "${CMAKE_SOURCE_DIR}/assets/shaders/m0_benchmarks.hlsl")
set(M1_SLICE_HLSL  "${CMAKE_SOURCE_DIR}/assets/shaders/m1_taipei_slice.hlsl")

set(M0_VS_CSO          "${BOO_OUT_SHADER_DIR}/m0_test_vs.cso")
set(M0_PS_CSO          "${BOO_OUT_SHADER_DIR}/m0_test_ps.cso")
set(M0_FALLBACK_VS_HPP "${BOO_GEN_SHADER_DIR}/m0_fallback_vs.hpp")
set(M0_FALLBACK_PS_HPP "${BOO_GEN_SHADER_DIR}/m0_fallback_ps.hpp")

set(M0_DXR_BENCH_CSO       "${BOO_OUT_SHADER_DIR}/m0_dxr_bench.cso")
set(M0_RT_FB_BENCH_CSO     "${BOO_OUT_SHADER_DIR}/m0_rt_fallback_bench.cso")
set(M0_PARTICLE_BENCH_CSO  "${BOO_OUT_SHADER_DIR}/m0_particle_bench.cso")
set(M0_TENSOR_FB_BENCH_CSO "${BOO_OUT_SHADER_DIR}/m0_tensor_fallback_bench.cso")
set(M0_SIGNAL_BENCH_CSO    "${BOO_OUT_SHADER_DIR}/m0_signal_bench.cso")

set(M1_DXR_SCENE_CSO       "${BOO_OUT_SHADER_DIR}/m1_dxr_scene.cso")
set(M1_RASTER_SCENE_CSO    "${BOO_OUT_SHADER_DIR}/m1_raster_scene.cso")
set(M1_BLOOM_DOWN_CSO      "${BOO_OUT_SHADER_DIR}/m1_bloom_down.cso")
set(M1_BLOOM_UP_CSO        "${BOO_OUT_SHADER_DIR}/m1_bloom_up.cso")
set(M1_COMPOSITE_CSO       "${BOO_OUT_SHADER_DIR}/m1_composite.cso")
set(M1_PRESENT_HUD_CSO     "${BOO_OUT_SHADER_DIR}/m1_present_hud.cso")
set(M1_PRESENT_CLEAN_CSO   "${BOO_OUT_SHADER_DIR}/m1_present_clean.cso")
set(M1_PARTICLES_SUB_CSO   "${BOO_OUT_SHADER_DIR}/m1_particles_sub_cs.cso")
set(M1_PARTICLES_VS_CSO    "${BOO_OUT_SHADER_DIR}/m1_particles_vs.cso")
set(M1_PARTICLES_PS_CSO    "${BOO_OUT_SHADER_DIR}/m1_particles_ps.cso")
set(M1_TENSOR_SUB_CSO      "${BOO_OUT_SHADER_DIR}/m1_tensor_sub_cs.cso")
set(M1_NV12_PACK_CSO       "${BOO_OUT_SHADER_DIR}/m1_nv12_pack_cs.cso")
set(M8_SPOUT_BLIT_CSO      "${BOO_OUT_SHADER_DIR}/m8_spout_blit.cso")

add_custom_command(
    OUTPUT "${M0_VS_CSO}" "${M0_FALLBACK_VS_HPP}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T vs_6_0 -E VSMain -Ges -WX -O3
            "${M0_VISUAL_HLSL}" -Fo "${M0_VS_CSO}" -Fh "${M0_FALLBACK_VS_HPP}" -Vn kFallbackVSBytecode
    DEPENDS "${M0_VISUAL_HLSL}"
    COMMENT "Compiling Fullscreen Vertex Shader (DXIL .cso + embedded fallback header)"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M0_PS_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSMain -Ges -WX -O3
            "${M0_VISUAL_HLSL}" -Fo "${M0_PS_CSO}"
    DEPENDS "${M0_VISUAL_HLSL}"
    COMMENT "Compiling M0 Glass Orchid + Telemetry HUD Pixel Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M0_FALLBACK_PS_HPP}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSFallbackMain -Ges -WX -O3
            "${M0_VISUAL_HLSL}" -Fh "${M0_FALLBACK_PS_HPP}" -Vn kFallbackPSBytecode
    DEPENDS "${M0_VISUAL_HLSL}"
    COMMENT "Compiling Embedded Precompiled Safe Fallback Pixel Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M0_DXR_BENCH_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_5 -E CSDxrRayQueryBench -DBOO_SHADER_DXR_BENCH -Ges -WX -O3
            "${M0_BENCH_HLSL}" -Fo "${M0_DXR_BENCH_CSO}"
    DEPENDS "${M0_BENCH_HLSL}"
    COMMENT "Compiling DXR 1.1 Inline RayQuery Benchmark Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M0_RT_FB_BENCH_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSAnalyticRtFallbackBench -DBOO_SHADER_RT_FALLBACK_BENCH -Ges -WX -O3
            "${M0_BENCH_HLSL}" -Fo "${M0_RT_FB_BENCH_CSO}"
    DEPENDS "${M0_BENCH_HLSL}"
    COMMENT "Compiling Analytic RT Substitute Benchmark Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M0_PARTICLE_BENCH_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSHlslParticleBench -DBOO_SHADER_PARTICLE_BENCH -Ges -WX -O3
            "${M0_BENCH_HLSL}" -Fo "${M0_PARTICLE_BENCH_CSO}"
    DEPENDS "${M0_BENCH_HLSL}"
    COMMENT "Compiling HLSL 1M-Particle Compute Benchmark Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M0_TENSOR_FB_BENCH_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSHlslNeuralVelvetBench -DBOO_SHADER_TENSOR_FALLBACK_BENCH -Ges -WX -O3
            "${M0_BENCH_HLSL}" -Fo "${M0_TENSOR_FB_BENCH_CSO}"
    DEPENDS "${M0_BENCH_HLSL}"
    COMMENT "Compiling HLSL Neural Velvet MLP Benchmark Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M0_SIGNAL_BENCH_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSShaderSignalBench -DBOO_SHADER_SIGNAL_BENCH -Ges -WX -O3
            "${M0_BENCH_HLSL}" -Fo "${M0_SIGNAL_BENCH_CSO}"
    DEPENDS "${M0_BENCH_HLSL}"
    COMMENT "Compiling HLSL SHADER SIGNAL Substitute Benchmark Shader"
    VERBATIM
)

# Milestone M1 4-Pass Shaders
add_custom_command(
    OUTPUT "${M1_DXR_SCENE_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_5 -E CSDxrTaipeiScene -DBOO_M1_DXR_CS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_DXR_SCENE_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 DXR 1.1 Inline RayQuery Taipei Scene Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_RASTER_SCENE_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSRasterTaipeiScene -DBOO_M1_RASTER_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_RASTER_SCENE_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 SM 6.0 Raster Taipei Scene Substitute Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_BLOOM_DOWN_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSBloomDownsample -DBOO_M1_BLOOM_DOWN_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_BLOOM_DOWN_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 Dual-Kawase HDR Bloom Downsample Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_BLOOM_UP_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSBloomUpsample -DBOO_M1_BLOOM_UP_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_BLOOM_UP_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 Dual-Kawase HDR Bloom Tent Upsample Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_COMPOSITE_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSCompositeCanvas -DBOO_M1_COMPOSITE_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_COMPOSITE_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 Clean 16:9 Canvas ACES + sRGB + IGN Dither Composite Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_PRESENT_HUD_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSPresentLetterboxHud -DBOO_M1_PRESENT_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PRESENT_HUD_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 Swapchain 16:9 Letterbox Catmull-Rom + 10/8-bit HUD Present Shader"
    VERBATIM
)

# The same pass for frames without the overlay or with hudMode 3: the overlay-only HUD layers left out.
add_custom_command(
    OUTPUT "${M1_PRESENT_CLEAN_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSPresentLetterboxHud -DBOO_M1_PRESENT_PS -DBOO_PRESENT_CLEAN -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PRESENT_CLEAN_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 Present Shader for frames without the overlay"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_PARTICLES_SUB_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSLiveParticlesSub -DBOO_M1_PARTICLES_SUB_CS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PARTICLES_SUB_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 HLSL 8K Linear Rain-Mist Substitute Compute Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_PARTICLES_VS_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T vs_6_0 -E VSLiveParticles -DBOO_M1_PARTICLES_VS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PARTICLES_VS_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 3D Velocity-Aligned Rain & Sanctuary Pollen Vertex Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_PARTICLES_PS_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSLiveParticles -DBOO_M1_PARTICLES_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PARTICLES_PS_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 3D Soft-Occluded Rain & Sanctuary Pollen Pixel Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_TENSOR_SUB_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSLiveTensorSub -DBOO_M1_TENSOR_SUB_CS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_TENSOR_SUB_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 HLSL Scalar Caustic Field Substitute Compute Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_NV12_PACK_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSPackCanvasToNv12 -DBOO_M1_NV12_PACK_CS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_NV12_PACK_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M1 CanvasClean -> 720p NV12 Hardware Codec Input Pack Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M8_SPOUT_BLIT_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSSpoutCleanBlit -DBOO_M8_SPOUT_BLIT_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M8_SPOUT_BLIT_CSO}"
    DEPENDS "${M1_SLICE_HLSL}" "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h"
    COMMENT "Compiling M8 Clean-Feed CanvasClean -> Spout2 Shared BGRA8 Blit Shader"
    VERBATIM
)

add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_mesh_vs.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T vs_6_0 -E VSMain  -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_mesh_vs.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_depth_cutout_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSDepthCutout -DBOO_DEPTH_CUTOUT_PS=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_depth_cutout_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_mesh_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSMain  -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_mesh_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_mesh_glass_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSMain -DBOO_GLASS_REFRACTION=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_mesh_glass_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_mesh_pane_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSMain -DBOO_GLASS_PANE=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_mesh_pane_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_mesh_pane_distance_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSPaneDistance -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_mesh_pane_distance_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_mesh_cutout_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSMain -DBOO_ALPHA_CUTOUT=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_mesh_cutout_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_mesh_rt_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_5 -E PSMain -DM2_RT -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_mesh_rt_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_mesh_rt_cutout_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_5 -E PSMain -DM2_RT -DBOO_ALPHA_CUTOUT=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_mesh_rt_cutout_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_sky_vs.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T vs_6_0 -E VSSky  -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_sky_vs.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_sky_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSSky  -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_sky_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_sky_rt_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_5 -E PSSky -DM2_RT -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_sky_rt_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)

add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_accum_vs.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T vs_6_0 -E VSAccum -DBOO_ACCUM=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_accum_vs.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_accum_blend_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSAccumBlend -DBOO_ACCUM=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_accum_blend_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_accum_resolve_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSAccumResolve -DBOO_ACCUM=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_accum_resolve_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_denoise_resolve_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSDeterministicResolve -DBOO_DENOISE_RESOLVE=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_denoise_resolve_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_nrd_pack_cs.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSPackNrdReblur -DBOO_NRD_PACK_CS=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_nrd_pack_cs.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)
add_custom_command(OUTPUT "${BOO_OUT_SHADER_DIR}/m2_nrd_unpack_ps.cso"
 COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSUnpackNrdReblur -DBOO_NRD_UNPACK=1 -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" -Fo "${BOO_OUT_SHADER_DIR}/m2_nrd_unpack_ps.cso"
 DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/m2_mesh.hlsl" VERBATIM)

set(BOO_VARIANT_SHADER_OUTPUTS)
function(boo_variant_shader name source profile entry)
    set(output "${BOO_OUT_SHADER_DIR}/${name}.cso")
    set(extra_dependencies)
    if(source STREQUAL "m1_taipei_slice")
        list(APPEND extra_dependencies "${CMAKE_SOURCE_DIR}/assets/shaders/m1_hud_layout.h")
    endif()
    add_custom_command(OUTPUT "${output}"
        COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ${profile} -E ${entry} ${ARGN} -Ges -WX -O3
            "${CMAKE_SOURCE_DIR}/assets/shaders/${source}.hlsl" -Fo "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/${source}.hlsl" ${extra_dependencies} VERBATIM)
    set(BOO_VARIANT_SHADER_OUTPUTS ${BOO_VARIANT_SHADER_OUTPUTS} "${output}" PARENT_SCOPE)
endfunction()
boo_variant_shader(dlss_resolve dlss_resolve cs_6_0 CSResolve)
boo_variant_shader(m2_motion_vs m2_mesh vs_6_0 VSSceneMotion)
boo_variant_shader(m2_motion_ps m2_mesh ps_6_0 PSSceneMotion)
boo_variant_shader(m2_motion_cutout_ps m2_mesh ps_6_0 PSSceneMotion -DBOO_ALPHA_CUTOUT=1)
boo_variant_shader(m2_sky_motion_ps m2_mesh ps_6_0 PSSkyMotion)
boo_variant_shader(m1_hud_cache m1_taipei_slice ps_6_0 PSHudCache -DBOO_M1_PRESENT_PS)
boo_variant_shader(m1_hud_cache_clean m1_taipei_slice ps_6_0 PSHudCache -DBOO_M1_PRESENT_PS -DBOO_PRESENT_CLEAN)
boo_variant_shader(m1_present_cached m1_taipei_slice ps_6_0 PSPresentCached -DBOO_M1_PRESENT_PS)
boo_variant_shader(m1_present_cached_frame_generation m1_taipei_slice ps_6_0 PSPresentCached -DBOO_M1_PRESENT_PS -DBOO_FRAME_GENERATION_OUTPUTS)
boo_variant_shader(m1_present_frame_generation m1_taipei_slice ps_6_0 PSPresentLetterboxHud -DBOO_M1_PRESENT_PS -DBOO_FRAME_GENERATION_OUTPUTS)
boo_variant_shader(m1_present_clean_frame_generation m1_taipei_slice ps_6_0 PSPresentLetterboxHud -DBOO_M1_PRESENT_PS -DBOO_FRAME_GENERATION_OUTPUTS -DBOO_PRESENT_CLEAN)
# Neural rendering suite (src/render/neural_rendering.cpp)
boo_variant_shader(neural_clear_cs neural_rendering cs_6_0 CSClearStats)
boo_variant_shader(neural_ntc_decode_cs neural_rendering cs_6_0 CSNtcDecode)
boo_variant_shader(neural_nrc_init_cs neural_rendering cs_6_0 CSNrcInit)
boo_variant_shader(neural_nrc_train_cs neural_rendering cs_6_0 CSNrcTrain)
boo_variant_shader(neural_nrc_update_cs neural_rendering cs_6_0 CSNrcUpdate)
boo_variant_shader(neural_nrc_infer_cs neural_rendering cs_6_0 CSNrcInfer)
boo_variant_shader(neural_composite_cs neural_rendering cs_6_0 CSNeuralComposite)
boo_variant_shader(neural_nrc_pack_cs neural_rendering cs_6_0 CSNrcPackF16)

# Tensor-core neural rendering: D3D12 Agility SDK 1.721 preview + DXC preview (Shader Model 6.10 LinAlg).
# Test-rig builds only: the preview runtime needs Windows Developer Mode and is not allowed in Store packages.
# tools/fetch_neural_sdk.py downloads both into build/dependencies.
option(ENABLE_LINALG_PREVIEW "Tensor-core neural rendering via the D3D12 Agility SDK preview (not for Store builds)" OFF)
set(BOO_AGILITY_SDK "${CMAKE_SOURCE_DIR}/build/dependencies/d3d12_agility" CACHE PATH "D3D12 Agility SDK 1.721 preview")
set(BOO_DXC_PREVIEW "${CMAKE_SOURCE_DIR}/build/dependencies/dxc_preview" CACHE PATH "DXC with Shader Model 6.10")
set(BOO_HAS_LINALG OFF)
if(ENABLE_LINALG_PREVIEW)
    if(EXISTS "${BOO_AGILITY_SDK}/build/native/include/d3d12.h" AND EXISTS "${BOO_AGILITY_SDK}/build/native/bin/x64/D3D12Core.dll"
       AND EXISTS "${BOO_DXC_PREVIEW}/bin/x64/dxc.exe" AND EXISTS "${BOO_DXC_PREVIEW}/inc/hlsl/dx/linalg.h")
        set(BOO_HAS_LINALG ON)
        message(STATUS "Tensor-core neural rendering: ON (Agility SDK preview + DXC preview)")
        foreach(pair "neural_ntc_decode_tc_cs;CSNtcDecode" "neural_nrc_infer_tc_cs;CSNrcInfer" "neural_composite_tc_cs;CSNeuralComposite")
            list(GET pair 0 tc_name)
            list(GET pair 1 tc_entry)
            set(tc_out "${BOO_OUT_SHADER_DIR}/${tc_name}.cso")
            add_custom_command(OUTPUT "${tc_out}"
                COMMAND "${BOO_DXC_PREVIEW}/bin/x64/dxc.exe" -nologo -I "${BOO_DXC_PREVIEW}/inc/hlsl" -T cs_6_10 -enable-16bit-types
                    -DBOO_NEURAL_LINALG=1 -E ${tc_entry} -Ges -WX -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/neural_rendering.hlsl" -Fo "${tc_out}"
                DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/neural_rendering.hlsl" VERBATIM)
            list(APPEND BOO_VARIANT_SHADER_OUTPUTS "${tc_out}")
        endforeach()
    else()
        message(WARNING "ENABLE_LINALG_PREVIEW is ON but the preview SDKs are missing; run tools/fetch_neural_sdk.py")
    endif()
endif()

# NVIDIA PhysX suite. Part one is smoke: NVIDIA Flow, built from the PhysX SDK checkout by
# tools/fetch_physx_sdk.py into build/dependencies. Test-rig builds only for now; without it the
# feature compiles to a stub that reports itself unavailable.
option(ENABLE_PHYSX "NVIDIA PhysX suite (Flow smoke) from the PhysX SDK checkout in build/dependencies" OFF)
set(BOO_PHYSX_SDK "${CMAKE_SOURCE_DIR}/build/dependencies/physx-sdk" CACHE PATH "NVIDIA PhysX SDK checkout with Flow built")
set(BOO_FLOW_BIN "${BOO_PHYSX_SDK}/flow/_build/windows-x86_64/release")
set(BOO_HAS_FLOW OFF)
if(ENABLE_PHYSX)
    if(EXISTS "${BOO_PHYSX_SDK}/flow/include/nvflowext/NvFlowLoader.h" AND EXISTS "${BOO_FLOW_BIN}/nvflow.dll"
       AND EXISTS "${BOO_FLOW_BIN}/nvflowext.dll")
        set(BOO_HAS_FLOW ON)
        message(STATUS "PhysX suite: NVIDIA Flow smoke ON")
        set(flow_cso "${BOO_OUT_SHADER_DIR}/flow_smoke_cs.cso")
        # PNanoVDB.h, the NanoVDB reader, comes from the SDK checkout; it is not copied into this repository.
        add_custom_command(OUTPUT "${flow_cso}"
            COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSSmokeMarch -I "${BOO_PHYSX_SDK}/flow/include/nvflow/nanovdb"
                -Ges -O3 "${CMAKE_SOURCE_DIR}/assets/shaders/flow_smoke.hlsl" -Fo "${flow_cso}"
            DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/flow_smoke.hlsl" VERBATIM)
        list(APPEND BOO_VARIANT_SHADER_OUTPUTS "${flow_cso}")
    else()
        message(WARNING "ENABLE_PHYSX is ON but NVIDIA Flow is not built; run tools/fetch_physx_sdk.py")
    endif()
endif()

add_custom_target(boo_shaders DEPENDS ${BOO_VARIANT_SHADER_OUTPUTS}
    "${BOO_OUT_SHADER_DIR}/m2_mesh_vs.cso"
    "${BOO_OUT_SHADER_DIR}/m2_depth_cutout_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_mesh_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_mesh_glass_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_mesh_pane_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_mesh_pane_distance_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_mesh_cutout_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_mesh_rt_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_mesh_rt_cutout_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_sky_vs.cso"
    "${BOO_OUT_SHADER_DIR}/m2_sky_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_sky_rt_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_accum_vs.cso"
    "${BOO_OUT_SHADER_DIR}/m2_accum_blend_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_accum_resolve_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_denoise_resolve_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_nrd_pack_cs.cso"
    "${BOO_OUT_SHADER_DIR}/m2_nrd_unpack_ps.cso"

    "${M0_VS_CSO}"
    "${M0_PS_CSO}"
    "${M0_FALLBACK_VS_HPP}"
    "${M0_FALLBACK_PS_HPP}"
    "${M0_DXR_BENCH_CSO}"
    "${M0_RT_FB_BENCH_CSO}"
    "${M0_PARTICLE_BENCH_CSO}"
    "${M0_TENSOR_FB_BENCH_CSO}"
    "${M0_SIGNAL_BENCH_CSO}"
    "${M1_DXR_SCENE_CSO}"
    "${M1_RASTER_SCENE_CSO}"
    "${M1_BLOOM_DOWN_CSO}"
    "${M1_BLOOM_UP_CSO}"
    "${M1_COMPOSITE_CSO}"
    "${M1_PRESENT_HUD_CSO}"
    "${M1_PRESENT_CLEAN_CSO}"
    "${M1_PARTICLES_SUB_CSO}"
    "${M1_PARTICLES_VS_CSO}"
    "${M1_PARTICLES_PS_CSO}"
    "${M1_TENSOR_SUB_CSO}"
    "${M1_NV12_PACK_CSO}"
    "${M8_SPOUT_BLIT_CSO}"
)

# Common compiler settings
function(boo_apply_compiler_flags target_name)
    target_compile_definitions(${target_name} PUBLIC
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        _CRT_SECURE_NO_WARNINGS
        $<$<BOOL:${ENABLE_DXR}>:BOO_ENABLE_DXR=1>
    )
    if(MSVC)
        target_compile_options(${target_name} PRIVATE /W4 /permissive- /EHsc /utf-8)
    endif()
endfunction()

# Core foundation static library (zero optional proprietary SDK dependencies!)
add_library(boo_core STATIC
    src/core/api.cpp
    src/core/json.cpp
    src/core/logging.cpp
    src/core/clock.cpp
    src/core/cues.cpp
    src/core/capabilities.cpp
    src/core/gpu_sensors.cpp
    src/core/startup_progress.cpp
    src/core/pipeline_compiler.cpp
    src/core/baked_physics.cpp
    src/core/health.cpp
    src/inference/scripted_director.cpp
    src/inference/openvino_director.cpp
    src/audio/engine.cpp
    src/audio/audio_reactive.cpp
    src/render/d3d12_context.cpp
    src/render/live_accelerators.cpp
    src/render/m0_test_visual.cpp
    src/render/m1_scene.cpp
    src/render/bmesh.cpp
    src/render/btex.cpp
    src/render/nrd_bridge.cpp
    src/render/trilogy_renderer.cpp
    src/render/city_modules.cpp
    src/render/m1_taipei_slice.cpp
    src/render/dlss_bridge.cpp
    src/render/neural_rendering.cpp
    src/render/flow_smoke.cpp
    src/physics/physx_suite.cpp
    src/physics/physx_rigid.cpp
    src/physics/physx_marbles.cpp
    src/physics/physx_soft.cpp
    src/physics/physx_route_test.cpp
    src/physics/physx_fluid.cpp
    src/render/frame_generation.cpp
    src/render/ui_text.cpp
    src/media/media_foundation_input.cpp
    src/integrations/spout_sender.cpp
    src/bench/benchmarks.cpp
    src/app/settings.cpp
    src/app/window.cpp
    src/app/world_portal.cpp
    src/app/localization.cpp
    src/app/diagnostics.cpp
    src/app/results_worker.cpp
    src/app/results_text.cpp
    src/app/results_dialog.cpp
    src/app/results_card.cpp
    src/app/results_reference.cpp
    src/app/results_export.cpp
    src/bench/results_benchmark.cpp
    "${M0_FALLBACK_VS_HPP}"
    "${M0_FALLBACK_PS_HPP}"
)
add_dependencies(boo_core boo_shaders)
target_include_directories(boo_core PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${BOO_GEN_SHADER_DIR}"
)
target_compile_definitions(boo_core PRIVATE BOO_RELEASE_VERSION="${PROJECT_VERSION}")
boo_apply_compiler_flags(boo_core)

set(BOO_STREAMLINE_SDK "${CMAKE_SOURCE_DIR}/build/dependencies/nvidia_streamline" CACHE PATH "NVIDIA Streamline x64 release")
if(EXISTS "${BOO_STREAMLINE_SDK}/include/sl.h" AND EXISTS "${BOO_STREAMLINE_SDK}/bin/x64/sl.dlss_g.dll")
    target_compile_definitions(boo_core PRIVATE BOO_HAS_STREAMLINE=1)
    target_include_directories(boo_core SYSTEM PRIVATE "${BOO_STREAMLINE_SDK}/include")
    set(sl_files)
    foreach(sl_file sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll nvngx_dlss.license.txt reflex.license.txt)
        list(APPEND sl_files "${BOO_STREAMLINE_SDK}/bin/x64/${sl_file}")
    endforeach()
    add_custom_target(boo_stage_streamline
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/streamline"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${sl_files} "${BOO_STREAMLINE_SDK}/license.txt" "${BOO_STREAMLINE_SDK}/3rd-party-licenses.md" "${CMAKE_BINARY_DIR}/streamline" VERBATIM)
    add_dependencies(boo_core boo_stage_streamline)
endif()

set(BOO_DLSS_DLL "${BOO_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlss.dll")
if(ENABLE_DLSS AND EXISTS "${BOO_DLSS_DLL}" AND EXISTS "${BOO_DLSS_SDK}/include/nvsdk_ngx.h")
    set(BOO_HAS_DLSS ON)
    target_compile_definitions(boo_core PRIVATE BOO_HAS_DLSS=1)
    if(EXISTS "${BOO_DLSS_SDK}/include/nvsdk_ngx_helpers_dlssd_d3d.h" AND EXISTS "${BOO_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")
        set(BOO_HAS_DLSS_RR ON)
        target_compile_definitions(boo_core PRIVATE BOO_HAS_DLSS_RR=1)
    endif()
    target_include_directories(boo_core SYSTEM PRIVATE "${BOO_DLSS_SDK}/include")
    set(ngx_lib_dir "${BOO_DLSS_SDK}/lib/Windows_x86_64/x64")
    target_link_libraries(boo_core PUBLIC optimized "${ngx_lib_dir}/nvsdk_ngx_s.lib" debug "${ngx_lib_dir}/nvsdk_ngx_s_dbg.lib")
endif()

if(BOO_HAS_LINALG)
    target_compile_definitions(boo_core PRIVATE BOO_HAS_LINALG=1)
    # Only the neural suite sees the preview d3d12.h; the rest of the engine keeps the Windows SDK headers.
    set_source_files_properties(src/render/neural_rendering.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${BOO_AGILITY_SDK}/build/native/include")
    add_custom_target(boo_stage_agility
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/D3D12"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${BOO_AGILITY_SDK}/build/native/bin/x64/D3D12Core.dll" "${BOO_AGILITY_SDK}/build/native/bin/x64/d3d12SDKLayers.dll"
            "${BOO_AGILITY_SDK}/LICENSE.txt" "${CMAKE_BINARY_DIR}/D3D12" VERBATIM)
    add_dependencies(boo_core boo_stage_agility)
endif()

# The PhysX SDK itself (step 1 of the suite: the shared runtime). Built by tools/fetch_physx_sdk.py
# with the static C runtime, to match this project.
set(BOO_PHYSX_BIN "${BOO_PHYSX_SDK}/physx/bin/win.x86_64.vc143.mt/release")
set(BOO_HAS_PHYSX OFF)
if(ENABLE_PHYSX AND EXISTS "${BOO_PHYSX_SDK}/physx/include/PxPhysicsAPI.h" AND EXISTS "${BOO_PHYSX_BIN}/PhysX_64.dll")
    set(BOO_HAS_PHYSX ON)
    message(STATUS "PhysX suite: PhysX SDK runtime ON")
    target_compile_definitions(boo_core PRIVATE BOO_HAS_PHYSX=1)
    set_source_files_properties(src/physics/physx_suite.cpp src/physics/physx_rigid.cpp src/physics/physx_marbles.cpp src/physics/physx_soft.cpp src/physics/physx_route_test.cpp src/physics/physx_fluid.cpp PROPERTIES INCLUDE_DIRECTORIES "${BOO_PHYSX_SDK}/physx/include")
    target_link_libraries(boo_core PUBLIC
        "${BOO_PHYSX_BIN}/PhysX_64.lib" "${BOO_PHYSX_BIN}/PhysXCommon_64.lib" "${BOO_PHYSX_BIN}/PhysXFoundation_64.lib"
        "${BOO_PHYSX_BIN}/PhysXExtensions_static_64.lib" "${BOO_PHYSX_BIN}/PhysXPvdSDK_static_64.lib")
    add_custom_target(boo_stage_physx
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${BOO_PHYSX_BIN}/PhysX_64.dll" "${BOO_PHYSX_BIN}/PhysXCommon_64.dll" "${BOO_PHYSX_BIN}/PhysXFoundation_64.dll"
            "${BOO_PHYSX_BIN}/PhysXGpu_64.dll" "${CMAKE_BINARY_DIR}" VERBATIM)
    add_dependencies(boo_core boo_stage_physx)
    # The offline bake tool for the film's physics tracks (assets/physics).
    add_executable(boo_physx_bake tools/physx_bake/physx_bake.cpp)
    target_include_directories(boo_physx_bake PRIVATE "${BOO_PHYSX_SDK}/physx/include")
    target_link_libraries(boo_physx_bake PRIVATE boo_core)
    boo_apply_compiler_flags(boo_physx_bake)
elseif(ENABLE_PHYSX)
    message(WARNING "ENABLE_PHYSX is ON but the PhysX SDK is not built; run tools/fetch_physx_sdk.py")
endif()

if(BOO_HAS_FLOW)
    target_compile_definitions(boo_core PRIVATE BOO_HAS_FLOW=1)
    # Only the smoke bridge sees NVIDIA Flow's headers.
    set_source_files_properties(src/render/flow_smoke.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${BOO_PHYSX_SDK}/flow/include/nvflowext;${BOO_PHYSX_SDK}/flow/include/nvflow")
    add_custom_target(boo_stage_flow
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${BOO_FLOW_BIN}/nvflow.dll" "${BOO_FLOW_BIN}/nvflowext.dll" "${CMAKE_BINARY_DIR}" VERBATIM)
    add_dependencies(boo_core boo_stage_flow)
endif()

# Link strictly against inbox Windows 11 system libraries
target_link_libraries(boo_core PUBLIC
    bcrypt
    comctl32
    d3d12
    dxgi
    dxguid
    ole32
    oleaut32
    uuid
    avrt
    cfgmgr32
    mfplat
    mfuuid
    mf
    user32
    gdi32
    shell32
    shlwapi
    d2d1            # localized on-screen text (system fonts only)
    dwrite
    windowscodecs
)

# Stage runtime data directories into build folder for convenient direct launch
add_custom_target(boo_stage_assets ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/shaders" "${CMAKE_BINARY_DIR}/assets/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/meshes" "${CMAKE_BINARY_DIR}/assets/meshes"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/textures" "${CMAKE_BINARY_DIR}/assets/textures"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/models" "${CMAKE_BINARY_DIR}/assets/models"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/assets/audio" "${CMAKE_BINARY_DIR}/assets/audio"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/cues" "${CMAKE_BINARY_DIR}/assets/cues"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/strings" "${CMAKE_BINARY_DIR}/assets/strings"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/neural" "${CMAKE_BINARY_DIR}/assets/neural"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/assets/physics" "${CMAKE_BINARY_DIR}/assets/physics"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/manifest.json" "${CMAKE_BINARY_DIR}/assets/manifest.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/config" "${CMAKE_BINARY_DIR}/config"
    DEPENDS boo_shaders
    COMMENT "Staging cue sheets, models, first-party OST audio, artifact manifest, and config presets"
)

# 1. Main Milestone M1 Demoscene Application (Native WIN32 GUI subsystem — zero console window)
add_executable(boo_sah_sah WIN32 src/app/main.cpp src/app/results_dialog.rc src/app/world_portal.rc assets/icons/app_icon.rc)
if(BOO_HAS_LINALG)
    target_compile_definitions(boo_sah_sah PRIVATE BOO_HAS_LINALG=1) # exports D3D12SDKVersion / D3D12SDKPath
endif()
target_link_options(boo_sah_sah PRIVATE "/ENTRY:mainCRTStartup")
target_include_directories(boo_sah_sah PRIVATE "${CMAKE_SOURCE_DIR}/assets/icons")
target_link_libraries(boo_sah_sah PRIVATE boo_core)
add_dependencies(boo_sah_sah boo_stage_assets)
boo_apply_compiler_flags(boo_sah_sah)

# 2. Standalone Cue & Manifest Validator CLI Tool
add_executable(boo_cue_validate tools/cue_validate/main.cpp)
target_link_libraries(boo_cue_validate PRIVATE boo_core)
add_dependencies(boo_cue_validate boo_stage_assets)
boo_apply_compiler_flags(boo_cue_validate)

# 3. Standalone Ad-Hoc Numerical Accelerator Benchmark CLI Tool
add_executable(boo_bench tools/bench_report/main.cpp)
target_link_libraries(boo_bench PRIVATE boo_core)
add_dependencies(boo_bench boo_stage_assets)
boo_apply_compiler_flags(boo_bench)

# 3b. Native Single-File Windows Installer / Uninstaller Stub (inbox cabinet.dll LZMS SFX)
add_executable(boo_installer_stub WIN32 tools/installer/setup_main.cpp assets/icons/app_icon.rc)
target_compile_definitions(boo_installer_stub PRIVATE BOO_RELEASE_VERSION="${PROJECT_VERSION}")
target_include_directories(boo_installer_stub PRIVATE "${CMAKE_SOURCE_DIR}/assets/icons")
target_link_libraries(boo_installer_stub PRIVATE
    cabinet
    ole32
    shell32
    advapi32
    user32
    gdi32
    comctl32
    uuid
)
boo_apply_compiler_flags(boo_installer_stub)

# 4. Unit & Hardware Integration Tests (CTest)
if(BOO_BUILD_TESTS)
    enable_testing()
    target_sources(boo_sah_sah PRIVATE tests/verify_film_benchmark.cpp)
    target_compile_definitions(boo_sah_sah PRIVATE BOO_VERIFY_FILM_BENCHMARK=1)
    add_executable(boo_results_card_test tests/results_card_tests.cpp)
    target_link_libraries(boo_results_card_test PRIVATE boo_core)
    boo_apply_compiler_flags(boo_results_card_test)
    add_test(NAME BooResultsCard COMMAND boo_results_card_test)
    add_executable(boo_results_dialog_test tests/integration/results_dialog_test.cpp)
    target_link_libraries(boo_results_dialog_test PRIVATE boo_core user32 oleacc oleaut32 ole32)
    boo_apply_compiler_flags(boo_results_dialog_test)
    add_executable(boo_results_reset_fixture tests/integration/results_reset_fixture.cpp src/app/results_dialog.rc)
    target_link_libraries(boo_results_reset_fixture PRIVATE boo_core)
    boo_apply_compiler_flags(boo_results_reset_fixture)
    add_test(NAME BooResultsDialog COMMAND boo_results_dialog_test "$<TARGET_FILE:boo_sah_sah>" "$<TARGET_FILE:boo_results_reset_fixture>")
    set_tests_properties(BooResultsDialog PROPERTIES LABELS "integration;native-ui" TIMEOUT 85 RUN_SERIAL TRUE)

    add_executable(boo_worker_isolation_test tests/worker_isolation/worker_isolation.cpp)
    boo_apply_compiler_flags(boo_worker_isolation_test)
    add_test(NAME BooWorkerIsolation COMMAND boo_worker_isolation_test)
    set_tests_properties(BooWorkerIsolation PROPERTIES LABELS "unit;isolation" TIMEOUT 10)

    add_executable(boo_results_worker_test tests/worker_isolation/results_worker_test.cpp src/app/results_worker.cpp)
    target_compile_definitions(boo_results_worker_test PRIVATE BOO_RESULTS_CONTAINMENT_TEST=1)
    target_include_directories(boo_results_worker_test PRIVATE "${CMAKE_SOURCE_DIR}/include")
    boo_apply_compiler_flags(boo_results_worker_test)
    add_test(NAME BooResultsWorker COMMAND boo_results_worker_test --no-activate --mute)
    set_tests_properties(BooResultsWorker PROPERTIES LABELS "unit;isolation" TIMEOUT 75)

    foreach(mode warp wrong-adapter low-budget)
        add_test(NAME BooFilmBenchmark-${mode}
            COMMAND boo_sah_sah --verify-film-benchmark ${mode} --no-activate --mute)
        set_tests_properties(BooFilmBenchmark-${mode} PROPERTIES LABELS "integration;isolation" TIMEOUT 60)
    endforeach()

    add_executable(boo_results_model_tests tests/results_model_tests.cpp)
    target_include_directories(boo_results_model_tests PRIVATE "${CMAKE_SOURCE_DIR}/include")
    boo_apply_compiler_flags(boo_results_model_tests)
    add_test(NAME BooResultsModelTests COMMAND boo_results_model_tests)
    set_tests_properties(BooResultsModelTests PROPERTIES LABELS "unit" TIMEOUT 10)

    add_executable(boo_unit_tests tests/unit/unit_main.cpp)
    target_link_libraries(boo_unit_tests PRIVATE boo_core)
    if(BOO_HAS_PHYSX)
        target_compile_definitions(boo_unit_tests PRIVATE BOO_UNIT_HAS_PHYSX=1)
    endif()
    add_dependencies(boo_unit_tests boo_stage_assets)
    boo_apply_compiler_flags(boo_unit_tests)

    add_test(NAME BooUnitTests COMMAND boo_unit_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(BooUnitTests PROPERTIES LABELS "unit")

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME BooCompareCapturesTool
            COMMAND "${Python3_EXECUTABLE}" tests/tools/test_compare_captures.py -v
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        set_tests_properties(BooCompareCapturesTool PROPERTIES LABELS "unit")
    endif()

    add_test(NAME BooCueValidation COMMAND boo_cue_validate WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(BooCueValidation PROPERTIES LABELS "unit;validation")
    add_test(NAME BooWarpPlayback
        COMMAND "${CMAKE_COMMAND}" "-DAPP=$<TARGET_FILE:boo_sah_sah>" "-DOUT=${CMAKE_BINARY_DIR}/warp-test"
                -P "${CMAKE_SOURCE_DIR}/tests/integration/warp_playback.cmake")
    set_tests_properties(BooWarpPlayback PROPERTIES LABELS "integration;software" TIMEOUT 90)

    add_executable(boo_integration_tests tests/integration/integration_main.cpp)
    target_link_libraries(boo_integration_tests PRIVATE boo_core windowscodecs gdi32)
    add_dependencies(boo_integration_tests boo_stage_assets)
    boo_apply_compiler_flags(boo_integration_tests)

    add_test(NAME BooHardwareIntegrationTests COMMAND boo_integration_tests --no-activate WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME BooTrilogyCaptures COMMAND boo_integration_tests --trilogy-captures --no-activate WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME BooFilmCaptures COMMAND boo_integration_tests --film-captures --no-activate WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME BooInteractiveCaptures COMMAND boo_integration_tests --interactive-captures --no-activate WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_test(NAME BooFilmStress COMMAND boo_integration_tests --film-stress --no-activate WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(BooFilmCaptures PROPERTIES LABELS "hardware;integration" TIMEOUT 600)
    set_tests_properties(BooInteractiveCaptures PROPERTIES LABELS "hardware;integration" TIMEOUT 600)
    set_tests_properties(BooFilmStress PROPERTIES LABELS "hardware;integration" TIMEOUT 900)
    set_tests_properties(BooTrilogyCaptures PROPERTIES LABELS "hardware;integration")
    set_tests_properties(BooHardwareIntegrationTests PROPERTIES LABELS "hardware;integration")
endif()

# Stage optional NVIDIA NRD dynamic sidecar (nrd/NRD.dll) next to executables if present (zero static PE import!)
if(BOO_HAS_DLSS)
    add_custom_target(boo_stage_dlss
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/dlss"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BOO_DLSS_DLL}" "${CMAKE_BINARY_DIR}/dlss/nvngx_dlss.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${BOO_DLSS_SDK}/LICENSE.txt" "${CMAKE_BINARY_DIR}/dlss/LICENSE-NVIDIA-DLSS.txt"
        VERBATIM)
    add_dependencies(boo_core boo_stage_dlss)
    if(BOO_HAS_DLSS_RR)
        add_custom_command(TARGET boo_stage_dlss POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${BOO_DLSS_SDK}/lib/Windows_x86_64/rel/nvngx_dlssd.dll" "${CMAKE_BINARY_DIR}/dlss/nvngx_dlssd.dll" VERBATIM)
    endif()
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nvidia_nrd/bin/NRD.dll")
    add_custom_command(TARGET boo_sah_sah POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:boo_sah_sah>/nrd"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/nvidia_nrd/bin/NRD.dll"
            "$<TARGET_FILE_DIR:boo_sah_sah>/nrd/NRD.dll"
        VERBATIM)
endif()
