#include "boo/results_export.hpp"
#include "boo/results_card.hpp"
#include "results_export_transaction.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace boo {
namespace {
using Microsoft::WRL::ComPtr;
using Path = std::filesystem::path;
constexpr wchar_t kStem[] = L"BOO-SAH-SAH-results";
constexpr wchar_t kPng[] = L"BOO-SAH-SAH-results.png";
constexpr wchar_t kTxt[] = L"BOO-SAH-SAH-results.txt";
constexpr int kMaxTextBytes = 8192;
constexpr UINT kWidth = 1280, kMargin = 44;

struct Handle {
    HANDLE value;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

bool isLocal(const Path& directory) {
    auto localDrive = [](const Path& path) {
        const auto type = GetDriveTypeW(path.root_path().c_str());
        return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_RAMDISK;
    };
    if (!directory.is_absolute() || directory.native().starts_with(L"\\\\") || !localDrive(directory)) return false;
    Handle handle{CreateFileW(directory.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) return false;
    std::wstring final(32768, L'\0');
    const auto n = GetFinalPathNameByHandleW(handle.value, final.data(), static_cast<DWORD>(final.size()), VOLUME_NAME_DOS);
    if (!n || n >= final.size()) return false;
    final.resize(n);
    if (!final.starts_with(L"\\\\?\\") || final.size() < 7 || final[5] != L':' || final[6] != L'\\') return false;
    return localDrive(Path(final.substr(4)));
}

bool encodeCard(const ResultCard& card, std::wstring_view language, bool rtl, std::vector<BYTE>& png, UINT& height) {
    ComPtr<IDWriteFactory> text;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(text.GetAddressOf()))) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())) ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    const std::wstring locale(language);
    struct Cell { ComPtr<IDWriteTextLayout> layout; D2D1_POINT_2F origin; };
    struct Band { float top, bottom; };
    std::vector<Cell> cells;
    std::vector<Band> bands;
    std::vector<float> rules;
    constexpr float full = kWidth - 2.0f*kMargin;
    constexpr std::array<float,3> widths{378, 407, full-378-407};
    constexpr float inset = 12;
    float y = kMargin;
    auto add = [&](std::wstring_view value, float x, float top, float width, float em,
                   bool bold, bool numeric, bool right, float& measuredHeight) {
        ComPtr<IDWriteTextFormat> format;
        ComPtr<IDWriteTextLayout> layout;
        // Consolas has equal advances for all ten digits. Each numeric cell is
        // positioned against the same column edge at its one-decimal precision.
        const auto family = numeric ? L"Consolas" : L"Segoe UI";
        if (FAILED(text->CreateTextFormat(family, nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, em, locale.c_str(), &format))) return false;
        const bool readRtl = rtl && !numeric;
        if (FAILED(format->SetReadingDirection(readRtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT)) ||
            FAILED(format->SetTextAlignment(right == readRtl ? DWRITE_TEXT_ALIGNMENT_LEADING : DWRITE_TEXT_ALIGNMENT_TRAILING)) ||
            FAILED(text->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format.Get(), width, 8192, &layout))) return false;
        DWRITE_TEXT_METRICS metrics{};
        if (FAILED(layout->GetMetrics(&metrics)) || metrics.height > 4096) return false;
        measuredHeight = (std::max)(measuredHeight, metrics.height);
        cells.push_back({std::move(layout), D2D1::Point2F(x,top)});
        return true;
    };
    auto paragraph = [&](const std::wstring& value, float size, bool bold, float gap) {
        float h = 0;
        if (!add(value, kMargin, y, full, size, bold, false, rtl, h)) return false;
        y += h + gap;
        return true;
    };
    if (!paragraph(card.title,32,true,3) || !paragraph(card.subtitle,19,false,20)) return false;
    const auto tableRow = [&](const std::array<std::wstring,3>& value, const std::array<bool,3>& numeric,
                              bool header, bool metric) {
        float height = 0, offset = 0;
        for (size_t i = 0; i < 3; ++i) {
            const float left = rtl ? kMargin + full - offset - widths[i] : kMargin + offset;
            const bool right = numeric[i] || rtl || (header && i != 0);
            if (!add(value[i], left+inset, y+7, widths[i]-2*inset, metric ? 23.0f : 20.0f,
                    header || i == 0 || metric, numeric[i], right, height)) return false;
            offset += widths[i];
        }
        const float top = y;
        y += std::ceil(height) + 14;
        if (header || metric) bands.push_back({top,y});
        rules.push_back(y);
        return true;
    };
    if (!tableRow(card.headings, {false,false,false}, true, false)) return false;
    for (size_t i = 0; i < card.rows.size(); ++i) {
        const auto& row = card.rows[i];
        if (!tableRow({row.label,row.values[0].text,row.values[1].text},
                {false,row.values[0].numeric,row.values[1].numeric}, false, i < 2)) return false;
    }
    y += 16;
    if (!paragraph(card.note,19,false,0) || y > 7800) return false;
    height = static_cast<UINT>(std::ceil(y)) + kMargin;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> ink;
    const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(wic->CreateBitmap(kWidth, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap)) ||
        FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), properties, &target)) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(0x18262a), &ink))) return false;
    target->BeginDraw();
    target->Clear(D2D1::ColorF(0xf9faf7));
    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    for (size_t i = 0; i < bands.size(); ++i) {
        ink->SetColor(D2D1::ColorF(i ? 0xeaf4ee : 0xd4e6dc));
        target->FillRectangle(D2D1::RectF(kMargin,bands[i].top,kWidth-kMargin,bands[i].bottom),ink.Get());
    }
    ink->SetColor(D2D1::ColorF(0xd2dcd7));
    for (const float bottom : rules) target->DrawLine(D2D1::Point2F(kMargin,bottom),D2D1::Point2F(kWidth-kMargin,bottom),ink.Get());
    ink->SetColor(D2D1::ColorF(0x18262a));
    for (const auto& cell : cells) target->DrawTextLayout(cell.origin,cell.layout.Get(),ink.Get());
    ink->SetColor(D2D1::ColorF(0x18705c));
    target->FillRectangle(D2D1::RectF(0, 0, kWidth, 7), ink.Get());
    if (FAILED(target->EndDraw())) return false;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(kWidth, height)) || FAILED(frame->SetPixelFormat(&pf)) ||
        !IsEqualGUID(pf, GUID_WICPixelFormat24bppBGR) || FAILED(frame->WriteSource(bitmap.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) return false;

    // Keep only critical PNG chunks. In particular, WIC can add a resolution
    // chunk automatically. The exported file has no text, EXIF or timestamps.
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 32ULL*1024*1024 ||
        stat.cbSize.QuadPart < 33 || FAILED(stream->Seek({}, STREAM_SEEK_SET, nullptr))) return false;
    std::vector<BYTE> source(stat.cbSize.LowPart);
    ULONG got{};
    if (FAILED(stream->Read(source.data(), stat.cbSize.LowPart, &got)) || got != source.size()) return false;
    png.assign(source.begin(), source.begin()+8);
    bool header = false, data = false, end = false;
    for (size_t pos = 8; pos < source.size();) {
        if (source.size()-pos < 12 || end) return false;
        const auto* p = source.data()+pos;
        const size_t length = (size_t(p[0]) << 24) | (size_t(p[1]) << 16) | (size_t(p[2]) << 8) | p[3];
        if (length > source.size()-pos-12) return false;
        const auto next = pos+length+12;
        const std::string_view type(reinterpret_cast<const char*>(p+4), 4);
        if (type == "IHDR" || type == "IDAT" || type == "IEND") {
            header |= type == "IHDR";
            data |= type == "IDAT";
            end = type == "IEND";
            png.insert(png.end(), source.data()+pos, source.data()+next);
        }
        pos = next;
    }
    return header && data && end;
}

