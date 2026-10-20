#include "glyph.hpp"

#include <cstddef>

namespace smplhud {

namespace {

// One glyph per line. A point is two digits, x then y (x in 0..4, y in 0..6, y
// growing downwards); points are joined into a stroke, a '/' starts the next
// stroke, and anything else (spaces) is only there to be read.
constexpr const char* kTable[] = {
    // Half-width katakana, as the film shows them.
    "00 40/20 26/06 46",       // e
    "00 40 46 06",             // ko
    "00 40 46 06 00",          // ro
    "01 41/03 43/30 16",       // ki
    "11 31/05 45",             // ni
    "05 22 45",                // he
    "00 06 41",                // re
    "00 40 33 06",             // fu
    "40 05",                   // no
    "20 04/30 46",             // ha
    "10 14/30 36 26",          // ri
    "10 16/12 34",             // to
    "01 41/10 12/30 36 16",    // sa
    "00 40 33 06/22 46",       // su
    "00 40 22 04/24 46",       // ma
    "20 06 46/33 36",          // mu
    "40 06/00 46",             // me
    "01 41/03 43/20 26 36",    // mo
    "00 40 43/06 46",          // yu
    "00 40 46 06/03 43",       // yo
    "10 30/03 43 36",          // ra
    "00 11/06 42",             // n
    "00 11/20 31/06 42",       // tsu
    "00 11/02 13/06 42",       // shi
    "10 21/40 05",             // so
    "10 30/02 42/22 26 16",    // te
    "30 05/22 26",             // i
    "20 21/01 41 43 06",       // u
    "01 41/20 26 16/40 02",    // o
    "10 40 45 35/20 05",       // ka
    "20 02/10 40 45 25",       // ku
    "20 03/01 41/31 36",       // ke
    "01 41/20 26 46 43",       // se
    "30 02/10 40 46/14 34",    // ta
    "10 40/01 41/22 26 06",    // chi
    "00 06 46/02 32",          // hi
    "01 41/20 26/03 15/33 45", // ho
    "10 31/12 33/14 35",       // mi

    // Digits.
    "10 30 41 45 36 16 05 01 10", // 0
    "10 20 26/06 46",             // 1
    "00 40 42 06 46",             // 2
    "00 40 23 43 46 06",          // 3
    "00 03 43/30 36",             // 4
    "40 00 03 43 46 06",          // 5
    "00 40 06",                   // 7
    "00 40 46 06 00/03 43",       // 8
    "43 40 00 03 43 46 06",       // 9

    // Signs and letters.
    "20 26/03 43",       // +
    "20 26/01 45/05 41", // *
    "20 21/25 26",       // :
    "40 03 46",          // <
    "00 40 06 46",       // Z
    "40 00 06 46",       // C
    "00 06/40 46/03 43", // H
    "00 23/40 23/23 26", // Y
    "20 03 26 43 20",    // diamond
    "00 40/20 26",       // T
    "00 06 46",          // L
    "20 06 46 20",       // triangle
    "00 06/40 03 46",    // K
};

constexpr int kCount = int(sizeof(kTable) / sizeof(kTable[0]));

std::vector<GlyphStroke> parse(const char* text) {
    std::vector<GlyphStroke> strokes;
    GlyphStroke              stroke;

    const auto finish = [&] {
        if (!stroke.empty())
            strokes.push_back(std::move(stroke));
        stroke.clear();
    };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };

    for (const char* p = text; *p;) {
        if (*p == '/') {
            finish();
            ++p;
        } else if (digit(p[0]) && digit(p[1])) {
            stroke.push_back({p[0] - '0', p[1] - '0'});
            p += 2;
        } else
            ++p;
    }
    finish();
    return strokes;
}

const std::vector<std::vector<GlyphStroke>>& glyphs() {
    static const std::vector<std::vector<GlyphStroke>> all = [] {
        std::vector<std::vector<GlyphStroke>> parsed;
        parsed.reserve(kCount);
        for (const char* text : kTable)
            parsed.push_back(parse(text));
        return parsed;
    }();
    return all;
}

} // namespace

int glyphCount() {
    return kCount;
}

int glyphIndex(long long index) {
    const long long n = kCount;
    return int(((index % n) + n) % n);
}

std::span<const GlyphStroke> glyphStrokes(int index) {
    return glyphs()[std::size_t(glyphIndex(index))];
}

} // namespace smplhud
