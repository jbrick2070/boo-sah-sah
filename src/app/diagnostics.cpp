#include "boo/diagnostics.hpp"
#include "boo/json.hpp"
#include "boo/logging.hpp"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace boo {

std::string formatHardwareOrchestraTable(
    const SystemCapabilities& caps,
    const FallbackOrchestrator& orchestrator,
    ClockSource clockSrc,
    const std::string& audioDesc) {
    std::ostringstream os;
    os << "\n+=============================================================================================+\n";
    os << "|                     BOO SAH SAH — HARDWARE ORCHESTRA STATUS                                 |\n";
    os << "+=============================================================================================+\n";
    os << "  OS         : " << caps.osVersion << "\n";
    os << "  CPU        : " << caps.cpuBrand << " (" << caps.logicalCores << " logical threads, "
       << (caps.totalSystemRamBytes >> 30) << " GB RAM)\n";

    if (caps.selectedAdapterIndex >= 0 && static_cast<size_t>(caps.selectedAdapterIndex) < caps.adapters.size()) {
        const auto& a = caps.adapters[static_cast<size_t>(caps.selectedAdapterIndex)];
        os << "  D3D12 GPU  : " << a.description << " (FL " << a.maxFeatureLevel
           << ", SM " << a.shaderModel << ", DXR " << a.dxrTier << ", "
           << (a.dedicatedVideoMemoryBytes >> 20) << " MB VRAM"
           << (a.isSoftwareWarp ? ", WARP SOFTWARE" : "") << ")\n";
    } else {
        os << "  D3D12 GPU  : NONE AVAILABLE\n";
    }
    os << "  Clock      : " << toString(clockSrc) << " | Audio: " << audioDesc << "\n";
    os << "-----------------------------------------------------------------------------------------------\n";
    os << "  Engine  | Badge        | VerifiedWork | Active Backend                      | Diagnostic\n";
    os << "----------+--------------+--------------+-------------------------------------+----------------\n";

    const EngineKind kinds[] = {
        EngineKind::RT, EngineKind::Tensor, EngineKind::NPU, EngineKind::CUDA, EngineKind::Codec};
    for (EngineKind k : kinds) {
        const HardwareResult& r = orchestrator.engineStatus(k);
        char row[256]{};
        std::snprintf(
            row,
            sizeof(row),
            "  %-7s | %-12s | %-12s | %-35.35s | %s\n",
            toString(k),
            toString(r.status),
            r.successfullyExecutedTestWork ? "YES (Tested)" : "NO (Untested)",
            r.activeBackend.c_str(),
            r.diagnosticCode.c_str());
        os << row;
        if (!r.detectedHardwareDetail.empty()) {
            os << "            Detected : " << r.detectedHardwareDetail << "\n";
        }
        os << "            Reason   : " << r.humanReadableReason << "\n";
    }
    os << "+=============================================================================================+\n";
    return os.str();
}

