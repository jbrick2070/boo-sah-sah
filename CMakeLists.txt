cmake_minimum_required(VERSION 3.28)

project(BooSahSah
    VERSION 1.0.0
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
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 DXR 1.1 Inline RayQuery Taipei Scene Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_RASTER_SCENE_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSRasterTaipeiScene -DBOO_M1_RASTER_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_RASTER_SCENE_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 SM 6.0 Raster Taipei Scene Substitute Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_BLOOM_DOWN_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSBloomDownsample -DBOO_M1_BLOOM_DOWN_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_BLOOM_DOWN_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 Dual-Kawase HDR Bloom Downsample Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_BLOOM_UP_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSBloomUpsample -DBOO_M1_BLOOM_UP_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_BLOOM_UP_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 Dual-Kawase HDR Bloom Tent Upsample Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_COMPOSITE_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSCompositeCanvas -DBOO_M1_COMPOSITE_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_COMPOSITE_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 Clean 16:9 Canvas ACES + sRGB + IGN Dither Composite Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_PRESENT_HUD_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSPresentLetterboxHud -DBOO_M1_PRESENT_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PRESENT_HUD_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 Swapchain 16:9 Letterbox Catmull-Rom + 10/8-bit HUD Present Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_PARTICLES_SUB_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSLiveParticlesSub -DBOO_M1_PARTICLES_SUB_CS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PARTICLES_SUB_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 HLSL 8K Linear Rain-Mist Substitute Compute Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_PARTICLES_VS_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T vs_6_0 -E VSLiveParticles -DBOO_M1_PARTICLES_VS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PARTICLES_VS_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 3D Velocity-Aligned Rain & Sanctuary Pollen Vertex Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_PARTICLES_PS_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSLiveParticles -DBOO_M1_PARTICLES_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_PARTICLES_PS_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 3D Soft-Occluded Rain & Sanctuary Pollen Pixel Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_TENSOR_SUB_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSLiveTensorSub -DBOO_M1_TENSOR_SUB_CS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_TENSOR_SUB_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 HLSL Scalar Caustic Field Substitute Compute Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M1_NV12_PACK_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T cs_6_0 -E CSPackCanvasToNv12 -DBOO_M1_NV12_PACK_CS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M1_NV12_PACK_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
    COMMENT "Compiling M1 CanvasClean -> 720p NV12 Hardware Codec Input Pack Shader"
    VERBATIM
)

add_custom_command(
    OUTPUT "${M8_SPOUT_BLIT_CSO}"
    COMMAND "${BOO_DXC_EXECUTABLE}" -nologo -T ps_6_0 -E PSSpoutCleanBlit -DBOO_M8_SPOUT_BLIT_PS -Ges -WX -O3
            "${M1_SLICE_HLSL}" -Fo "${M8_SPOUT_BLIT_CSO}"
    DEPENDS "${M1_SLICE_HLSL}"
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

add_custom_target(boo_shaders DEPENDS
    "${BOO_OUT_SHADER_DIR}/m2_mesh_vs.cso"
    "${BOO_OUT_SHADER_DIR}/m2_depth_cutout_ps.cso"
    "${BOO_OUT_SHADER_DIR}/m2_mesh_ps.cso"
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
    src/render/ui_text.cpp
    src/media/media_foundation_input.cpp
    src/integrations/spout_sender.cpp
    src/bench/benchmarks.cpp
    src/app/settings.cpp
    src/app/window.cpp
    src/app/localization.cpp
    src/app/diagnostics.cpp
    "${M0_FALLBACK_VS_HPP}"
    "${M0_FALLBACK_PS_HPP}"
)
add_dependencies(boo_core boo_shaders)
target_include_directories(boo_core PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
    "${BOO_GEN_SHADER_DIR}"
)
boo_apply_compiler_flags(boo_core)

# Link strictly against inbox Windows 11 system libraries
target_link_libraries(boo_core PUBLIC
    bcrypt
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
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_SOURCE_DIR}/assets/manifest.json" "${CMAKE_BINARY_DIR}/assets/manifest.json"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/config" "${CMAKE_BINARY_DIR}/config"
    DEPENDS boo_shaders
    COMMENT "Staging cue sheets, models, first-party OST audio, artifact manifest, and config presets"
)

# 1. Main Milestone M1 Demoscene Application (Native WIN32 GUI subsystem — zero console window)
add_executable(boo_sah_sah WIN32 src/app/main.cpp assets/icons/app_icon.rc)
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

    add_executable(boo_unit_tests tests/unit/unit_main.cpp)
    target_link_libraries(boo_unit_tests PRIVATE boo_core)
    add_dependencies(boo_unit_tests boo_stage_assets)
    boo_apply_compiler_flags(boo_unit_tests)

    add_test(NAME BooUnitTests COMMAND boo_unit_tests WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(BooUnitTests PROPERTIES LABELS "unit")

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
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/nvidia_nrd/bin/NRD.dll")
    add_custom_command(TARGET boo_sah_sah POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:boo_sah_sah>/nrd"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/nvidia_nrd/bin/NRD.dll"
            "$<TARGET_FILE_DIR:boo_sah_sah>/nrd/NRD.dll"
        VERBATIM)
endif()