bool writeAndVerify(const Path& file, const void* data, DWORD size) {
    Handle handle{CreateFileW(file.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
    if (handle.value == INVALID_HANDLE_VALUE) return false;
    DWORD n{};
    if (!WriteFile(handle.value, data, size, &n, nullptr) || n != size ||
        !FlushFileBuffers(handle.value) || !SetFilePointerEx(handle.value, {}, nullptr, FILE_BEGIN)) return false;
    BYTE buffer[16384];
    const auto* source = static_cast<const BYTE*>(data);
    for (DWORD offset = 0; offset < size; offset += n) {
        const DWORD chunk = (std::min)(size - offset, static_cast<DWORD>(sizeof(buffer)));
        if (!ReadFile(handle.value, buffer, chunk, &n, nullptr) || n != chunk || std::memcmp(buffer, source+offset, n)) return false;
    }
    return true;
}

struct Staging {
    Path folder;
    bool active = false;
    ~Staging() {
        if (!active) return;
        DeleteFileW((folder / kPng).c_str());
        DeleteFileW((folder / kTxt).c_str());
        RemoveDirectoryW(folder.c_str());
    }
};
} // namespace

bool detail::publishResultsPair(const Path& directory, const std::function<bool(const Path&)>& stage) {
    GUID guid{};
    wchar_t id[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, id, 40)) return false;
    Staging staged{directory / (std::wstring(L".BOO-SAH-SAH-") + id)};
    if (!CreateDirectoryW(staged.folder.c_str(), nullptr)) return false;
    staged.active = true;
    if (!stage(staged.folder)) return false;

    for (unsigned suffix = 1; suffix <= 10000; ++suffix) {
        auto folder = directory / (std::wstring(kStem) + (suffix == 1 ? L"" : L"-" + std::to_wstring(suffix)));
        if (MoveFileExW(staged.folder.c_str(), folder.c_str(), MOVEFILE_WRITE_THROUGH)) {
            staged.active = false;
            return true;
        }
        const auto error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS) return false;
    }
    return false;
}

