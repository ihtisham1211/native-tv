#include "graphics/shaders.hpp"

namespace nativetv {
namespace {
// highp where the GPU has it: with 16-bit fragment floats, coordinates above 1024 lose whole
// pixels and sub-pixel animation steps.
const char *kFragmentPrecision =
    "#ifdef GL_ES\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "#endif\n";

const char *kVertex =
    "#ifdef GL_ES\n"
    "precision highp float;\n"
    "#endif\n"
    "attribute vec4 a_pos;\n"   // x, y, u, v
    "attribute vec4 a_shape;\n" // offset from the shape centre, half extent
    "attribute vec4 a_radii;\n" // top-left, top-right, bottom-right, bottom-left
    "attribute vec4 a_band;\n"  // outer, inner, edge softness, texture slot
    "attribute vec4 a_color;\n"
    "uniform vec4 u_proj;\n" // canvas to clip: scale x, scale y, offset x, offset y
    "varying vec2 v_uv;\n"
    "varying vec4 v_shape;\n"
    "varying vec4 v_radii;\n"
    "varying vec4 v_band;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "  gl_Position = vec4(a_pos.x * u_proj.x + u_proj.z, a_pos.y * u_proj.y + u_proj.w, 0.0, 1.0);\n"
    "  v_uv = a_pos.zw;\n"
    "  v_shape = a_shape;\n"
    "  v_radii = a_radii;\n"
    "  v_band = a_band;\n"
    "  v_color = a_color;\n"
    "}\n";

const char *kVaryings =
    "varying vec2 v_uv;\n"
    "varying vec4 v_shape;\n"
    "varying vec4 v_radii;\n"
    "varying vec4 v_band;\n"
    "varying vec4 v_color;\n";

// Fill, ring and shadow are one formula: the coverage between the edge pushed out by `outer` and
// the edge pulled in by `inner`, each blurred over +-softness. A fill has inner far beyond the
// shape, a ring a finite inner and outer, a shadow a large softness.
const char *kCoverage =
    "float edge(float x, float s) { return 1.0 - smoothstep(-s, s, x); }\n"
    "float coverage() {\n"
    "  vec2 p = v_shape.xy;\n"
    "  vec2 h = v_shape.zw;\n"
    "  float r = p.x < 0.0 ? (p.y < 0.0 ? v_radii.x : v_radii.w)\n"
    "                      : (p.y < 0.0 ? v_radii.y : v_radii.z);\n"
    "  vec2 q = abs(p) - (h - vec2(r));\n"
    "  float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;\n"
    "  float s = v_band.z;\n"
    "  return clamp(edge(d - v_band.x, s) - edge(d + v_band.y, s), 0.0, 1.0);\n"
    "}\n";

const char *kFetch =
    "uniform sampler2D u_tex0;\n"
    "uniform sampler2D u_tex1;\n"
    "uniform sampler2D u_tex2;\n"
    "uniform sampler2D u_tex3;\n"
    // The slot is constant across a triangle, so the branches are coherent.
    "vec4 fetch(float slot, vec2 uv) {\n"
    "  if (slot < 0.5) return texture2D(u_tex0, uv);\n"
    "  if (slot < 1.5) return texture2D(u_tex1, uv);\n"
    "  if (slot < 2.5) return texture2D(u_tex2, uv);\n"
    "  return texture2D(u_tex3, uv);\n"
    "}\n";
} // namespace

std::string vertexSource() {
    return kVertex;
}
std::string fragmentSource(Program program) {
    std::string source = kFragmentPrecision;
    source += kVaryings;
    switch (program) {
    case Program::Sdf:
        source += kCoverage;
        source += "void main() { gl_FragColor = vec4(v_color.rgb, v_color.a * coverage()); }\n";
        break;
    case Program::Image:
        source += kCoverage;
        source += kFetch;
        source += "void main() {\n"
                  "  vec4 t = fetch(v_band.w, v_uv);\n"
                  "  gl_FragColor = vec4(t.rgb * v_color.rgb, t.a * v_color.a * coverage());\n"
                  "}\n";
        break;
    case Program::Mask:
        source += kFetch;
        source += "void main() { gl_FragColor = vec4(v_color.rgb, v_color.a * fetch(v_band.w, v_uv).a); }\n";
        break;
    }
    return source;
}
} // namespace nativetv
