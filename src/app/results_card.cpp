#include "boo/results_card.hpp"
#include "boo/results_reference.hpp"
#include "boo/ui_text.hpp"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <string_view>

#define BOO_WIDEN_INNER(x) L##x
#define BOO_WIDEN(x) BOO_WIDEN_INNER(x)
namespace boo {
using T = ResultText;

ResultText resultsReasonText(BenchmarkReason reason) {
    using R = BenchmarkReason;
    switch (reason) {
    case R::Busy: return T::Busy;
    case R::Cancelled: return T::Cancelled;
    case R::UnsupportedDevice: return T::Unsupported;
    case R::SoftwareRenderer: return T::Software;
    case R::MemoryBudget: return T::Memory;
    case R::TimeLimit: return T::Timeout;
    case R::GraphicsReset: return T::Reset;
    case R::RendererFailed: return T::RenderFailed;
    case R::TimingUnavailable: return T::TimingFailed;
    case R::WorkloadChanged: return T::Changed;
    case R::WorkerExited: return T::WorkerExited;
    case R::RequiresDxr: return T::NoDxr;
    case R::RequiresDlss: return T::NoDlss;
    case R::AdapterMismatch:
    case R::DlssInitFailed: return T::Unconfirmed;
    default: return T::Unknown;
    }
}

ResultText resultsModeText(BenchmarkMode mode) {
    switch (mode) {
    case BenchmarkMode::Raster: return T::Raster;
    case BenchmarkMode::PathTracing: return T::Path;
    case BenchmarkMode::DlssQuality: return T::Dlss;
    default: return T::Unknown;
    }
}

namespace {
std::wstring chip(uint32_t vendor, uint32_t device) {
    if (vendor == 0x10de && device == 0x28a0) return L"NVIDIA GeForce RTX 4060 Laptop GPU";
    if (vendor == 0x10de && device == 0x2c59) return L"NVIDIA GeForce RTX 5080 Laptop GPU";
    if (!vendor || !device || vendor > 0xffff || device > 0xffff) return resultText(T::Unknown);
    const wchar_t* name = nullptr;
    switch (vendor) {
    case 0x10de: name = L"NVIDIA"; break;
    case 0x8086: name = L"Intel"; break;
    case 0x1002: name = L"AMD"; break;
    case 0x1414: name = L"Microsoft"; break;
    }
    if (name) return std::format(L"{} · PCI {:04X}", name, device);
    return std::format(L"PCI {:04X}:{:04X}", vendor, device);
}

std::wstring cpuModel(const ResultsHostHardware& hardware) {
    switch (hardware.cpu) {
    case ResultsCpuModel::IntelCoreI9_13900H: return L"Intel Core i9-13900H";
    case ResultsCpuModel::IntelCoreUltra9_275HX: return L"Intel Core Ultra 9 275HX";
    case ResultsCpuModel::Intel:
    case ResultsCpuModel::AMD: {
        const wchar_t* vendor = hardware.cpu == ResultsCpuModel::Intel ? L"Intel" : L"AMD";
        return hardware.cpuFamily ? std::format(L"{} · CPUID {:02X}:{:02X}", vendor, hardware.cpuFamily, hardware.cpuModel) : vendor;
    }
    default: return resultText(T::Unknown);
    }
}

std::wstring npuModel(const ResultsHostHardware& hardware) {
    switch (hardware.npu) {
    case ResultsNpuModel::NoneDetected: return resultText(T::NoNpu);
    case ResultsNpuModel::IntelAiBoost: return L"Intel AI Boost";
    case ResultsNpuModel::AmdRyzenAI: return L"AMD Ryzen AI";
    case ResultsNpuModel::Unknown: {
        const auto vendor = hardware.acceleratorVendor, device = hardware.acceleratorDevice;
        if (!vendor || !device) return resultText(T::Unknown);
        std::wstring name;
        switch (vendor) {
        case 0x8086: name = L"Intel · "; break;
        case 0x1002: case 0x1022: name = L"AMD · "; break;
        case 0x17cb: name = L"Qualcomm · "; break;
        }
        return std::format(L"{}PCI {:04X}:{:04X} · {}", name,vendor,device,resultText(T::Unknown));
    }
    default: return resultText(T::Unknown);
    }
}

std::wstring architecture() {
    USHORT processMachine{}, nativeMachine{};
    IsWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine);
    const wchar_t* native = resultText(T::Unknown);
    switch (nativeMachine) {
    case IMAGE_FILE_MACHINE_AMD64: native = L"x64"; break;
    case IMAGE_FILE_MACHINE_ARM64: native = L"ARM64"; break;
    case IMAGE_FILE_MACHINE_I386: native = L"x86"; break;
    }
#if defined(_M_ARM64)
    return std::wstring(native) + L" / ARM64";
#elif defined(_M_X64)
    return std::wstring(native) + L" / x64";
#else
    return std::wstring(native) + L" / x86";
#endif
}

ResultText powerText(BenchmarkPower power) {
    switch (power) {
    case BenchmarkPower::Mains: return T::Mains;
    case BenchmarkPower::Battery: return T::Battery;
    case BenchmarkPower::Changed: return T::PowerChanged;
    default: return T::Unknown;
    }
}

ResultText outcomeText(ResultsOutcome outcome) {
    switch (outcome) {
    case ResultsOutcome::Completed: return T::Completed;
    case ResultsOutcome::Interrupted: return T::Interrupted;
    case ResultsOutcome::Cancelled: return T::Cancelled;
    default: return T::Unavailable;
    }
}

std::wstring evidence(BenchmarkEvidence item) {
    using E = BenchmarkEvidence;
    const std::wstring ran = std::wstring(resultText(T::Ran)) + L" · ";
    switch (item) {
    case E::FilmGpu: return ran + L"D3D12 · GPU";
    case E::RasterGpu: return ran + resultText(T::Raster) + L" · GPU";
    case E::PathTracingGpu: return ran + L"DXR · GPU";
    case E::DlssGpu: return ran + L"DLSS SR · GPU";
    case E::ScriptedCpu: return ran + resultText(T::Scripted) + L" · CPU";
    case E::HlslGpu: return std::wstring(resultText(T::Fallback)) + L" · HLSL · GPU";
    case E::Off: return resultText(T::Off);
    case E::Failed: return resultText(T::Failed);
    case E::NotExercised: return resultText(T::NotExercised);
    default: return resultText(T::Unknown);
    }
}

std::wstring decimal(double value) {
    auto number = std::format(L"{:.1f}", value);
    wchar_t separator[8]{};
    const auto& tag = UiStrings::instance().languageTag();
    const std::wstring locale(tag.begin(), tag.end());
    if (GetLocaleInfoEx(locale.c_str(), LOCALE_SDECIMAL, separator, 8) > 1) {
        if (auto at = number.find(L'.'); at != number.npos) number.replace(at, 1, separator);
    }
    return number;
}
std::vector<ResultCardRow> runRows(const ResultsDialogContext& context, const BenchmarkSnapshot& s,
                                   std::wstring_view build, bool reference, bool rtl) {
    std::vector<ResultCardRow> rows;
    const auto row = [&](std::wstring label, std::wstring value, bool numeric = false) {
        if (rtl && numeric) value = L"\u202d" + value + L"\u202c";
        rows.push_back({std::move(label), {ResultCardCell{std::move(value), numeric}, ResultCardCell{L"—"}}});
    };
    const auto field = [&](T key, std::wstring value, bool numeric = false) { row(resultText(key), std::move(value), numeric); };
    const auto observed = [&](BenchmarkFeature feature) { return s.evidence[static_cast<size_t>(feature)]; };
    const auto lighting = s.mode == BenchmarkMode::PathTracing ? BenchmarkEvidence::PathTracingGpu : BenchmarkEvidence::RasterGpu;
    const auto upscaling = s.mode == BenchmarkMode::DlssQuality ? BenchmarkEvidence::DlssGpu : BenchmarkEvidence::Off;
    const bool modeVerified = observed(BenchmarkFeature::Film) == BenchmarkEvidence::FilmGpu &&
        observed(BenchmarkFeature::Lighting) == lighting && observed(BenchmarkFeature::Upscaling) == upscaling;
    const bool score = modeVerified && s.outcome == ResultsOutcome::Completed && s.stage == BenchmarkStage::Finished &&
        s.reason == BenchmarkReason::None && s.measuredCount == kResultsMeasuredCount &&
        s.warmupCount == kResultsWarmupCount && std::isfinite(s.medianMs) && std::isfinite(s.worstMs) &&
        s.medianMs >= 0 && s.worstMs >= s.medianMs;
    const bool invalid = s.outcome == ResultsOutcome::Completed && !score;
    const auto reason = invalid ? (modeVerified ? BenchmarkReason::TimingUnavailable : BenchmarkReason::WorkloadChanged) : s.reason;
    const auto outcome = invalid ? ResultsOutcome::Unavailable : s.outcome;
    const auto unknown = resultText(T::Unknown);
    const auto absent = resultText(T::Unavailable);
    field(T::Typical, score ? decimal(s.medianMs) : absent, score);
    field(T::Worst, score ? decimal(s.worstMs) : absent, score);
    field(T::Warmup, std::format(L"{} / {}", s.warmupCount, kResultsWarmupCount), true);
    field(T::Samples, std::format(L"{} / {}", s.measuredCount, kResultsMeasuredCount), true);
    const auto seconds = context.viewerStartupSeconds;
    const bool started = !reference && seconds && std::isfinite(*seconds) && *seconds >= 0;
    field(T::Startup, started ? decimal(*seconds) : absent, started);
    std::wstring status = resultText(outcomeText(outcome));
    if (reason != BenchmarkReason::None && reason != BenchmarkReason::Cancelled)
        status += std::wstring(L" · ") + resultText(resultsReasonText(reason));
    field(T::Outcome, std::move(status));
    row(L"GPU", chip(s.vendorId ? s.vendorId : context.vendorId, s.vendorId ? s.deviceId : context.deviceId));
    // CPU identity comes from the cached inventory. Actual scripted work is
    // credited only after a measured frame used its output. No CPU/NPU timing
    // is inferred from the graphics queue's fixed-shot timestamps.
    std::wstring directorWork = resultText(T::Unknown);
    if (s.measuredCount && observed(BenchmarkFeature::Director) == BenchmarkEvidence::ScriptedCpu)
        directorWork = std::wstring(resultText(T::Ran)) + L" · " + resultText(T::Scripted);
    else if (observed(BenchmarkFeature::Director) == BenchmarkEvidence::Failed)
        directorWork = resultText(T::Failed);
    row(L"CPU", cpuModel(context.hardware) + L" · " + directorWork);
    row(L"NPU", npuModel(context.hardware) + L" · " + resultText(T::Off));
    const auto v = s.driverVersion;
    field(T::Driver, v ? std::format(L"{}.{}.{}.{}", v >> 48, (v >> 32) & 65535, (v >> 16) & 65535, v & 65535) : unknown, v != 0);
    field(T::Build, std::wstring(build), true);
    field(T::Architecture, reference ? L"x64 / x64" : architecture());
    field(T::Power, resultText(powerText(s.power)));
    const auto protocol = s.mode == BenchmarkMode::Raster ? L"RC1" : s.mode == BenchmarkMode::PathTracing ? L"PT1" : L"DLSS-Q1";
    const auto canvas = rtl ? L"\u202d1280 × 720\u202c" : L"1280 × 720";
    const auto interval = rtl ? L"\u202d00:08–00:14\u202c" : L"00:08–00:14";
    field(T::Workload, std::format(L"{} · {} · {} · {} · taipei_intro · {}",
        protocol, resultText(resultsModeText(s.mode)), resultText(T::SafeFamily), canvas, interval));
    std::wstring resolution = unknown;
    if (s.width && s.height) {
        resolution = std::format(L"{} × {}", s.width, s.height);
        if (s.inputWidth && s.inputHeight && (s.inputWidth != s.width || s.inputHeight != s.height))
            resolution = std::format(L"{} × {} → {}", s.inputWidth, s.inputHeight, resolution);
    }
    field(T::Canvas, std::move(resolution), s.width && s.height);
    constexpr T keys[]{T::Renderer, T::Lighting, T::Particles, T::Neural, T::Director, T::Codec};
    static_assert(std::size(keys) == static_cast<size_t>(BenchmarkFeature::Upscaling));
    for (size_t i = 0; i < std::size(keys); ++i) field(keys[i], evidence(s.evidence[i]));
    row(L"DLSS SR", evidence(observed(BenchmarkFeature::Upscaling)));
    return rows;
}

// Display widths for the fixed-pitch text file. The PNG and native ListView
// use actual, independent cell bounds. Preserve combining marks at wrap points.
size_t cellWidth(wchar_t ch) {
    if (ch >= 0x202a && ch <= 0x202e) return 0;
    WORD category{};
    GetStringTypeW(CT_CTYPE3, &ch, 1, &category);
    // Windows marks precomposed letters such as é as NONSPACING | ALPHA
    // and spacing punctuation such as · as DIACRITIC | SYMBOL. Both occupy
    // a cell. Only a combining mark attached to a base has zero advance.
    if ((category & (C3_NONSPACING | C3_VOWELMARK)) && !(category & (C3_ALPHA | C3_SYMBOL))) return 0;
    const bool wideScript = (ch >= 0x3040 && ch <= 0x30ff) || (ch >= 0xac00 && ch <= 0xd7a3);
    return (category & (C3_FULLWIDTH | C3_IDEOGRAPH)) || wideScript ? 2u : 1u;
}
size_t displayWidth(std::wstring_view s) {
    size_t n = 0;
    for (const auto ch : s) n += cellWidth(ch);
    return n;
}
std::vector<std::wstring_view> wrap(std::wstring_view text, size_t width) {
    std::vector<std::wstring_view> lines;
    while (!text.empty()) {
        size_t n = 0, count = 0, lastSpace = std::wstring_view::npos;
        while (n < text.size() && count + cellWidth(text[n]) <= width) {
            if (text[n] == L' ') lastSpace = n;
            count += cellWidth(text[n++]);
        }
        if (n < text.size() && lastSpace != text.npos && lastSpace) n = lastSpace;
        lines.push_back(text.substr(0, n));
        text.remove_prefix(n);
        while (!text.empty() && text.front() == L' ') text.remove_prefix(1);
    }
    return lines;
}
} // namespace

