#include "boo/results_card.hpp"
#include "boo/results_export.hpp"
#include "boo/ui_text.hpp"
#include "../src/app/results_export_transaction.hpp"
#include <windows.h>
#include <sddl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>

namespace {
unsigned failures{};
void check(bool value, int line) {
    if (!value) { ++failures; std::cerr << "results card check failed at line " << line << '\n'; }
}
#define CHECK(value) check((value), __LINE__)

std::wstring field(const boo::ResultCard& card, boo::ResultText key, size_t column = 0) {
    for (const auto& row : card.rows) if (row.label == boo::resultText(key)) return row.values.at(column).text;
    return {};
}

namespace fs = std::filesystem;
std::string read(const fs::path& p) {
    std::ifstream file(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
void inspectPng(const fs::path& p) {
    using Microsoft::WRL::ComPtr;
    const auto file = read(p);
    CHECK(file.starts_with("\x89PNG\r\n\x1a\n"));
    bool end = false;
    for (size_t i = 8; i < file.size();) {
        if (file.size() - i < 12) { CHECK(false); return; }
        const auto byte = [&](size_t n) { return static_cast<unsigned char>(file[i+n]); };
        const size_t length = (size_t(byte(0)) << 24) | (size_t(byte(1)) << 16) | (size_t(byte(2)) << 8) | byte(3);
        auto type = file.substr(i+4,4);
        CHECK(type == "IHDR" || type == "IDAT" || type == "IEND");
        if (file.size()-i-12 < length || end) { CHECK(false); return; }
        end = type == "IEND";
        i += length+12;
    }
    CHECK(end);
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> image;
    ComPtr<IWICFormatConverter> rgb;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(p.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder)) ||
        FAILED(decoder->GetFrame(0, &image)) || FAILED(wic->CreateFormatConverter(&rgb)) ||
        FAILED(rgb->Initialize(image.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                               nullptr, 0, WICBitmapPaletteTypeCustom))) { CHECK(false); return; }
    UINT w{}, h{};
    CHECK(SUCCEEDED(rgb->GetSize(&w, &h)) && w == 1280 && h > 400 && h <= 8192);
    if (!w || !h || w > 2048 || h > 8192) return;
    std::vector<BYTE> pixels(size_t(w)*h*4);
    CHECK(SUCCEEDED(rgb->CopyPixels(nullptr, w*4, static_cast<UINT>(pixels.size()), pixels.data())));
    size_t textRows = 0;
    for (UINT y = 15; y < h; ++y) {
        bool ink = false;
        for (UINT x = 0; x < w; ++x) {
            const size_t offset = (size_t(y)*w+x)*4;
            if (pixels[offset] >= 96 || pixels[offset+1] >= 96 || pixels[offset+2] >= 96) continue;
            CHECK(x >= 30 && x < w-30 && y >= 30 && y < h-30);
            ink = true;
        }
        if (ink) ++textRows;
    }
    CHECK(textRows > 200);
}

void pairedSaveTests(const boo::ResultsDialogContext& viewer, const boo::BenchmarkSnapshot& snapshot) {
    GUID id{}; wchar_t unique[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, unique, 40)) { CHECK(false); return; }
    const fs::path folder = fs::temp_directory_path() / (std::wstring(L"BooResultsTest-")+unique);
    if (!fs::create_directory(folder)) { CHECK(false); return; }
    const auto tested = [&](const boo::ResultCard& card, const wchar_t* locale, bool rtl, const wchar_t* name) {
        if (!boo::writeResultsFiles(folder, card, locale, rtl)) { CHECK(false); return; }
        const auto bytes = read(folder/name/L"BOO-SAH-SAH-results.txt");
        CHECK(!bytes.empty() && bytes.size() <= 8192);
        const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring decoded(n, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), decoded.data(), n);
        CHECK(boo::resultsCardText(card) == decoded);
        inspectPng(folder/name/L"BOO-SAH-SAH-results.png");
    };
    auto& strings = boo::UiStrings::instance();
    const auto english = boo::makeResultsCard(viewer, snapshot);
    tested(english, L"en-US", false, L"BOO-SAH-SAH-results");
    auto before = read(folder/L"BOO-SAH-SAH-results"/L"BOO-SAH-SAH-results.png");
    CHECK(strings.load("assets/strings", "ar-SA"));
    const auto arabic = boo::makeResultsCard(viewer, snapshot);
    CHECK(strings.rightToLeft() && arabic.title != english.title);
    tested(arabic, L"ar-SA", true, L"BOO-SAH-SAH-results-2");
    CHECK(before == read(folder/L"BOO-SAH-SAH-results"/L"BOO-SAH-SAH-results.png"));
    CHECK(before != read(folder/L"BOO-SAH-SAH-results-2"/L"BOO-SAH-SAH-results.png"));
    strings.clear();
    auto oversized = english;
    oversized.note.assign(8193,L'0');
    CHECK(!boo::writeResultsFiles(folder, oversized, L"en-US", false));
    // The private fixture grants read access and explicitly denies subdirectory
    // creation, including on elevated accounts. A later save to its parent succeeds.
    PSECURITY_DESCRIPTOR descriptor{};
    CHECK(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(D;;0x4;;;WD)(A;;FA;;;OW)(A;;FA;;;SY)", SDDL_REVISION_1, &descriptor, nullptr));
    if (descriptor) {
        SECURITY_ATTRIBUTES attrs{sizeof(attrs),descriptor,FALSE};
        const auto denied = folder/L"denied";
        CHECK(CreateDirectoryW(denied.c_str(), &attrs));
        CHECK(!boo::writeResultsFiles(denied, english, L"en-US", false));
        CHECK(fs::is_empty(denied));
        CHECK(RemoveDirectoryW(denied.c_str()));
        LocalFree(descriptor);
    }
    // Inject a failed second write after both staged paths have appeared,
    // matching an out-of-space error partway through the image. Neither may publish.
    CHECK(!boo::detail::publishResultsPair(folder, [&](const fs::path& stage) {
        std::ofstream(stage/L"BOO-SAH-SAH-results.txt") << "complete";
        std::ofstream(stage/L"BOO-SAH-SAH-results.png") << "partial";
        SetLastError(ERROR_DISK_FULL);
        return false;
    }));
    CHECK(std::distance(fs::directory_iterator(folder), fs::directory_iterator()) == 2);
    tested(english, L"en-US", false, L"BOO-SAH-SAH-results-3");
    for (auto name : {L"BOO-SAH-SAH-results", L"BOO-SAH-SAH-results-2", L"BOO-SAH-SAH-results-3"}) {
        fs::remove(folder/name/L"BOO-SAH-SAH-results.png");
        fs::remove(folder/name/L"BOO-SAH-SAH-results.txt");
        fs::remove(folder/name);
    }
    fs::remove(folder);
}
}