bool writeResultsFiles(const Path& directory, const ResultCard& card, std::wstring_view language, bool rtl) {
    if (card.rows.empty() || !isLocal(directory)) return false;
    const std::wstring content = resultsCardText(card);
    if (content.empty() || content.size() > kMaxTextBytes) return false;
    const int length = static_cast<int>(content.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content.data(), length, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0 || bytes > kMaxTextBytes) return false;
    std::string utf8(bytes, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, content.data(), length, utf8.data(), bytes, nullptr, nullptr) != bytes) return false;
    std::vector<BYTE> png;
    UINT height{};
    if (!encodeCard(card, language, rtl, png, height)) return false;
    return detail::publishResultsPair(directory, [&](const Path& stage) {
        return writeAndVerify(stage / kTxt, utf8.data(), bytes) &&
               writeAndVerify(stage / kPng, png.data(), static_cast<DWORD>(png.size()));
    });
}

ResultsSaveStatus pickAndSaveResults(HWND owner, const ResultCard& card, const std::wstring& language,
                                    bool rtl, const std::wstring& title) {
    ComPtr<IFileOpenDialog> picker;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&picker))))
        return ResultsSaveStatus::Failed;
    FILEOPENDIALOGOPTIONS options{};
    if (FAILED(picker->GetOptions(&options)) || FAILED(picker->SetOptions(options | FOS_PICKFOLDERS |
        FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_DONTADDTORECENT | FOS_NOCHANGEDIR))) return ResultsSaveStatus::Failed;
    picker->SetTitle(title.c_str());
    ComPtr<IShellItem> pictures;
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Pictures, KF_FLAG_DONT_VERIFY, nullptr, IID_PPV_ARGS(&pictures))))
        picker->SetFolder(pictures.Get());
    const HRESULT shown = picker->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return ResultsSaveStatus::Cancelled;
    ComPtr<IShellItem> item;
    PWSTR path{};
    if (FAILED(shown) || FAILED(picker->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        return ResultsSaveStatus::Failed;
    struct FreePath { PWSTR path; ~FreePath() { CoTaskMemFree(path); } } freePath{path};
    return writeResultsFiles(path, card, language, rtl) ? ResultsSaveStatus::Saved : ResultsSaveStatus::Failed;
}
} // namespace boo
