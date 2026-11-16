#include "boo/ui_text.hpp"

#include "boo/json.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <system_error>

using Microsoft::WRL::ComPtr;

namespace boo {

namespace {

constexpr UiLabelSpec kLabelSpecs[static_cast<size_t>(UiLabel::Count)] = {
    {"hud.prev",        4.0f, 1.50f, true,  0}, {"hud.play",       5.4f, 1.50f, true,  0},
    {"hud.pause",       5.4f, 1.50f, true,  0}, {"hud.next",       5.0f, 1.55f, true,  0},
    {"hud.mode_film",   8.8f, 1.50f, true,  0}, {"hud.mode_free",  8.8f, 1.50f, true,  0},
    {"hud.live_gpu",   15.0f, 1.55f, false, 0}, {"hud.col_size",   8.0f, 1.40f, false, 0},
    {"hud.col_rate",    8.0f, 1.40f, false, 0},
    {"hud.state_live",  5.0f, 1.45f, true,  0}, {"hud.state_sub",  5.0f, 1.45f, true,  0},
    {"hud.state_off",   5.0f, 1.45f, true,  0}, {"hud.state_fault",5.0f, 1.45f, true,  0},
    {"hud.bench",      11.0f, 1.50f, false, 0},
    {"hud.cam_free",    5.6f, 1.35f, true,  0}, {"hud.cam_film",   5.6f, 1.35f, true,  0},
    {"hud.lamp",        4.7f, 1.35f, true,  0}, {"hud.wipe",       4.7f, 1.35f, true,  0},
    {"hud.marbles",     4.9f, 1.35f, true,  0},
    {"hud.sun",         3.5f, 1.35f, false, 0}, {"hud.azimuth",    3.5f, 1.35f, false, 0},
    {"hud.mute",        5.0f, 1.35f, true,  0}, {"hud.music",      5.0f, 1.35f, true,  0},
    {"hud.reset",       4.1f, 1.35f, true,  0}, {"hud.next_scene", 9.9f, 1.35f, true,  0},
    {"hud.solo",        6.0f, 1.40f, false, 0}, {"hud.all",        7.4f, 1.40f, true,  0},
    {"hud.act", 8.9f, 1.35f, false, 1}, {"hud.act", 8.9f, 1.35f, false, 2}, {"hud.act", 8.9f, 1.35f, false, 3},
    {"hud.act", 8.9f, 1.35f, false, 4}, {"hud.act", 8.9f, 1.35f, false, 5}, {"hud.act", 8.9f, 1.35f, false, 6},
    {"hud.act", 8.9f, 1.35f, false, 7},
    {"hud.scene.bathroom", 8.9f, 1.35f, false, 0}, {"hud.scene.faucet",    8.9f, 1.35f, false, 0},
    {"hud.scene.leaves",   8.9f, 1.35f, false, 0}, {"hud.scene.orchid",    8.9f, 1.35f, false, 0},
    {"hud.scene.enamel",   8.9f, 1.35f, false, 0}, {"hud.scene.sanctuary", 8.9f, 1.35f, false, 0},
    {"hud.scene.custom",   8.9f, 1.35f, false, 0},
    {"hud.aov.albedo",   16.8f, 1.45f, false, 0}, {"hud.aov.normal",      16.8f, 1.45f, false, 0},
    {"hud.aov.rough_metal", 16.8f, 1.45f, false, 0}, {"hud.aov.direct",   16.8f, 1.45f, false, 0},
    {"hud.aov.indirect", 16.8f, 1.45f, false, 0}, {"hud.aov.reflect",     16.8f, 1.45f, false, 0},
    {"hud.aov.ray_heat", 16.8f, 1.45f, false, 0},
    {"hud.live_camera",  13.4f, 1.55f, false, 0},
    {"hud.ost.synth",    24.0f, 1.32f, false, 0}, {"hud.ost.custom",      24.0f, 1.32f, false, 0},
};

std::wstring utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
    return out;
}

std::string wideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string primarySubtag(std::string_view tag) {
    const size_t dash = tag.find('-');
    return lowerAscii(tag.substr(0, dash));
}

bool isEnglishTag(std::string_view tag) { return primarySubtag(tag) == "en"; }

// Traditional Chinese regions and the script subtag; everything else Chinese reads Simplified.
bool isTraditionalChinese(std::string_view tag) {
    const std::string t = lowerAscii(tag);
    return t.find("hant") != std::string::npos || t.ends_with("-tw") || t.ends_with("-hk") || t.ends_with("-mo");
}

// Accented look-alikes keep the pseudo-language readable while proving nothing assumes ASCII.
std::wstring pseudoLocalize(std::wstring_view english, bool lengthen) {
    std::wstring out;
    bool inBrace = false;
    for (wchar_t c : english) {
        if (c == L'{') inBrace = true;
        if (inBrace) { out.push_back(c); if (c == L'}') inBrace = false; continue; }
        switch (c) {
            case L'A': c = L'Å'; break; case L'E': c = L'É'; break; case L'I': c = L'Î'; break;
            case L'O': c = L'Ö'; break; case L'U': c = L'Ü'; break; case L'C': c = L'Ç'; break;
            case L'N': c = L'Ñ'; break; case L'a': c = L'å'; break; case L'e': c = L'é'; break;
            case L'i': c = L'î'; break; case L'o': c = L'ö'; break; case L'u': c = L'ü'; break;
            default: break;
        }
        out.push_back(c);
    }
    if (lengthen) out.append(std::max<size_t>(1, english.size() / 3), L'~');
    return out;
}

struct LanguageFonts {
    const wchar_t* hud;
    const wchar_t* caption;
    bool condensedHud;
};

// Windows 11 system fonts only; nothing is bundled. DirectWrite falls back per glyph where needed.
LanguageFonts fontsFor(std::string_view tag) {
    const std::string p = primarySubtag(tag);
    if (p == "ja") return {L"Yu Gothic UI", L"Yu Gothic UI", false};
    if (p == "ko") return {L"Malgun Gothic", L"Malgun Gothic", false};
    if (p == "zh") return isTraditionalChinese(tag) ? LanguageFonts{L"Microsoft JhengHei UI", L"Microsoft JhengHei UI", false}
                                                    : LanguageFonts{L"Microsoft YaHei UI", L"Microsoft YaHei UI", false};
    if (p == "hi") return {L"Nirmala UI", L"Nirmala UI", false};
    if (p == "th") return {L"Leelawadee UI", L"Leelawadee UI", false};
    if (p == "ar" || p == "vi") return {L"Segoe UI", L"Segoe UI", false};
    return {L"Bahnschrift", L"Segoe UI", true};
}

} // namespace