ResultCard makeResultsCard(const ResultsDialogContext& context, const BenchmarkSnapshot& s) {
    ResultCard card;
    card.title = std::wstring(L"BOO SAH SAH · ") + resultText(T::Results);
    card.subtitle = resultText(T::Community);
    card.rtl = UiStrings::instance().rightToLeft();
    card.headings = {resultText(T::Metric), resultText(T::ThisRun), resultText(T::Reference)};
    card.rows = runRows(context, s, BOO_WIDEN(BOO_RELEASE_VERSION), false, card.rtl);
    card.note = resultText(T::LowerIsBetter);
    if (const auto* ref = resultsReferenceFor(s)) {
        std::string_view version(ref->build);
        const std::wstring displayVersion(version.begin(), version.end());
        ResultsDialogContext referenceContext;
        referenceContext.hardware = ref->hardware;
        const auto comparison = runRows(referenceContext, ref->snapshot, displayVersion, true, card.rtl);
        for (size_t i = 0; i < card.rows.size(); ++i) card.rows[i].values[1] = comparison[i].values[0];
    } else {
        card.note += std::wstring(L" ") + resultText(T::NoReference);
    }
    card.note += std::wstring(L" ") + resultText(T::Conditions);
    return card;
}

std::wstring resultsCardText(const ResultCard& card) {
    std::array<size_t,3> widths{};
    for (size_t i = 0; i < widths.size(); ++i) widths[i] = displayWidth(card.headings[i]);
    for (const auto& row : card.rows) {
        widths[0] = (std::max)(widths[0], displayWidth(row.label));
        for (size_t i = 0; i < 2; ++i) widths[i+1] = (std::max)(widths[i+1], displayWidth(row.values[i].text));
    }
    // Bound line length, including for languages with long compound words.
    for (size_t i = 0; i < widths.size(); ++i) widths[i] = std::clamp<size_t>(widths[i], 10, i ? 38 : 32);
    std::wstring out = card.title + L"\r\n" + card.subtitle + L"\r\n\r\n";
    const auto line = [&](const std::array<std::wstring,3>& items, const std::array<bool,3>& numeric) {
        std::array<std::vector<std::wstring_view>,3> columns;
        size_t count = 1;
        for (size_t i = 0; i < 3; ++i) {
            columns[i] = wrap(items[i], widths[i]);
            count = (std::max)(count, columns[i].size());
        }
        for (size_t n = 0; n < count; ++n) {
            // Anchor complete logical columns left-to-right even if a text file
            // viewer picks the direction of its first Arabic label for the line.
            if (card.rtl) out += L"\u202a";
            for (size_t i = 0; i < 3; ++i) {
                const auto cell = n < columns[i].size() ? columns[i][n] : std::wstring_view{};
                const size_t pad = widths[i] - displayWidth(cell);
                if (numeric[i]) out.append(pad,L' ');
                out.append(cell);
                if (!numeric[i] && i+1 < 3) out.append(pad,L' ');
                if (i+1 < 3) out += L" | ";
            }
            if (card.rtl) out += L"\u202c";
            out += L"\r\n";
        }
    };
    line(card.headings, {false,true,true});
    for (const auto& row : card.rows)
        line({row.label, row.values[0].text, row.values[1].text}, {false,row.values[0].numeric,row.values[1].numeric});
    out += L"\r\n" + card.note;
    return out;
}
} // namespace boo

