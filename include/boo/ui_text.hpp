#pragma once

// Localized on-screen text.
//
// English is drawn by the first-party shader glyph tables and never touches this code, so English
// frames are unchanged. For any other language the catalog in assets/strings supplies the words and
// UiTextLayer draws them with Windows system fonts (DirectWrite: shaping, right-to-left, font
// fallback) into one 8-bit coverage atlas that the present/HUD shader samples one texel per pixel.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace boo {

// Slot ids are shared with assets/shaders/m1_taipei_slice.hlsl (UI_LBL_*). Keep both lists in step.
enum class UiLabel : uint32_t {
    Prev, Play, Pause, Next, ModeFilm, ModeFree, Hardware, ColSize, ColRate,
    StateLive, StateSub, StateOff, StateFault, Bench, CamFree, CamFilm, Lamp, Wipe, Marbles,
    Sun, Azimuth, Mute, Music, Reset, NextScene, Solo, All,
    Act1, Act2, Act3, Act4, Act5, Act6, Act7,
    SceneBathroom, SceneFaucet, SceneLeaves, SceneOrchid, SceneEnamel, SceneSanctuary, SceneCustom,
    AovAlbedo, AovNormal, AovRoughMetal, AovDirect, AovIndirect, AovReflect, AovRayHeat,
    LiveCamera, OstSynth, OstCustom, StateScript, StateWarming, StateIdle,
    PathTracing, Unavailable, Low, Medium, High, Quality, Balanced, Performance,
    RealTimeGraphics, Automatic, Enabled, Boost, RayReconstruction, FrameGeneration, RenderFps, DisplayFps, DlssUpscaling,
    PlayAgain, RunBenchmark,
    Count
};

struct UiLabelSpec {
    const char* key;   // catalog key
    float cells;       // width of the label's box, in glyph cells (5 units each) at `scale`
    float scale;       // glyph scale in HUD units; the shader passes scale * uiScale
    bool centered;     // centred in its box (buttons) or left-aligned (headings)
    uint32_t number;   // replaces {n} in the catalog text; 0 = none
};
const UiLabelSpec& uiLabelSpec(UiLabel label) noexcept;

// Atlas layout, in pixels. The label grid scales with uiScale (1..3); the atlas itself is fixed size.
struct UiTextAtlasLayout {
    static constexpr uint32_t kWidth = 3072;
    static constexpr uint32_t kLabelCols = 4;
    static constexpr uint32_t kLabelRows = 19;
    static constexpr uint32_t kLabelCellW = 160;   // x uiScale
    static constexpr uint32_t kLabelCellH = 20;    // x uiScale
    static constexpr uint32_t kMaxUiScale = 3;
    static constexpr uint32_t kCaptionTop = kLabelRows * kLabelCellH * kMaxUiScale;
    static constexpr uint32_t kCaptionMaxH = 224;
    static constexpr uint32_t kHeight = kCaptionTop + kCaptionMaxH;
    static_assert(static_cast<uint32_t>(UiLabel::Count) <= kLabelCols * kLabelRows,
                  "every label needs a cell in the atlas: add a row (and move the shader's UI_CAPTION_TOP)");
    // Label text box relative to the glyph origin the shader uses: starts 2.5 units above it, 11 units tall.
    static constexpr float kBoxTopUnits = 2.5f;
    static constexpr float kBoxHeightUnits = 11.0f;
};

class UiStrings {
public:
    static UiStrings& instance();

    // Loads <dir>/en-US.json and <dir>/<tag>.json. English, or a tag with no file, leaves the catalog
    // inactive (returns true: English is always valid). "qps-ploc" builds a pseudo-language from the
    // English text (accented and about a third longer) for layout testing.
    bool load(const std::filesystem::path& dir, std::string_view languageTag, std::string* error = nullptr);
    void clear();

    bool active() const noexcept { return active_; }
    const std::string& languageTag() const noexcept { return tag_; }
    bool rightToLeft() const noexcept { return rtl_; }
    uint64_t generation() const noexcept { return generation_; }
    size_t size() const noexcept { return strings_.size(); }

    const std::wstring* find(std::string_view key) const;              // translated text, or nullptr
    std::wstring textOr(std::string_view key, std::wstring_view fallback) const;
    const std::wstring* findEnglish(std::string_view key) const;       // source text, or nullptr
    const std::wstring* findCaption(std::string_view englishText) const; // translated caption, or nullptr
    std::string captionKey(std::string_view englishText) const;        // "" when the text is not in the catalog

private:
    bool active_ = false;
    bool rtl_ = false;
    uint64_t generation_ = 0;
    std::string tag_ = "en-US";
    std::unordered_map<std::string, std::wstring> strings_;
    std::unordered_map<std::string, std::wstring> english_;
    std::unordered_map<std::string, std::string> captionKeys_; // English caption text -> key
};

// Language tags that have a catalog file in `dir` (always includes en-US when en-US.json exists).
std::vector<std::string> uiAvailableLanguages(const std::filesystem::path& dir);
// Best catalog match for a preference list such as {"pt-PT", "en-GB"}; "en-US" when nothing matches.
std::string uiMatchLanguage(const std::vector<std::string>& preferred, const std::vector<std::string>& available);
// The signed-in user's Windows display languages, most preferred first.
std::vector<std::string> uiWindowsPreferredLanguages();

class UiTextLayer {
public:
    UiTextLayer();
    ~UiTextLayer();
    UiTextLayer(const UiTextLayer&) = delete;
    UiTextLayer& operator=(const UiTextLayer&) = delete;

    // False when DirectWrite/Direct2D could not start; the caller then keeps the English glyph path.
    bool ready() const noexcept;

    struct Frame {
        bool labelsDirty = false;   // rows [0, kCaptionTop) changed
        bool captionDirty = false;  // rows [kCaptionTop, kHeight) changed
        bool captionValid = false;
        uint32_t captionWidth = 0;  // caption block occupies (0, kCaptionTop) .. (+width, +height)
        uint32_t captionHeight = 0;
    };
    // uiScale: clamp(floor(targetHeight / 540), 1, 3), as in the shader. caption: nullptr for none.
    Frame prepare(const UiStrings& strings, uint32_t uiScale, const std::wstring* caption, float captionEmPx, float captionMaxWidthPx);

    const uint8_t* pixels() const noexcept; // kWidth * kHeight, one byte of coverage per texel

    // How much a label had to shrink to fit its box at this uiScale: 1 = full size. Below
    // kMinLabelFit the text is clipped, which the catalog test treats as a failure.
    static constexpr float kMinLabelFit = 0.58f;
    float labelFit(const UiStrings& strings, UiLabel label, uint32_t uiScale);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