const UiLabelSpec& uiLabelSpec(UiLabel label) noexcept {
    const size_t i = static_cast<size_t>(label);
    return kLabelSpecs[i < static_cast<size_t>(UiLabel::Count) ? i : 0];
}

// ---------------------------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------------------------

UiStrings& UiStrings::instance() {
    static UiStrings s;
    return s;
}

void UiStrings::clear() {
    active_ = false;
    rtl_ = false;
    tag_ = "en-US";
    strings_.clear();
    english_.clear();
    captionKeys_.clear();
    ++generation_;
}

bool UiStrings::load(const std::filesystem::path& dir, std::string_view languageTag, std::string* error) {
    clear();

    const JsonParseResult source = parseJsonFile(dir / "en-US.json");
    const JsonValue* sourceStrings = source.ok ? source.value.find("strings") : nullptr;
    if (!sourceStrings || !sourceStrings->isObject()) {
        if (error) *error = "String catalog en-US.json is missing or invalid: " + source.error;
        return false;
    }
    for (const auto& [key, entry] : sourceStrings->asObject()) {
        const JsonValue* text = entry.find("text");
        if (!text || !text->isString()) continue;
        english_[key] = utf8ToWide(text->asString());
        if (key.rfind("cap.", 0) == 0) captionKeys_[text->asString()] = key;
    }

    if (languageTag.empty() || isEnglishTag(languageTag)) return true;

    if (lowerAscii(languageTag) == "qps-ploc") {
        for (const auto& [key, text] : english_) strings_[key] = pseudoLocalize(text, key.rfind("cap.", 0) == 0);
        tag_ = "qps-ploc";
        active_ = true;
        return true;
    }

    const std::filesystem::path file = dir / (std::string(languageTag) + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) return true; // no catalog for this language: English

    const JsonParseResult parsed = parseJsonFile(file);
    const JsonValue* strings = parsed.ok ? parsed.value.find("strings") : nullptr;
    if (!strings || !strings->isObject()) {
        if (error) *error = "String catalog " + file.filename().string() + " is invalid: " + parsed.error;
        return false;
    }
    for (const auto& [key, value] : strings->asObject()) {
        if (!value.isString() || value.asString().empty()) continue;
        if (english_.find(key) == english_.end()) continue; // unknown key: ignore
        strings_[key] = utf8ToWide(value.asString());
    }
    if (strings_.empty()) return true;

    tag_ = std::string(languageTag);
    const std::string p = primarySubtag(languageTag);
    rtl_ = (p == "ar" || p == "he" || p == "fa" || p == "ur");
    active_ = true;
    return true;
}