bool exportDiagnosticsJson(
    const std::filesystem::path& path,
    const RuntimeDiagnosticsSummary& summary,
    const FallbackOrchestrator& orchestrator,
    std::string* outError) {
    JsonValue root = JsonValue::Object{};
    root["schema"] = 1;
    root["project"] = "BOO SAH SAH — The Glass Orchid Protocol";
    const std::string cueFn = summary.settings.cuePath.filename().string();
    const char* milestoneTag = "M10";
    if (cueFn.find("m1_") != std::string::npos) milestoneTag = "M1";
    else if (cueFn.find("m2_") != std::string::npos || cueFn.find("solo_rt") != std::string::npos) milestoneTag = "M2";
    else if (cueFn.find("solo_cuda") != std::string::npos) milestoneTag = "M3";
    else if (cueFn.find("solo_tensor") != std::string::npos) milestoneTag = "M4";
    else if (cueFn.find("solo_npu") != std::string::npos) milestoneTag = "M5";
    root["milestone"] = milestoneTag;

    JsonValue sys = JsonValue::Object{};
    sys["os"] = summary.capabilities.osVersion;
    sys["cpu"] = summary.capabilities.cpuBrand;
    sys["logical_cores"] = static_cast<int64_t>(summary.capabilities.logicalCores);
    sys["system_ram_bytes"] = static_cast<int64_t>(summary.capabilities.totalSystemRamBytes);
    sys["selected_adapter_index"] = static_cast<int64_t>(summary.capabilities.selectedAdapterIndex);
    sys["d3d12_available"] = summary.capabilities.d3d12Available;
    sys["d3d12_using_warp"] = summary.capabilities.d3d12UsingWarp;
    sys["d3d12_feature_level"] = summary.capabilities.d3d12FeatureLevel;
    sys["d3d12_shader_model"] = summary.capabilities.d3d12ShaderModel;
    sys["dxr_tier"] = summary.capabilities.dxrTierString;

    JsonValue adaptersArr = JsonValue::Array{};
    for (const auto& a : summary.capabilities.adapters) {
        JsonValue item = JsonValue::Object{};
        item["index"] = static_cast<int64_t>(a.index);
        item["description"] = a.description;
        item["vendor_id"] = static_cast<int64_t>(a.vendorId);
        item["device_id"] = static_cast<int64_t>(a.deviceId);
        item["dedicated_vram_bytes"] = static_cast<int64_t>(a.dedicatedVideoMemoryBytes);
        item["luid_low"] = static_cast<int64_t>(a.luidLowPart);
        item["luid_high"] = static_cast<int64_t>(a.luidHighPart);
        item["is_warp"] = a.isSoftwareWarp;
        item["supports_d3d12"] = a.supportsD3D12;
        item["max_feature_level"] = a.maxFeatureLevel;
        item["dxr_tier"] = a.dxrTier;
        adaptersArr.asArrayMut().push_back(std::move(item));
    }
    sys["adapters"] = std::move(adaptersArr);
    root["system"] = std::move(sys);

    JsonValue enginesObj = JsonValue::Object{};
    for (size_t i = 0; i < kEngineKindCount; ++i) {
        const auto kind = static_cast<EngineKind>(i);
        const auto& r = orchestrator.engineStatus(kind);
        JsonValue eo = JsonValue::Object{};
        eo["status"] = toString(r.status);
        eo["health"] = toString(r.health);
        eo["requested_backend"] = r.requestedBackend;
        eo["active_backend"] = r.activeBackend;
        eo["detected_hardware"] = r.detectedHardwareDetail;
        eo["capability_detected"] = r.capabilityDetected;
        eo["successfully_executed_test_work"] = r.successfullyExecutedTestWork;
        eo["diagnostic_code"] = r.diagnosticCode;
        eo["reason"] = r.humanReadableReason;
        enginesObj[toString(kind)] = std::move(eo);
    }
    root["engines"] = std::move(enginesObj);

    JsonValue run = JsonValue::Object{};
    run["clock_source"] = toString(summary.activeClockSource);
    run["audio_endpoint"] = summary.audioEndpoint;
    run["audio_fallback_reason"] = summary.audioFallbackReason;
    run["audio_uses_iaudioclock_presentation"] = summary.audioUsesHardwarePresentationClock;
    run["audio_submitted_samples_48k"] = summary.audioSubmittedSamples48k;
    run["audio_presented_samples_48k"] = summary.audioPresentedSamples48k;
    run["audio_buffer_latency_ms"] = summary.audioBufferLatencyMs;
    run["audio_endpoint_migrations"] = static_cast<int64_t>(summary.audioEndpointMigrations);
    run["edition"] = toString(summary.settings.edition);
    run["playback_mode"] = toString(summary.settings.playbackMode);
    run["final_quality_preset"] = toString(summary.finalQualityPreset);
    run["quality_downgrades"] = static_cast<int64_t>(summary.qualityDowngrades);
    run["rendered_frames"] = static_cast<int64_t>(summary.renderedFrames);
    run["present_stalls"] = static_cast<int64_t>(summary.presentStalls);
    run["start_sample_48k"] = summary.startSample;
    run["final_sample_48k"] = summary.finalSample;
    run["wall_elapsed_seconds"] = summary.wallElapsedSeconds;
    run["peak_estimated_vram_bytes"] = static_cast<int64_t>(summary.peakEstimatedVramBytes);
    run["effect_fault_count"] = static_cast<int64_t>(summary.effectFaultCount);
    run["used_embedded_fallback_shader"] = summary.usedEmbeddedFallbackShader;
    run["spout_active"] = summary.spoutActive;
    run["spout_sender_name"] = summary.spoutSenderName;
    run["spout_published_frames"] = static_cast<int64_t>(summary.spoutPublishedFrames);
    run["media_input_active"] = summary.mediaInputActive;
    run["live_camera_active"] = summary.liveCameraActive;
    run["media_decoded_frames"] = static_cast<int64_t>(summary.mediaDecodedFrames);
    run["media_diagnostic_code"] = summary.mediaDiagnosticCode;
    run["media_status_reason"] = redactUserPath(summary.mediaStatusReason);
    run["swapchain_bits"] = static_cast<int64_t>(summary.swapchainBits);
    run["npu_infer_ms"] = summary.lastNpuInferMs;
    run["npu_result_age_ms"] = summary.lastNpuResultAgeMs;
    run["npu_completed_inferences"] = static_cast<int64_t>(summary.npuCompletedInferences);

    const char* pillarNames[4] = {"RT", "CUDA", "Tensor", "Codec"};
    JsonValue livePillarsObj = JsonValue::Object{};
    for (size_t p = 0; p < 4; ++p) {
        const auto& lp = summary.lastLivePillars[p];
        JsonValue po = JsonValue::Object{};
        po["status"] = toString(lp.status);
        po["active_in_scene"] = lp.activeInScene;
        po["hardware_verified"] = lp.hardwareVerified;
        po["workload_items"] = static_cast<int64_t>(lp.workloadItems);
        po["workload_steps"] = static_cast<int64_t>(lp.workloadSteps);
        po["execution_ms"] = lp.executionMs;
        po["throughput"] = lp.throughput;
        po["quality_or_bitrate"] = lp.qualityOrBitrate;
        po["aux_metric"] = lp.auxMetric;
        livePillarsObj[pillarNames[p]] = std::move(po);
    }
    run["live_scene_pillars"] = std::move(livePillarsObj);

    JsonValue passObj = JsonValue::Object{};
    passObj["total_ms"] = summary.lastGpuPassTimings.totalMs;
    passObj["scene_ms"] = summary.lastGpuPassTimings.sceneMs;
    passObj["bloom_ms"] = summary.lastGpuPassTimings.bloomMs;
    passObj["composite_ms"] = summary.lastGpuPassTimings.compositeMs;
    passObj["present_pass_ms"] = summary.lastGpuPassTimings.presentPassMs;
    run["gpu_pass_ms"] = std::move(passObj);

    auto packStats = [](const FrametimeStats& s) {
        JsonValue o = JsonValue::Object{};
        o["sample_count"] = static_cast<int64_t>(s.sampleCount);
        o["mean_ms"] = s.meanMs;
        o["p50_ms"] = s.p50Ms;
        o["p95_ms"] = s.p95Ms;
        o["p99_ms"] = s.p99Ms;
        o["min_ms"] = s.minMs;
        o["max_ms"] = s.maxMs;
        return o;
    };
    run["cpu_frametime_ms"] = packStats(summary.cpuFrameStats);
    run["gpu_frametime_ms"] = packStats(summary.gpuFrameStats);
    root["runtime"] = std::move(run);

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs) {
        if (outError) *outError = "Unable to write diagnostics JSON: " + redactUserPath(path.string());
        return false;
    }
    ofs << serializeJson(root, 2);
    return true;
}

