#include "graphics/text.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <vector>

namespace nativetv {
namespace {
int width(TTF_Font *font, const std::string &value) {
    int w = 0, h = 0;
    if (!value.empty())
        TTF_SizeUTF8(font, value.c_str(), &w, &h);
    return w;
}
size_t previous(const std::string &value, size_t end) {
    if (!end)
        return 0;
    --end;
    while (end && (static_cast<unsigned char>(value[end]) & 0xc0) == 0x80)
        --end;
    return end;
}
std::string ellipsis(TTF_Font *font, std::string line, int maximum) {
    const std::string dots = "…";
    while (!line.empty() && width(font, line + dots) > maximum)
        line.resize(previous(line, line.size()));
    return line + dots;
}
} // namespace
SDL_Surface *rasterText(TTF_Font *font, const Node &node) {
    if (!font || node.value.empty())
        return nullptr;
    int lineHeight =
        node.lineHeight > 0 ? node.lineHeight : static_cast<int>(std::ceil(node.font * 1.35));
    int maximum = std::max(1, static_cast<int>(node.rect.h) / lineHeight);
    int maximumWidth = std::max(1, static_cast<int>(node.rect.w));
    std::vector<std::string> lines;
    std::string line;
    std::istringstream words(node.value.substr(0, 12000));
    std::string word;
    bool more = false;
    while (words >> word) {
        std::string candidate = line.empty() ? word : line + " " + word;
        if (width(font, candidate) <= maximumWidth) {
            line = candidate;
            continue;
        }
        if (!line.empty()) {
            lines.push_back(line);
            line.clear();
        }
        if (static_cast<int>(lines.size()) >= maximum) {
            more = true;
            break;
        }
        while (width(font, word) > maximumWidth) {
            size_t end = word.size();
            while (end && width(font, word.substr(0, end)) > maximumWidth)
                end = previous(word, end);
            if (!end) {
                end = 1;
                while (end < word.size() && (static_cast<unsigned char>(word[end]) & 0xc0) == 0x80)
                    ++end;
            }
            lines.push_back(word.substr(0, end));
            word.erase(0, end);
            if (static_cast<int>(lines.size()) >= maximum) {
                more = true;
                break;
            }
        }
        if (more)
            break;
        line = word;
    }
    if (!line.empty() && static_cast<int>(lines.size()) < maximum)
        lines.push_back(line);
    if (lines.empty())
        return nullptr;
    if (more)
        lines.back() = ellipsis(font, lines.back(), maximumWidth);
    SDL_Surface *target =
        SDL_CreateRGBSurface(0, maximumWidth, static_cast<int>(lines.size()) * lineHeight, 32, 0xff,
                             0xff00, 0xff0000, 0xff000000);
    if (!target)
        return nullptr;
    SDL_FillRect(target, nullptr, 0);
    SDL_Color white = {255, 255, 255, 255};
    for (size_t i = 0; i < lines.size(); ++i) {
        SDL_Surface *glyphs = TTF_RenderUTF8_Blended(font, lines[i].c_str(), white);
        if (!glyphs)
            continue;
        int x = node.align == Node::Align::Center  ? (maximumWidth - glyphs->w) / 2
                : node.align == Node::Align::Right ? maximumWidth - glyphs->w
                                                   : 0;
        SDL_Rect position = {
            x, static_cast<int>(i) * lineHeight + std::max(0, (lineHeight - glyphs->h) / 2),
            glyphs->w, glyphs->h};
        SDL_SetSurfaceBlendMode(glyphs, SDL_BLENDMODE_NONE);
        SDL_BlitSurface(glyphs, nullptr, target, &position);
        SDL_FreeSurface(glyphs);
    }
    return target;
}

namespace {
const uint32_t kEllipsis = 0x2026, kReplacement = 0xfffd;
std::vector<uint32_t> decode(const std::string &text) {
    std::vector<uint32_t> result;
    result.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        int extra = lead < 0x80         ? 0
                    : (lead >> 5) == 6  ? 1
                    : (lead >> 4) == 14 ? 2
                    : (lead >> 3) == 30 ? 3
                                        : -1;
        uint32_t value = extra == 0   ? lead
                         : extra == 1 ? lead & 0x1f
                         : extra == 2 ? lead & 0x0f
                                      : lead & 0x07;
        ++i;
        for (int k = 0; k < extra && extra > 0; ++k, ++i) {
            if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xc0) != 0x80) {
                value = kReplacement;
                break;
            }
            value = (value << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
        }
        result.push_back(extra < 0 || value > 0x10ffff ? kReplacement : value);
    }
    return result;
}
void append(std::string &out, uint32_t value) {
    if (value < 0x80)
        out += static_cast<char>(value);
    else if (value < 0x800) {
        out += static_cast<char>(0xc0 | value >> 6);
        out += static_cast<char>(0x80 | (value & 0x3f));
    } else if (value < 0x10000) {
        out += static_cast<char>(0xe0 | value >> 12);
        out += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (value & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | value >> 18);
        out += static_cast<char>(0x80 | ((value >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((value >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (value & 0x3f));
    }
}
std::string encode(const std::vector<uint32_t> &codepoints) {
    std::string out;
    for (uint32_t value : codepoints)
        append(out, value);
    return out;
}
// Pen position before each glyph (x[i]) and after the last (x[n]), kerning and tracking
// included.
struct Run {
    std::vector<Glyph *> glyphs;
    std::vector<float> x;
};
Run shape(GlyphAtlas &atlas, const std::vector<uint32_t> &codepoints, const TextStyle &style) {
    Run run;
    run.glyphs.reserve(codepoints.size());
    run.x.reserve(codepoints.size() + 1);
    float tracking = style.tracking * static_cast<float>(style.size), pen = 0;
    const Glyph *previous = nullptr;
    for (uint32_t value : codepoints) {
        Glyph &glyph = atlas.glyph(style.size, style.weight, value);
        if (previous)
            pen += atlas.kerning(style.size, style.weight, previous->index, glyph.index);
        run.x.push_back(pen);
        run.glyphs.push_back(&glyph);
        pen += glyph.advance + tracking;
        previous = &glyph;
    }
    run.x.push_back(pen);
    return run;
}
struct Span {
    size_t begin, end;
};
// Greedy word wrap of one paragraph; a word wider than the line breaks between characters.
void wrapParagraph(const Run &run, const std::vector<uint32_t> &text, size_t first, size_t last,
                   float maxWidth, std::vector<Span> &lines) {
    size_t before = lines.size(), start = first;
    while (start < last) {
        if (start > first)
            while (start < last && text[start] == ' ')
                ++start;
        if (start >= last)
            break;
        size_t end = start, position = start;
        while (position < last) {
            size_t wordStart = position, wordEnd;
            while (wordStart < last && text[wordStart] == ' ')
                ++wordStart;
            wordEnd = wordStart;
            while (wordEnd < last && text[wordEnd] != ' ')
                ++wordEnd;
            if (wordEnd == wordStart)
                break;
            if (maxWidth <= 0 || run.x[wordEnd] - run.x[start] <= maxWidth + 0.01f) {
                end = position = wordEnd;
                continue;
            }
            if (end == start) {
                end = wordStart + 1;
                while (end < wordEnd && run.x[end + 1] - run.x[start] <= maxWidth)
                    ++end;
            }
            break;
        }
        if (end == start)
            break;
        lines.push_back({start, end});
        start = end;
    }
    if (lines.size() == before) // blank or only spaces: still a line
        lines.push_back({first, first});
}
// The characters of each line as drawn, "…" included. Fills `truncated` when text was cut.
std::vector<std::vector<uint32_t>> breakLines(GlyphAtlas &atlas, const std::vector<uint32_t> &text,
                                              const TextStyle &style, const TextOptions &options,
                                              bool &truncated) {
    Run run = shape(atlas, text, style);
    std::vector<Span> spans;
    for (size_t first = 0; first <= text.size();) {
        size_t last = first;
        while (last < text.size() && text[last] != '\n')
            ++last;
        wrapParagraph(run, text, first, last, options.maxWidth, spans);
        first = last + 1;
    }
    truncated = options.maxLines > 0 && spans.size() > static_cast<size_t>(options.maxLines);
    if (truncated) {
        spans.resize(static_cast<size_t>(options.maxLines));
        spans.back().end = text.size();
    }
    float ellipsisWidth = atlas.glyph(style.size, style.weight, kEllipsis).advance +
                          style.tracking * static_cast<float>(style.size);
    std::vector<std::vector<uint32_t>> lines;
    for (size_t i = 0; i < spans.size(); ++i) {
        const Span &span = spans[i];
        size_t end = span.end;
        bool cut = truncated && i + 1 == spans.size();
        if (cut && options.ellipsis && options.maxWidth > 0)
            while (end > span.begin &&
                   run.x[end] - run.x[span.begin] + ellipsisWidth > options.maxWidth)
                --end;
        else if (cut && options.maxWidth > 0)
            while (end > span.begin && run.x[end] - run.x[span.begin] > options.maxWidth)
                --end;
        while (cut && end > span.begin && text[end - 1] <= ' ')
            --end;
        lines.emplace_back(text.begin() + static_cast<std::ptrdiff_t>(span.begin),
                           text.begin() + static_cast<std::ptrdiff_t>(end));
        if (cut && options.ellipsis)
            lines.back().push_back(kEllipsis);
    }
    return lines;
}
uint64_t hashOf(const std::string &text, const TextStyle &style, const TextOptions &options) {
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&hash](const void *data, size_t size) {
        const unsigned char *bytes = static_cast<const unsigned char *>(data);
        for (size_t i = 0; i < size; ++i)
            hash = (hash ^ bytes[i]) * 1099511628211ull;
    };
    mix(text.data(), text.size());
    mix(&style.size, sizeof style.size);
    mix(&style.weight, sizeof style.weight);
    mix(&style.tracking, sizeof style.tracking);
    mix(&options.maxWidth, sizeof options.maxWidth);
    mix(&options.maxLines, sizeof options.maxLines);
    mix(&options.lineHeight, sizeof options.lineHeight);
    int flags = static_cast<int>(options.align) | options.ellipsis << 4 | options.snap << 5;
    mix(&flags, sizeof flags);
    return hash;
}
bool sameKey(const TextStyle &a, const TextStyle &b, const TextOptions &c, const TextOptions &d) {
    return a.size == b.size && a.weight == b.weight && a.tracking == b.tracking &&
           c.maxWidth == d.maxWidth && c.maxLines == d.maxLines && c.lineHeight == d.lineHeight &&
           c.align == d.align && c.ellipsis == d.ellipsis && c.snap == d.snap;
}
} // namespace

float measureText(GlyphAtlas &atlas, const std::string &text, const TextStyle &style) {
    return shape(atlas, decode(text), style).x.back();
}
std::string ellipsize(GlyphAtlas &atlas, const std::string &text, const TextStyle &style,
                      float maxWidth) {
    TextOptions options;
    options.maxWidth = maxWidth;
    std::vector<uint32_t> flat = decode(text);
    std::replace(flat.begin(), flat.end(), static_cast<uint32_t>('\n'), static_cast<uint32_t>(' '));
    bool truncated = false;
    auto lines = breakLines(atlas, flat, style, options, truncated);
    return truncated && !lines.empty() ? encode(lines[0]) : text;
}
std::vector<std::string> wrapLines(GlyphAtlas &atlas, const std::string &text,
                                   const TextStyle &style, const TextOptions &options) {
    bool truncated = false;
    std::vector<std::string> result;
    for (const auto &line : breakLines(atlas, decode(text), style, options, truncated))
        result.push_back(encode(line));
    return result;
}
TextLayout layoutText(GlyphAtlas &atlas, const std::string &text, const TextStyle &style,
                      const TextOptions &options) {
    TextLayout layout;
    bool truncated = false;
    auto lines = breakLines(atlas, decode(text), style, options, truncated);
    FontMetrics metrics = atlas.metrics(style.size, style.weight);
    float lineHeight = options.lineHeight > 0 ? options.lineHeight
                                              : static_cast<float>(metrics.naturalLineHeight());
    float halfLeading = (lineHeight - static_cast<float>(metrics.ascent + metrics.descent)) / 2;
    layout.lines = static_cast<int>(lines.size());
    layout.height = lineHeight * static_cast<float>(lines.size());
    layout.truncated = truncated;
    std::vector<Run> runs;
    for (const auto &line : lines) {
        runs.push_back(shape(atlas, line, style));
        layout.width = std::max(layout.width, runs.back().x.back());
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        unsigned generation = atlas.atlas().generation();
        layout.quads.clear();
        for (size_t i = 0; i < runs.size(); ++i) {
            const Run &run = runs[i];
            float shift = 0;
            if (options.maxWidth > 0)
                shift = (options.maxWidth - run.x.back()) *
                        (options.align == TextAlign::Center  ? 0.5f
                         : options.align == TextAlign::Right ? 1.0f
                                                             : 0.0f);
            float baseline = std::round(static_cast<float>(i) * lineHeight + halfLeading +
                                        static_cast<float>(metrics.ascent));
            for (size_t k = 0; k < run.glyphs.size(); ++k) {
                Glyph &glyph = *run.glyphs[k];
                if (!atlas.place(glyph) || glyph.width <= 0)
                    continue;
                float x = shift + run.x[k];
                if (options.snap)
                    x = std::round(x);
                layout.quads.push_back(
                    {x + static_cast<float>(glyph.bearingX),
                     baseline - static_cast<float>(glyph.bearingY), static_cast<float>(glyph.width),
                     static_cast<float>(glyph.height), glyph.u0, glyph.v0, glyph.u1, glyph.v1});
            }
        }
        if (generation == atlas.atlas().generation())
            break;
    }
    return layout;
}
void TextRuns::beginFrame() {
    frame_.clear();
    if (entries_.size() > 2048)
        entries_.clear();
}
void TextRuns::refresh(Entry &entry) const {
    entry.layout = layoutText(atlas_, entry.text, entry.style, entry.options);
    entry.generation = atlas_.atlas().generation();
}
RunHandle TextRuns::layout(const std::string &text, const TextStyle &style,
                           const TextOptions &options) {
    if (text.empty())
        return 0;
    Entry &entry = entries_[hashOf(text, style, options)];
    if (entry.layout.lines == 0 || entry.generation != atlas_.atlas().generation() ||
        entry.text != text || !sameKey(entry.style, style, entry.options, options)) {
        entry.text = text;
        entry.style = style;
        entry.options = options;
        refresh(entry);
    }
    frame_.push_back(&entry);
    return static_cast<RunHandle>(frame_.size());
}
const TextLayout *TextRuns::layoutOf(RunHandle handle) const {
    return handle && handle <= frame_.size() ? &frame_[handle - 1]->layout : nullptr;
}
bool TextRuns::run(RunHandle handle, TextRun &out) const {
    if (!handle || handle > frame_.size())
        return false;
    Entry &entry = *frame_[handle - 1];
    if (entry.generation != atlas_.atlas().generation())
        refresh(entry);
    if (entry.layout.quads.empty())
        return false;
    out.texture = atlas_.atlas().texture();
    out.glyphs = entry.layout.quads.data();
    out.count = static_cast<uint32_t>(entry.layout.quads.size());
    return true;
}
} // namespace nativetv