const std::wstring* UiStrings::find(std::string_view key) const {
    const auto it = strings_.find(std::string(key));
    return it == strings_.end() ? nullptr : &it->second;
}

const std::wstring* UiStrings::findEnglish(std::string_view key) const {
    const auto it = english_.find(std::string(key));
    return it == english_.end() ? nullptr : &it->second;
}

std::wstring UiStrings::textOr(std::string_view key, std::wstring_view fallback) const {
    if (const std::wstring* s = find(key)) return *s;
    return std::wstring(fallback);
}

std::string UiStrings::captionKey(std::string_view englishText) const {
    const auto it = captionKeys_.find(std::string(englishText));
    return it == captionKeys_.end() ? std::string() : it->second;
}

const std::wstring* UiStrings::findCaption(std::string_view englishText) const {
    if (!active_) return nullptr;
    const auto it = captionKeys_.find(std::string(englishText));
    return it == captionKeys_.end() ? nullptr : find(it->second);
}

std::vector<std::string> uiAvailableLanguages(const std::filesystem::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json") continue;
        out.push_back(it->path().stem().string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::string uiMatchLanguage(const std::vector<std::string>& preferred, const std::vector<std::string>& available) {
    for (const std::string& want : preferred) {
        const std::string wantLower = lowerAscii(want);
        const std::string wantPrimary = primarySubtag(want);
        if (wantPrimary == "en") return "en-US";
        for (const std::string& have : available) {
            if (lowerAscii(have) == wantLower) return have;
        }
        if (wantPrimary == "zh") {
            const char* target = isTraditionalChinese(want) ? "zh-tw" : "zh-cn";
            for (const std::string& have : available) {
                if (lowerAscii(have) == target) return have;
            }
        }
        // Norwegian: Windows reports nb or nn (or the macro-language no); one catalog serves all.
        const bool norwegian = (wantPrimary == "nb" || wantPrimary == "nn" || wantPrimary == "no");
        for (const std::string& have : available) {
            const std::string havePrimary = primarySubtag(have);
            if (havePrimary == wantPrimary || (norwegian && havePrimary == "nb")) return have;
        }
    }
    return "en-US";
}

std::vector<std::string> uiWindowsPreferredLanguages() {
    std::vector<std::string> out;
    ULONG count = 0, chars = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, nullptr, &chars) && chars > 0) {
        std::wstring buffer(chars, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buffer.data(), &chars)) {
            for (const wchar_t* p = buffer.c_str(); *p; p += wcslen(p) + 1) out.push_back(wideToUtf8(p));
        }
    }
    if (out.empty()) out.push_back("en-US");
    return out;
}

// ---------------------------------------------------------------------------------------------
// Rasterizer
// ---------------------------------------------------------------------------------------------

struct UiTextLayer::Impl {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;       // scratch surface, premultiplied BGRA
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> brush;
    bool comInitialized = false;
    bool ok = false;

    std::vector<uint8_t> atlas;
    uint64_t labelsGeneration = ~0ull;
    uint32_t labelsUiScale = 0;
    std::wstring captionText;
    float captionEm = 0.0f;
    float captionMaxWidth = 0.0f;
    uint64_t captionGeneration = ~0ull;
    bool captionValid = false;
    uint32_t captionW = 0;
    uint32_t captionH = 0;

    static constexpr uint32_t kScratchW = UiTextAtlasLayout::kWidth;
    static constexpr uint32_t kScratchH = UiTextAtlasLayout::kCaptionMaxH;