bool exportBenchmarkJson(
    const std::filesystem::path& path,
    const BenchmarkSuiteReport& report,
    std::string* outError) {
    JsonValue root = JsonValue::Object{};
    root["schema"] = 1;
    root["project"] = "BOO SAH SAH — Ad-Hoc Numerical Accelerator Benchmarks";
    root["timestamp"] = report.timestampIso;
    root["adapter"] = report.adapterName;
    root["cpu"] = report.cpuName;
    root["timed_iterations"] = static_cast<int64_t>(report.timedIterations);
    root["total_elapsed_seconds"] = report.totalElapsedSeconds;

    JsonValue arr = JsonValue::Array{};
    for (const auto& e : report.entries) {
        JsonValue item = JsonValue::Object{};
        item["category"] = toString(e.category);
        item["id"] = e.id;
        item["title"] = e.title;
        item["backend"] = e.backendName;
        item["status"] = toString(e.status);
        item["is_fallback_comparison"] = e.isFallbackComparison;
        item["executed"] = e.executed;
        item["output_verified"] = e.outputVerified;
        item["cold_init_ms"] = e.coldInitMs;
        item["primary_score"] = e.primaryScore;
        item["primary_unit"] = e.primaryUnit;
        item["secondary_score"] = e.secondaryScore;
        item["secondary_unit"] = e.secondaryUnit;
        item["mean_ms"] = e.timingMs.meanMs;
        item["p50_ms"] = e.timingMs.p50Ms;
        item["p95_ms"] = e.timingMs.p95Ms;
        item["p99_ms"] = e.timingMs.p99Ms;
        item["verification_detail"] = e.verificationDetail;
        item["notes"] = e.notes;
        arr.asArrayMut().push_back(std::move(item));
    }
    root["benchmarks"] = std::move(arr);

    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs) {
        if (outError) *outError = "Unable to write benchmark JSON: " + redactUserPath(path.string());
        return false;
    }
    ofs << serializeJson(root, 2);
    return true;
}

bool exportBenchmarkCsv(
    const std::filesystem::path& path,
    const BenchmarkSuiteReport& report,
    std::string* outError) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream ofs(path, std::ios::out | std::ios::trunc);
    if (!ofs) {
        if (outError) *outError = "Unable to write benchmark CSV: " + redactUserPath(path.string());
        return false;
    }
    ofs << "category,id,backend,status,is_fallback,executed,verified,primary_score,primary_unit,secondary_score,secondary_unit,cold_init_ms,mean_ms,p50_ms,p95_ms,p99_ms\n";
    for (const auto& e : report.entries) {
        ofs << toString(e.category) << ','
            << '"' << e.id << "\","
            << '"' << e.backendName << "\","
            << toString(e.status) << ','
            << (e.isFallbackComparison ? 1 : 0) << ','
            << (e.executed ? 1 : 0) << ','
            << (e.outputVerified ? 1 : 0) << ','
            << std::fixed << std::setprecision(4)
            << e.primaryScore << ','
            << '"' << e.primaryUnit << "\","
            << e.secondaryScore << ','
            << '"' << e.secondaryUnit << "\","
            << e.coldInitMs << ','
            << e.timingMs.meanMs << ','
            << e.timingMs.p50Ms << ','
            << e.timingMs.p95Ms << ','
            << e.timingMs.p99Ms << '\n';
    }
    return true;
}

} // namespace boo
