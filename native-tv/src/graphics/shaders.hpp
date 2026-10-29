#pragma once
#include <string>

namespace nativetv {
// The variants share one vertex shader and one vertex layout (batch.hpp). Text and icons are
// both Alpha8 masks, so they share Mask; a separate text program would only be needed for
// gamma-corrected glyph blending. Flat and FlatImage are Sdf and Image without the distance
// field, for the parts of a shape the field is certain to cover (batch.hpp): the field costs the
// B3's GPU about eight times a plain fill, and a Home frame has several screens' worth of them.
enum class Program { Sdf, Image, Mask, Flat, FlatImage };
constexpr int kProgramCount = 5;
// Textures one draw can sample. GLES2 guarantees 8 units; four keeps the selection chain short.
constexpr int kTextureSlots = 4;

// Complete GLSL source for one stage. Compiles as GLSL ES 1.00 and as desktop GLSL 1.10/1.20
// (the macOS development build), so there is no #version line and precision is guarded.
std::string vertexSource();
// `variant` bits: kSingleTexture is real; 1 (no distance field) is measurement only
// (DebugSkip::shaderVariant).
// A draw that samples one texture has no use for the slot selection, which on the B3's GPU costs
// a texture read per slot on every pixel (a full-screen picture took 3.4 ms instead of 1).
constexpr unsigned kSingleTexture = 16;
std::string fragmentSource(Program program, unsigned variant = 0);
} // namespace nativetv
