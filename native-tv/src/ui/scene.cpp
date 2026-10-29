#include "ui/scene.hpp"
#include "ui/kit/components.hpp"
#include <algorithm>

namespace nativetv {
namespace {
uint32_t hashBytes(const char *text, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i)
        hash = (hash ^ static_cast<unsigned char>(text[i])) * 16777619u;
    return hash;
}
// A table this large means the strings are unbounded (a clock, a counter), not a working set.
constexpr size_t kStringTableLimit = 4096;
} // namespace

size_t StringTable::find(const char *text, size_t length, uint32_t hash) const {
    size_t mask = slots_.size() - 1;
    for (size_t slot = hash & mask;; slot = (slot + 1) & mask) {
        uint32_t id = slots_[slot];
        if (!id)
            return slot;
        const std::string &known = strings_[id];
        if (known.size() == length && std::equal(text, text + length, known.begin()))
            return slot;
    }
}
StringId StringTable::intern(const char *text, size_t length) {
    if (!length)
        return 0;
    uint32_t hash = hashBytes(text, length);
    size_t slot = find(text, length, hash);
    if (slots_[slot])
        return slots_[slot];
    if ((strings_.size() + 1) * 2 > slots_.size()) {
        slots_.assign(slots_.size() * 2, 0);
        for (size_t id = 1; id < strings_.size(); ++id)
            slots_[find(strings_[id].data(), strings_[id].size(),
                        hashBytes(strings_[id].data(), strings_[id].size()))] =
                static_cast<uint32_t>(id);
        slot = find(text, length, hash);
    }
    strings_.emplace_back(text, length);
    slots_[slot] = static_cast<uint32_t>(strings_.size() - 1);
    return slots_[slot];
}
void StringTable::clear() {
    strings_.resize(1);
    std::fill(slots_.begin(), slots_.end(), 0);
}

void Scene::reset() {
    nodes.clear();
    hits.clear();
    transforms_.clear();
    opacities_.clear();
    clips_.clear();
    if (strings.size() > kStringTableLimit)
        strings.clear();
}
Node &Scene::add(Node::Kind kind, Rect area) {
    nodes.emplace_back();
    Node &node = nodes.back();
    node.kind = kind;
    node.rect = area;
    if (!transforms_.empty())
        node.group = transforms_.back();
    if (!opacities_.empty())
        node.groupOpacity = opacities_.back();
    if (!clips_.empty()) {
        node.clip = clips_.back();
        node.clipped = true;
    }
    return node;
}
Node &Scene::shape(Rect area, Color fill, float radius) {
    Node &node = add(Node::Kind::Rectangle, area);
    node.color = fill;
    node.radius = radius;
    return node;
}
Node &Scene::picture(Rect area, TextureId texture, float radius, bool contain, Color tint) {
    Node &node = add(Node::Kind::Image, area);
    node.texture = texture;
    node.radius = radius;
    node.contain = contain;
    node.color = tint;
    return node;
}
Node &Scene::icon(Rect area, TextureId atlas, Rect uv, Color tint) {
    Node &node = add(Node::Kind::Mask, area);
    node.texture = atlas;
    node.uv = uv;
    node.color = tint;
    return node;
}
Node &Scene::textRun(Rect origin, RunHandle run, Color color) {
    Node &node = add(Node::Kind::Run, origin);
    node.run = run;
    node.color = color;
    return node;
}
// Groups are stored already composed, so a node reads one entry instead of walking the stack.
void Scene::pushTransform(const Transform &transform) {
    transforms_.push_back(transforms_.empty() ? transform : transform.then(transforms_.back()));
}
void Scene::popTransform() {
    if (!transforms_.empty())
        transforms_.pop_back();
}
void Scene::pushOpacity(float opacity) {
    opacities_.push_back(opacities_.empty() ? opacity : opacity * opacities_.back());
}
void Scene::popOpacity() {
    if (!opacities_.empty())
        opacities_.pop_back();
}
void Scene::pushClip(Rect area) {
    if (!clips_.empty()) {
        const Rect &below = clips_.back();
        float x0 = std::max(area.x, below.x), y0 = std::max(area.y, below.y),
              x1 = std::min(area.x + area.w, below.x + below.w),
              y1 = std::min(area.y + area.h, below.y + below.h);
        area = {x0, y0, std::max(0.f, x1 - x0), std::max(0.f, y1 - y0)};
    }
    clips_.push_back(area);
}
void Scene::popClip() {
    if (!clips_.empty())
        clips_.pop_back();
}

void Scene::rect(Rect area, Color color, float radius) {
    Node node;
    node.rect = area;
    node.color = color;
    node.radius = radius;
    nodes.push_back(node);
}
void Scene::text(Rect area, const std::string &value, int size, Color color, int lineHeight,
                 int weight, Node::Align align) {
    if (value.empty())
        return;
    Node node;
    node.kind = Node::Kind::Text;
    node.rect = area;
    node.value = value;
    node.font = size;
    node.lineHeight = lineHeight;
    node.weight = weight;
    node.align = align;
    node.color = color;
    nodes.push_back(node);
}
void Scene::image(Rect area, const std::string &path, float radius, bool contain, Color tint) {
    if (path.empty())
        return;
    Node node;
    node.kind = Node::Kind::Image;
    node.rect = area;
    node.value = path;
    node.radius = radius;
    node.contain = contain;
    node.color = tint;
    nodes.push_back(node);
}
void Scene::scrim(Rect area) {
    image(area, "local:carousel-scrim.svg");
    nodes.back().stretch = true;
}
void Scene::button(Rect area, const std::string &label, const std::string &id, bool selected,
                   bool light) {
    kit::button(*this, area, label, id, selected, kit::Theme{light});
}
} // namespace nativetv
