#pragma once

#include "boo/results_dialog.hpp"
#include "boo/results_text.hpp"
#include <array>
#include <string>
#include <vector>

namespace boo {
ResultText resultsReasonText(BenchmarkReason reason);
ResultText resultsModeText(BenchmarkMode mode);

struct ResultCardCell {
    std::wstring text;
    bool numeric = false; // Number, version, count, or canvas dimensions.
};
struct ResultCardRow {
    std::wstring label;
    std::array<ResultCardCell, 2> values;
};
struct ResultCard {
    std::wstring title, subtitle;
    std::array<std::wstring, 3> headings;
    std::vector<ResultCardRow> rows;
    std::wstring note;
    bool rtl = false;
};
// Native cells, PNG glyph layout and the UTF-8 table all read these immutable
// fields. No adapter descriptions, filesystem paths or free-form diagnostics.
ResultCard makeResultsCard(const ResultsDialogContext&, const BenchmarkSnapshot&);
std::wstring resultsCardText(const ResultCard&);
} // namespace boo