    Impl() {
        atlas.assign(static_cast<size_t>(UiTextAtlasLayout::kWidth) * UiTextAtlasLayout::kHeight, 0);
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInitialized = SUCCEEDED(co); // RPC_E_CHANGED_MODE: COM is already up in another mode, which is fine
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) return;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
        if (FAILED(wic->CreateBitmap(kScratchW, kScratchH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))) return;
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target))) return;
        target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush))) return;
        ok = true;
    }

    ~Impl() {
        brush.Reset(); target.Reset(); bitmap.Reset(); wic.Reset(); dwrite.Reset(); d2d.Reset();
        if (comInitialized) CoUninitialize();
    }

    ComPtr<IDWriteTextFormat> makeFormat(const wchar_t* family, DWRITE_FONT_STRETCH stretch, float em, const std::string& tag) const {
        ComPtr<IDWriteTextFormat> format;
        const std::wstring locale = utf8ToWide(tag == "qps-ploc" ? std::string("en-US") : tag);
        dwrite->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                 stretch, std::max(em, 4.0f), locale.c_str(), &format);
        return format;
    }

    // Cap height of the family as a fraction of the em, so Latin capitals sit on the glyph row.
    float capHeightRatio(const wchar_t* family) const {
        ComPtr<IDWriteFontCollection> fonts;
        if (FAILED(dwrite->GetSystemFontCollection(&fonts, FALSE)) || !fonts) return 0.70f;
        UINT32 index = 0; BOOL exists = FALSE;
        if (FAILED(fonts->FindFamilyName(family, &index, &exists)) || !exists) return 0.70f;
        ComPtr<IDWriteFontFamily> fam; ComPtr<IDWriteFont> font;
        if (FAILED(fonts->GetFontFamily(index, &fam)) || !fam) return 0.70f;
        if (FAILED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)) || !font) return 0.70f;
        DWRITE_FONT_METRICS m{};
        font->GetMetrics(&m);
        if (m.designUnitsPerEm == 0 || m.capHeight == 0) return 0.70f;
        return static_cast<float>(m.capHeight) / static_cast<float>(m.designUnitsPerEm);
    }

    // Draws `layout` at `origin` (scaled horizontally by sx about the origin) and copies a w x h block
    // of coverage to the atlas at (dstX, dstY).
    void drawToAtlas(IDWriteTextLayout* layout, D2D1_POINT_2F origin, float sx, uint32_t w, uint32_t h, uint32_t dstX, uint32_t dstY) {
        w = std::min(w, kScratchW); h = std::min(h, kScratchH);
        if (w == 0 || h == 0 || dstX + w > UiTextAtlasLayout::kWidth || dstY + h > UiTextAtlasLayout::kHeight) return;
        target->BeginDraw();
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->PushAxisAlignedClip(D2D1::RectF(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)), D2D1_ANTIALIAS_MODE_ALIASED);
        target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
        target->SetTransform(D2D1::Matrix3x2F::Scale(sx, 1.0f, origin));
        target->DrawTextLayout(origin, layout, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        target->PopAxisAlignedClip();
        if (FAILED(target->EndDraw())) return;

        WICRect rect{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
        ComPtr<IWICBitmapLock> lock;
        if (FAILED(bitmap->Lock(&rect, WICBitmapLockRead, &lock)) || !lock) return;
        UINT stride = 0, size = 0; BYTE* data = nullptr;
        if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data)) || !data) return;
        for (uint32_t y = 0; y < h; ++y) {
            const BYTE* src = data + static_cast<size_t>(y) * stride;
            uint8_t* dst = atlas.data() + static_cast<size_t>(dstY + y) * UiTextAtlasLayout::kWidth + dstX;
            for (uint32_t x = 0; x < w; ++x) dst[x] = src[x * 4u + 3u]; // white premultiplied: alpha is the coverage
        }
    }

    std::wstring labelText(const UiStrings& strings, const UiLabelSpec& spec) const {
        std::wstring text;
        if (const std::wstring* t = strings.find(spec.key)) text = *t;
        else if (const std::wstring* e = strings.findEnglish(spec.key)) text = *e;
        const size_t at = text.find(L"{n}");
        if (at != std::wstring::npos) text.replace(at, 3, std::to_wstring(spec.number));
        return text;
    }

    struct LabelLayout {
        ComPtr<IDWriteTextLayout> layout;
        float em = 0.0f;
        float width = 0.0f;
        float fit = 1.0f;     // total shrink applied (font size x horizontal squeeze)
        float squeeze = 1.0f; // horizontal squeeze alone
        float baseline = 0.0f;
        float capRatio = 0.7f;
    };

    LabelLayout layoutLabel(const UiStrings& strings, const UiLabelSpec& spec, uint32_t uiScale) const {
        LabelLayout out;
        const std::wstring text = labelText(strings, spec);
        if (text.empty()) return out;
        const LanguageFonts fonts = fontsFor(strings.languageTag());
        const float unit = spec.scale * static_cast<float>(uiScale);
        const float boxW = spec.cells * 5.0f * unit;
        out.capRatio = capHeightRatio(fonts.hud);
        // Latin-family capitals match the height of the glyph-table capitals (6 units, a touch under);
        // other scripts size by the em so their taller glyphs stay inside the row.
        const float fullEm = fonts.condensedHud ? (5.7f / out.capRatio) * unit : 7.4f * unit;

        // Width first: the widest face that fits at full size (normal, semi-condensed, condensed).
        // Then prefer a smaller font over a squeezed one, down to 72%; then squeeze, down to 80%.
        const DWRITE_FONT_STRETCH stretches[3] = {DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STRETCH_SEMI_CONDENSED, DWRITE_FONT_STRETCH_CONDENSED};
        const int lastStretch = fonts.condensedHud ? 2 : 0;
        int stretchIndex = 0;
        float em = fullEm;
        for (int pass = 0; pass < 5; ++pass) {
            ComPtr<IDWriteTextFormat> format = makeFormat(fonts.hud, stretches[stretchIndex], em, strings.languageTag());
            if (!format) return out;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if (strings.rightToLeft()) format->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
            ComPtr<IDWriteTextLayout> layout;
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), 8192.0f, 256.0f, &layout)) || !layout) return out;
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            DWRITE_LINE_METRICS lm{}; UINT32 lines = 0;
            layout->GetLineMetrics(&lm, 1, &lines);
            out.layout = layout; out.em = em; out.width = tm.width; out.baseline = lm.baseline;
            if (tm.width <= boxW || em < fullEm) break;
            if (stretchIndex < lastStretch) { ++stretchIndex; continue; }
            em = fullEm * std::max(0.72f, boxW / tm.width);
        }
        out.squeeze = (out.width > boxW) ? std::max(0.80f, boxW / out.width) : 1.0f;
        const bool clipped = out.width * out.squeeze > boxW + 0.5f;
        out.fit = clipped ? 0.0f : (out.em / fullEm) * out.squeeze;
        return out;
    }

    void rasterLabels(const UiStrings& strings, uint32_t uiScale) {
        const uint32_t cellW = UiTextAtlasLayout::kLabelCellW * uiScale;
        const uint32_t cellH = UiTextAtlasLayout::kLabelCellH * uiScale;
        std::memset(atlas.data(), 0, static_cast<size_t>(UiTextAtlasLayout::kWidth) * UiTextAtlasLayout::kCaptionTop);
        for (uint32_t i = 0; i < static_cast<uint32_t>(UiLabel::Count); ++i) {
            const UiLabelSpec& spec = kLabelSpecs[i];
            LabelLayout l = layoutLabel(strings, spec, uiScale);
            if (!l.layout) continue;
            const float unit = spec.scale * static_cast<float>(uiScale);
            const float boxW = std::min(spec.cells * 5.0f * unit, static_cast<float>(cellW));
            const float boxH = std::min(UiTextAtlasLayout::kBoxHeightUnits * unit, static_cast<float>(cellH));
            const float drawnW = l.width * l.squeeze;
            // Under RTL the layout is 8192 wide with the text at its right edge; find the ink's left.
            DWRITE_TEXT_METRICS tm{};
            l.layout->GetMetrics(&tm);
            const float inkLeft = tm.left;
            const float x = (spec.centered ? std::max(0.0f, 0.5f * (boxW - drawnW)) : 0.0f) - inkLeft * l.squeeze;
            // Centre the capital-letter band on the glyph row: 5.5 units below the box top.
            const float capCentre = (UiTextAtlasLayout::kBoxTopUnits + 3.0f) * unit;
            const float y = capCentre - (l.baseline - 0.5f * l.capRatio * l.em);
            const uint32_t w = static_cast<uint32_t>(std::ceil(boxW));
            const uint32_t h = static_cast<uint32_t>(std::ceil(boxH));
            const uint32_t dstX = (i % UiTextAtlasLayout::kLabelCols) * cellW;
            const uint32_t dstY = (i / UiTextAtlasLayout::kLabelCols) * cellH;
            // The squeeze pivots on the draw origin, so pre-divide the ink offset.
            drawToAtlas(l.layout.Get(), D2D1::Point2F(std::round(x), std::round(y)), l.squeeze, w, h, dstX, dstY);
        }
    }

    void rasterCaption(const UiStrings& strings, const std::wstring& text, float em, float maxWidth) {
        captionValid = false; captionW = captionH = 0;
        std::memset(atlas.data() + static_cast<size_t>(UiTextAtlasLayout::kWidth) * UiTextAtlasLayout::kCaptionTop, 0,
                    static_cast<size_t>(UiTextAtlasLayout::kWidth) * UiTextAtlasLayout::kCaptionMaxH);
        if (text.empty()) return;
        const LanguageFonts fonts = fontsFor(strings.languageTag());
        maxWidth = std::clamp(maxWidth, 64.0f, static_cast<float>(kScratchW) - 4.0f);
        em = std::max(em, 9.0f);
        ComPtr<IDWriteTextLayout> layout;
        DWRITE_TEXT_METRICS tm{};
        // Two lines at most, and never taller than the strip: shrink the type until both hold.
        for (int attempt = 0; attempt < 8; ++attempt) {
            ComPtr<IDWriteTextFormat> format = makeFormat(fonts.caption, DWRITE_FONT_STRETCH_NORMAL, em, strings.languageTag());
            if (!format) return;
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            if (strings.rightToLeft()) format->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
            layout.Reset();
            if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.Get(), maxWidth, 4096.0f, &layout)) || !layout) return;
            layout->GetMetrics(&tm);
            if (tm.lineCount <= 2 && tm.height + 2.0f <= static_cast<float>(UiTextAtlasLayout::kCaptionMaxH)) break;
            em *= 0.90f;
        }
        if (!layout) return;
        const uint32_t w = std::min<uint32_t>(static_cast<uint32_t>(std::ceil(tm.width)) + 2u, kScratchW);
        const uint32_t h = std::min<uint32_t>(static_cast<uint32_t>(std::ceil(tm.height)) + 2u, UiTextAtlasLayout::kCaptionMaxH);
        drawToAtlas(layout.Get(), D2D1::Point2F(1.0f - std::floor(tm.left), 1.0f), 1.0f, w, h, 0, UiTextAtlasLayout::kCaptionTop);
        captionW = w; captionH = h; captionValid = true;
    }
};

