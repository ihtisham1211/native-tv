#pragma once
#include <string>

namespace nativetv {
// Three variants share one vertex shader and one vertex layout (batch.hpp). Text and icons are
// both Alpha8 masks, so they share Mask; a separate text program would only be needed for
// gamma-corrected glyph blending.
enum class Program { Sdf, Image, Mask };
constexpr int kProgramCount = 3;
// Textures one draw can sample. GLES2 guarantees 8 units; four keeps the selection chain short.
constexpr int kTextureSlots = 4;

// Complete GLSL source for one stage. Compiles as GLSL ES 1.00 and as desktop GLSL 1.10/1.20
// (the macOS development build), so there is no #version line and precision is guarded.
std::string vertexSource();
std::string fragmentSource(Program program);
} // namespace nativetv
