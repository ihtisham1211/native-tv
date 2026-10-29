#include "platform/probe.hpp"

#include "core/json.hpp"
#include "platform/gl_window.hpp"
#ifdef __APPLE__
#include <SDL_opengl.h>
#else
#include <GLES2/gl2.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace nativetv {
namespace {
// The hero art is 1600x900 and the glyph atlas 2048x1024.
constexpr GLint kMinTextureSize = 2048;
constexpr int kExpected[4] = {51, 153, 204, 255};
constexpr int kReadbackTolerance = 3;

struct ProbeFailure {
    int code;
    const char *stage;
    std::string message;
};

std::string glText(GLenum name) {
    const GLubyte *text = glGetString(name);
    return text ? reinterpret_cast<const char *>(text) : "";
}
void requireNoError(const char *step) {
    GLenum error = glGetError();
    if (error != GL_NO_ERROR) {
        char text[64];
        std::snprintf(text, sizeof(text), "%s: GL error 0x%04x", step, error);
        throw ProbeFailure{kProbeGlError, "gl", text};
    }
}
GLuint compile(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = "";
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        throw ProbeFailure{kProbeGlError, "gl", std::string("shader: ") + log};
    }
    return shader;
}
bool highpFragment() {
#ifdef __APPLE__
    return true;
#else
    GLint range[2] = {0, 0}, precision = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
    return precision > 0;
#endif
}

void probeContext(const GlWindow &window, Json &report) {
    const char *fault = std::getenv("NATIVETV_PROBE_FAULT");
    const bool injectGlError = fault && !std::strcmp(fault, "gl-error");
    const bool skipDraw = fault && !std::strcmp(fault, "readback");

    GLint maxTexture = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexture);
    report.set("renderer", Json::text(glText(GL_RENDERER)));
    report.set("version", Json::text(glText(GL_VERSION)));
    report.set("maxTextureSize", Json::numeric(maxTexture));
    report.set("highpFragment", Json::flag(highpFragment()));
    report.set("width", Json::numeric(window.width()));
    report.set("height", Json::numeric(window.height()));
    requireNoError("query");
    if (maxTexture < kMinTextureSize)
        throw ProbeFailure{kProbeGlError, "gl",
                           "max texture size " + std::to_string(maxTexture) + " is below " +
                               std::to_string(kMinTextureSize)};

    const char *vertex = "attribute vec2 position;void main(){gl_Position=vec4(position,0.0,1.0);}";
    const char *fragment =
#ifndef __APPLE__
        "precision mediump float;"
#endif
        "uniform vec4 color;void main(){gl_FragColor=color;}";
    GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vertex));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment));
    glBindAttribLocation(program, 0, "position");
    glLinkProgram(program);
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked)
        throw ProbeFailure{kProbeGlError, "gl", "program did not link"};
    glUseProgram(program);
    glUniform4f(glGetUniformLocation(program, "color"), kExpected[0] / 255.0f,
                kExpected[1] / 255.0f, kExpected[2] / 255.0f, 1.0f);

    // One triangle larger than the screen, from a buffer as the client does.
    const GLfloat triangle[] = {-1, -1, 3, -1, -1, 3};
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    glViewport(0, 0, window.width(), window.height());
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!skipDraw)
        glDrawArrays(GL_TRIANGLES, 0, 3);
    if (injectGlError)
        glActiveTexture(0);
    unsigned char pixel[4] = {0, 0, 0, 0};
    glReadPixels(window.width() / 2, window.height() / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    requireNoError("draw");

    Json values = Json::array();
    for (unsigned char value : pixel)
        values.add(Json::numeric(value));
    report.set("pixel", values);
    for (int i = 0; i < 4; ++i)
        if (std::abs(pixel[i] - kExpected[i]) > kReadbackTolerance)
            throw ProbeFailure{kProbeReadback, "readback", "the pixel read back is not the colour drawn"};

    glDeleteBuffers(1, &buffer);
    glDeleteProgram(program);
}
} // namespace

int runProbe(bool windowed) {
    Json report = Json::object();
    report.set("ok", Json::flag(false)); // first in the line; set for real below
    int code = kProbeOk;
    try {
        GlWindow window(windowed);
        probeContext(window, report);
    } catch (const WindowError &error) {
        code = error.stage == WindowStage::Display ? kProbeNoDisplay : kProbeNoContext;
        report.set("stage",
                   Json::text(error.stage == WindowStage::Display ? "display" : "context"));
        report.set("error", Json::text(error.what()));
    } catch (const ProbeFailure &failure) {
        code = failure.code;
        report.set("stage", Json::text(failure.stage));
        report.set("error", Json::text(failure.message));
    }
    report.set("ok", Json::flag(code == kProbeOk));
    SDL_Quit();
    std::puts(report.dump(false).c_str());
    return code;
}

} // namespace nativetv
