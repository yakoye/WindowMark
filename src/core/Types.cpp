#include "windowmark/core/Types.h"

#include <cstddef>

namespace windowmark {
namespace {

// Code points that render as nothing. Kept as an explicit list rather than a Unicode
// category lookup: pulling in ICU for this would dwarf the rest of the program, and the
// set that actually shows up in window titles is small and stable.
bool DrawsNothing(char32_t cp) {
    return cp == 0x00AD ||                      // soft hyphen
           cp == 0x061C ||                      // arabic letter mark
           cp == 0x180E ||                      // mongolian vowel separator
           (cp >= 0x200B && cp <= 0x200F) ||    // zero-width space/joiners, LRM, RLM
           (cp >= 0x202A && cp <= 0x202E) ||    // bidi embedding and override
           (cp >= 0x2060 && cp <= 0x2064) ||    // word joiner, invisible operators
           (cp >= 0x2066 && cp <= 0x2069) ||    // bidi isolates
           cp == 0xFEFF ||                      // zero-width no-break space / BOM
           (cp >= 0xFFF9 && cp <= 0xFFFB) ||    // interlinear annotation
           (cp >= 0xE0000 && cp <= 0xE007F);    // tag characters
}

bool IsSpace(char32_t cp) {
    return cp == U' ' || cp == U'\t' || cp == U'\n' || cp == U'\r' || cp == 0x00A0;
}

// Minimal UTF-8 decode. Returns the code point and advances `i`; on a malformed byte it
// yields the byte itself so the sequence is copied through untouched rather than dropped.
char32_t Decode(std::string_view text, std::size_t& i) {
    const auto lead = static_cast<unsigned char>(text[i]);
    std::size_t extra = 0;
    char32_t cp = lead;
    if (lead >= 0xF0) { extra = 3; cp = lead & 0x07U; }
    else if (lead >= 0xE0) { extra = 2; cp = lead & 0x0FU; }
    else if (lead >= 0xC0) { extra = 1; cp = lead & 0x1FU; }
    else if (lead >= 0x80) { ++i; return lead; }   // stray continuation byte

    if (i + extra >= text.size()) { ++i; return lead; }
    for (std::size_t k = 1; k <= extra; ++k) {
        const auto byte = static_cast<unsigned char>(text[i + k]);
        if ((byte & 0xC0U) != 0x80U) { ++i; return lead; }
        cp = (cp << 6) | (byte & 0x3FU);
    }
    i += extra + 1;
    return cp;
}

} // namespace

std::string SanitizeTitle(std::string_view title) {
    std::string out;
    out.reserve(title.size());

    for (std::size_t i = 0; i < title.size();) {
        const std::size_t start = i;
        const char32_t cp = Decode(title, i);
        if (DrawsNothing(cp)) continue;
        // Leading whitespace would otherwise survive the strip and take the place of a
        // visible character in the collapsed label, which is the same blank tab again.
        if (out.empty() && IsSpace(cp)) continue;
        out.append(title, start, i - start);
    }

    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' ||
                            out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

} // namespace windowmark
