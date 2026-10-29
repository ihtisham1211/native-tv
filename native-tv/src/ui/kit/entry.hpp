#pragma once
#include "ui/kit/widgets.hpp"
#include <string>
#include <vector>

// What a page that asks for input needs beyond the keys: the text field, the step dots, the QR
// tile and the bar of remote-key hints. Same rules as widgets.hpp: state in, nodes and hits out.
namespace nativetv {
namespace kit {

// Fade and lift of one component's entrance (and an optional scale) around the nodes added while
// it lives; the same grouping widgets.cpp gives its components.
class Layer {
  public:
    Layer(Scene &scene, const Enter &enter, const Transform *scale = nullptr);
    ~Layer();
    Layer(const Layer &) = delete;
    Layer &operator=(const Layer &) = delete;

  private:
    Scene &scene_;
    bool fade_, lift_, scale_;
};

struct TextField {
    Rect rest = metrics::setup::field;
    std::string id, value, placeholder;
    size_t cursor = 0;   // byte offset into value, on a character boundary
    bool secret = false; // dots instead of the characters
    float caret = 0;     // 0..1: caret opacity, from the blink
    Enter enter;
};
// 2 px strong outline, 36 px text at 28 px inside; a value wider than the field scrolls so the
// caret stays in view. Hit: `id`.
void textField(Paint paint, const TextField &field);

// Whether the caret is lit: on for the first half of each 1100 ms period, restarting at the last
// edit so it does not blink while someone types.
bool caretLit(double nowMs, double lastEditMs);

struct StepDots {
    Rect origin = {80, 940, 0, 8};
    std::vector<float> on; // per dot, 0..1; the lit dots are accent, the last lit one is a pill
    Enter enter;
};
void stepDots(Paint paint, const StepDots &dots);

struct QrTile {
    Rect rest = metrics::setup::qrTile;
    std::string text;      // what the code says
    float sweep = 0;       // 0..1 position of the scan line, < 0 for none
    float sweepAlpha = 0;
    Enter enter;
};
// White tile with the raised shadow and the code at whole-pixel modules; false when the text
// does not fit at a readable size (the tile is still drawn).
bool qrTile(Paint paint, const QrTile &tile);

struct Hint {
    std::string cap;   // "OK", "Back"; empty draws the four arrows
    std::string label; // "Choose"
};
// Key caps and labels in a row from `origin`.
void hintBar(Paint paint, Rect origin, const std::vector<Hint> &hints, Enter enter = {});

} // namespace kit
} // namespace nativetv