int main() {
    using namespace boo;
    using T = ResultText;
    ResultsDialogContext viewer;
    viewer.adapter.luidLow = 987654321;
    viewer.vendorId = 0x10de;
    viewer.deviceId = 0x28a0;
    viewer.hardware = {ResultsCpuModel::IntelCoreI9_13900H, ResultsNpuModel::NoneDetected};
    viewer.viewerStartupSeconds = 0.0;
    BenchmarkSnapshot s;
    s.stage = BenchmarkStage::Finished;
    s.warmupCount = kResultsWarmupCount;
    s.measuredCount = kResultsMeasuredCount;
    s.outcome = ResultsOutcome::Completed;
    s.width = s.inputWidth = 1280;
    s.height = s.inputHeight = 720;
    s.driverVersion = (32ULL << 48) | (5ULL << 32) | (12ULL << 16) | 3456;
    s.medianMs = 1;
    s.worstMs = 3;
    s.evidence[0] = BenchmarkEvidence::FilmGpu;
    s.evidence[1] = BenchmarkEvidence::RasterGpu;
    s.evidence[static_cast<size_t>(BenchmarkFeature::Director)] = BenchmarkEvidence::ScriptedCpu;
    s.power = BenchmarkPower::Mains;
    s.evidence[static_cast<size_t>(BenchmarkFeature::Upscaling)] = BenchmarkEvidence::Off;
    const auto matchingSnapshot = s;
    const auto render = [&] { return makeResultsCard(viewer, s); };
    const auto raster = render();
    CHECK(field(raster, T::Outcome) == resultText(T::Completed));
    CHECK(field(raster, T::Typical) != resultText(T::Unavailable));
    CHECK(field(raster, T::Startup) != resultText(T::Unavailable));
    CHECK(field(raster, T::Driver) == L"32.5.12.3456");
    const auto tableText = resultsCardText(raster);
    CHECK(tableText.find(L"987654321") == tableText.npos);
    bool distinctHardware = false;
    for (const auto& row : raster.rows) if (row.label == L"GPU") {
        distinctHardware = row.values[0].text == L"NVIDIA GeForce RTX 4060 Laptop GPU" &&
                         row.values[1].text == L"NVIDIA GeForce RTX 5080 Laptop GPU";
    }
    CHECK(distinctHardware);
    const auto hardwareCell = [](const ResultCard& card, std::wstring_view label, size_t column) {
        for (const auto& row : card.rows) if (row.label == label) return row.values[column].text;
        return std::wstring();
    };
    const auto cpuWork = std::wstring(L" · ") + resultText(T::Ran) + L" · " + resultText(T::Scripted);
    const auto npuOff = std::wstring(L" · ") + resultText(T::Off);
    CHECK(hardwareCell(raster,L"CPU",0) == L"Intel Core i9-13900H" + cpuWork);
    CHECK(hardwareCell(raster,L"CPU",1) == L"Intel Core Ultra 9 275HX" + cpuWork);
    CHECK(hardwareCell(raster,L"NPU",0) == resultText(T::NoNpu) + npuOff);
    CHECK(hardwareCell(raster,L"NPU",1) == L"Intel AI Boost" + npuOff);
    {
        auto unknownHost = viewer;
        unknownHost.hardware = {static_cast<ResultsCpuModel>(255), static_cast<ResultsNpuModel>(255)};
        auto earlyFailure = s;
        earlyFailure.measuredCount = 0;
        earlyFailure.outcome = ResultsOutcome::Unavailable;
        const auto unavailable = makeResultsCard(unknownHost, earlyFailure);
        CHECK(hardwareCell(unavailable,L"CPU",0) == std::wstring(resultText(T::Unknown)) + L" · " + resultText(T::Unknown));
        CHECK(hardwareCell(unavailable,L"NPU",0) == resultText(T::Unknown) + npuOff);
        CHECK(hardwareCell(unavailable,L"CPU",1) == L"—" && hardwareCell(unavailable,L"NPU",1) == L"—");
        unknownHost.hardware = {ResultsCpuModel::Intel, ResultsNpuModel::Unknown, 6, 0xba, 0x8086, 0xabcd};
        const auto publicCodes = makeResultsCard(unknownHost,earlyFailure);
        CHECK(hardwareCell(publicCodes,L"CPU",0).starts_with(L"Intel · CPUID 06:BA"));
        CHECK(hardwareCell(publicCodes,L"NPU",0).starts_with(L"Intel · PCI 8086:ABCD"));
    }
    CHECK(raster.headings[2] == resultText(T::Reference));
    CHECK(tableText.find(L"RC1") != tableText.npos);
    CHECK(field(raster, T::Typical, 1) != L"—");
    CHECK(field(raster, T::Worst, 1) != L"—");
    CHECK(field(raster,T::Samples,1) == L"180 / 180");
    CHECK(raster.rows.front().label == resultText(T::Typical));
    CHECK(raster.rows[1].label == resultText(T::Worst));
    const auto columnEnds = [&](T key) {
        const auto pos = tableText.find(resultText(key));
        auto line = tableText.substr(pos,tableText.find(L"\r\n",pos)-pos);
        return std::pair{line.rfind(L" | "),line.size()};
    };
    CHECK(columnEnds(T::Typical) == columnEnds(T::Worst));
    // In English every logical line of the fixed-pitch table must have the
    // same separator columns, even an accented letter or a spacing middle dot.
    auto punctuation = raster;
    punctuation.rows[3].values[0] = {L"Résumé · D3D12 · GPU"};
    const auto padded = resultsCardText(punctuation);
    size_t first = std::wstring::npos, second = std::wstring::npos;
    for (size_t start = 0; start < padded.size();) {
        const auto end = padded.find(L"\r\n", start);
        const auto line = padded.substr(start, end == padded.npos ? padded.npos : end-start);
        if (const auto sep = line.find(L" | "); sep != line.npos) {
            if (first == padded.npos) { first = sep; second = line.rfind(L" | "); }
            CHECK(first == sep && second == line.rfind(L" | "));
        }
        if (end == padded.npos) break;
        start = end + 2;
    }
    s.power = BenchmarkPower::Battery;
    CHECK(field(render(),T::Typical,1) == L"—");
    s = matchingSnapshot;
    s.evidence[static_cast<size_t>(BenchmarkFeature::Director)] = BenchmarkEvidence::Unknown;
    CHECK(field(render(), T::Typical, 1) == L"—");
    s = matchingSnapshot;
    --s.width;
    CHECK(field(render(),T::Worst,1) == L"—");
    s = matchingSnapshot;
    --s.measuredCount;
    CHECK(field(render(),T::Worst,1) == L"—");
    for (const auto mode : {BenchmarkMode::Raster, BenchmarkMode::PathTracing}) {
        s = matchingSnapshot;
        s.mode = mode;
        s.evidence[1] = mode == BenchmarkMode::Raster ? BenchmarkEvidence::RasterGpu : BenchmarkEvidence::PathTracingGpu;
        s.evidence[static_cast<size_t>(BenchmarkFeature::Upscaling)] = BenchmarkEvidence::DlssGpu;
        const auto substituted = render();
        CHECK(field(substituted, T::Typical) == resultText(T::Unavailable));
        CHECK(field(substituted, T::Worst) == resultText(T::Unavailable));
        CHECK(field(substituted, T::Typical, 1) == L"—");
    }
    s = matchingSnapshot;
    s.mode = BenchmarkMode::PathTracing;
    CHECK(field(render(), T::Typical) == resultText(T::Unavailable));
    s.evidence[1] = BenchmarkEvidence::PathTracingGpu;
    CHECK(field(render(), T::Outcome) == resultText(T::Completed));
    CHECK(field(render(), T::Workload).find(L"PT1") != std::wstring::npos);
    CHECK(field(render(), T::Typical, 1) != L"—");
    s.mode = BenchmarkMode::DlssQuality;
    s.evidence[1] = BenchmarkEvidence::RasterGpu;
    CHECK(field(render(), T::Worst) == resultText(T::Unavailable));
    s.evidence[static_cast<size_t>(BenchmarkFeature::Upscaling)] = BenchmarkEvidence::DlssGpu;
    CHECK(field(render(), T::Outcome) == resultText(T::Completed));
    CHECK(field(render(), T::Workload).find(L"DLSS-Q1") != std::wstring::npos);
    CHECK(field(render(), T::Typical, 1) == L"—");
    s.inputWidth = 853;
    s.inputHeight = 480;
    CHECK(field(render(), T::Typical, 1) != L"—");
    --s.measuredCount;
    CHECK(field(render(), T::Worst) == resultText(T::Unavailable));
    s.measuredCount = kResultsMeasuredCount;
    s.medianMs = std::numeric_limits<double>::quiet_NaN();
    CHECK(field(render(), T::Typical) == resultText(T::Unavailable));
    s.medianMs = 1.0;
    for (auto outcome : {ResultsOutcome::Cancelled, ResultsOutcome::Interrupted, ResultsOutcome::Unavailable}) {
        s.outcome = outcome;
        CHECK(field(render(), T::Typical) == resultText(T::Unavailable));
        CHECK(field(render(), T::Worst) == resultText(T::Unavailable));
    }
    viewer.viewerStartupSeconds.reset();
    CHECK(field(render(), T::Startup) == resultText(T::Unavailable));
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        // Every localized report, including its numeric padding and UTF-8
        // glyphs, fits within the original privacy/export size limit.
        auto& strings = UiStrings::instance();
        for (const auto& lang : uiAvailableLanguages("assets/strings")) {
            CHECK(strings.load("assets/strings",lang));
            const auto text = resultsCardText(makeResultsCard(viewer,matchingSnapshot));
            const auto bytes = WideCharToMultiByte(CP_UTF8,0,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
            CHECK(bytes > 0 && bytes <= 8192);
        }
        strings.clear();
        pairedSaveTests(viewer, matchingSnapshot);
        CoUninitialize();
    } else CHECK(false);
    return failures ? 1 : 0;
}