UiTextLayer::UiTextLayer() : impl_(std::make_unique<Impl>()) {}
UiTextLayer::~UiTextLayer() = default;

bool UiTextLayer::ready() const noexcept { return impl_ && impl_->ok; }

const uint8_t* UiTextLayer::pixels() const noexcept { return impl_->atlas.data(); }

UiTextLayer::Frame UiTextLayer::prepare(const UiStrings& strings, uint32_t uiScale, const std::wstring* caption,
                                        float captionEmPx, float captionMaxWidthPx) {
    Frame frame;
    if (!ready() || !strings.active()) return frame;
    uiScale = std::clamp<uint32_t>(uiScale, 1u, UiTextAtlasLayout::kMaxUiScale);

    if (impl_->labelsGeneration != strings.generation() || impl_->labelsUiScale != uiScale) {
        impl_->rasterLabels(strings, uiScale);
        impl_->labelsGeneration = strings.generation();
        impl_->labelsUiScale = uiScale;
        frame.labelsDirty = true;
    }

    const std::wstring wanted = caption ? *caption : std::wstring();
    // Type size moves in whole pixels, so a resize of a few pixels does not redraw the caption.
    const float em = std::round(captionEmPx);
    const float maxW = std::round(captionMaxWidthPx);
    if (impl_->captionGeneration != strings.generation() || impl_->captionText != wanted ||
        (!wanted.empty() && (impl_->captionEm != em || impl_->captionMaxWidth != maxW))) {
        const bool hadInk = impl_->captionValid;
        impl_->rasterCaption(strings, wanted, em, maxW);
        impl_->captionGeneration = strings.generation();
        impl_->captionText = wanted;
        impl_->captionEm = em;
        impl_->captionMaxWidth = maxW;
        frame.captionDirty = hadInk || impl_->captionValid;
    }
    frame.captionValid = impl_->captionValid;
    frame.captionWidth = impl_->captionW;
    frame.captionHeight = impl_->captionH;
    return frame;
}

float UiTextLayer::labelFit(const UiStrings& strings, UiLabel label, uint32_t uiScale) {
    if (!ready()) return 1.0f;
    const Impl::LabelLayout l = impl_->layoutLabel(strings, uiLabelSpec(label), std::clamp<uint32_t>(uiScale, 1u, UiTextAtlasLayout::kMaxUiScale));
    return l.layout ? l.fit : 0.0f;
}

} // namespace boo
