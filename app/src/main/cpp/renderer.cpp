#include "renderer.h"

#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <cstdint>
#include <algorithm>
#include <cmath>

#include "font.h"

namespace soccer {
namespace {

constexpr const char* kLogTag = "PocketSoccer";
constexpr float kPi = 3.14159265f;

const char* kVertSrc =
    "attribute vec2 aPos;\n"
    "attribute vec4 aColor;\n"
    "varying vec4 vColor;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vColor = aColor;\n"
    "}\n";

const char* kFragSrc =
    "precision mediump float;\n"
    "varying vec4 vColor;\n"
    "void main() {\n"
    "  gl_FragColor = vColor;\n"
    "}\n";

GLuint CompileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        char log[256];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "shader: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}  // namespace

bool Renderer::Prepare(android_app* app) {
    if (display_ == EGL_NO_DISPLAY) {
        display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display_ == EGL_NO_DISPLAY) return false;
        if (eglInitialize(display_, nullptr, nullptr) != EGL_TRUE) {
            display_ = EGL_NO_DISPLAY;
            return false;
        }

        const EGLint attribs[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_NONE,
        };
        EGLint numConfigs = 0;
        if (eglChooseConfig(display_, attribs, &config_, 1, &numConfigs) != EGL_TRUE ||
            numConfigs < 1) {
            return false;
        }

        const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttribs);
        if (context_ == EGL_NO_CONTEXT) return false;
    }
    return EnsureSurface(app);
}

bool Renderer::EnsureSurface(android_app* app) {
    if (app->window == nullptr) return false;
    if (surface_ != EGL_NO_SURFACE) return true;

    EGLint format = 0;
    eglGetConfigAttrib(display_, config_, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(app->window, 0, 0, format);

    surface_ = eglCreateWindowSurface(display_, config_,
                                      static_cast<EGLNativeWindowType>(app->window),
                                      nullptr);
    if (surface_ == EGL_NO_SURFACE) return false;
    if (eglMakeCurrent(display_, surface_, surface_, context_) != EGL_TRUE) return false;

    width_ = 0;
    height_ = 0;
    eglQuerySurface(display_, surface_, EGL_WIDTH, &width_);
    eglQuerySurface(display_, surface_, EGL_HEIGHT, &height_);
    if (width_ <= 0 || height_ <= 0) {
        ReleaseSurface();
        return false;
    }

    if (!EnsureProgram()) {
        ReleaseSurface();
        return false;
    }
    batch_.reserve(8192 * 6);
    return true;
}

bool Renderer::EnsureProgram() {
    if (program_ != 0) return true;
    GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertSrc);
    GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragSrc);
    if (vs == 0 || fs == 0) {
        if (vs != 0) glDeleteShader(vs);
        if (fs != 0) glDeleteShader(fs);
        return false;
    }
    program_ = glCreateProgram();
    if (program_ == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) return false;
    aPos_ = glGetAttribLocation(program_, "aPos");
    aColor_ = glGetAttribLocation(program_, "aColor");
    glUseProgram(program_);
    return true;
}

void Renderer::ReleaseSurface() {
    if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display_, surface_);
    }
    surface_ = EGL_NO_SURFACE;
}

void Renderer::Shutdown() {
    ReleaseSurface();
    if (vbo_ != 0 && display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
        glDeleteBuffers(1, &vbo_);
        vbo_ = 0;
    }
    if (context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display_, context_);
        context_ = EGL_NO_CONTEXT;
    }
    if (display_ != EGL_NO_DISPLAY) {
        eglTerminate(display_);
        display_ = EGL_NO_DISPLAY;
    }
    program_ = 0;
}

void Renderer::BeginFrame() {
    glClearColor(0.03f, 0.07f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    batch_.clear();
}
void Renderer::SetFieldView() {
    // Fit a 115 x 76 m pitch area with a small margin.
    const float worldW = 115.0f;
    const float worldH = 76.0f;
    const float ppu =
        0.94f * std::min(width_ / worldW, height_ / worldH);
    sx_ = ppu * 2.0f / static_cast<float>(width_);
    sy_ = ppu * 2.0f / static_cast<float>(height_);
    ox_ = 0.0f;
    oy_ = 0.0f;
}

void Renderer::SetPixelView() {
    sx_ = 2.0f / static_cast<float>(width_);
    sy_ = -2.0f / static_cast<float>(height_);
    ox_ = -1.0f;
    oy_ = 1.0f;
}

void Renderer::PushVert(float x, float y, float r, float g, float b, float a) {
    batch_.push_back(x * sx_ + ox_);
    batch_.push_back(y * sy_ + oy_);
    batch_.push_back(r);
    batch_.push_back(g);
    batch_.push_back(b);
    batch_.push_back(a);
}

void Renderer::Rect(float cx, float cy, float w, float h,
                    float r, float g, float b, float a) {
    const float hx = w * 0.5f;
    const float hy = h * 0.5f;
    const float x0 = cx - hx;
    const float x1 = cx + hx;
    const float y0 = cy - hy;
    const float y1 = cy + hy;
    PushVert(x0, y0, r, g, b, a);
    PushVert(x1, y0, r, g, b, a);
    PushVert(x1, y1, r, g, b, a);
    PushVert(x0, y0, r, g, b, a);
    PushVert(x1, y1, r, g, b, a);
    PushVert(x0, y1, r, g, b, a);
}

void Renderer::Circle(float cx, float cy, float radius,
                      float r, float g, float b, float a, int segments) {
    if (segments < 3) segments = 3;
    const float step = 2.0f * kPi / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a0 = i * step;
        const float a1 = (i + 1) * step;
        PushVert(cx, cy, r, g, b, a);
        PushVert(cx + std::cos(a0) * radius, cy + std::sin(a0) * radius,
                 r, g, b, a);
        PushVert(cx + std::cos(a1) * radius, cy + std::sin(a1) * radius,
                 r, g, b, a);
    }
}
void Renderer::Ring(float cx, float cy, float radius, float thickness,
                      float r, float g, float b, float a, int segments) {
    if (segments < 3) segments = 3;
    const float ri = radius - thickness * 0.5f;
    const float ro = radius + thickness * 0.5f;
    const float step = 2.0f * kPi / static_cast<float>(segments);
    for (int i = 0; i < segments; ++i) {
        const float a0 = i * step;
        const float a1 = (i + 1) * step;
        const float i0x = cx + std::cos(a0) * ri;
        const float i0y = cy + std::sin(a0) * ri;
        const float o0x = cx + std::cos(a0) * ro;
        const float o0y = cy + std::sin(a0) * ro;
        const float i1x = cx + std::cos(a1) * ri;
        const float i1y = cy + std::sin(a1) * ri;
        const float o1x = cx + std::cos(a1) * ro;
        const float o1y = cy + std::sin(a1) * ro;
        PushVert(i0x, i0y, r, g, b, a);
        PushVert(o0x, o0y, r, g, b, a);
        PushVert(o1x, o1y, r, g, b, a);
        PushVert(i0x, i0y, r, g, b, a);
        PushVert(o1x, o1y, r, g, b, a);
        PushVert(i1x, i1y, r, g, b, a);
    }
}

void Renderer::Text(float x, float y, const char* text, float pixelSize,
                    float r, float g, float b, float a) {
    if (text == nullptr || pixelSize <= 0.0f) return;
    float px = x;
    const float cell = pixelSize * 1.02f;  // tiny gap between pixels
    for (const char* p = text; *p != '\0'; ++p) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c >= 96 && c < 128) c = static_cast<unsigned char>(c - 32);
        int idx = static_cast<int>(c) - 32;
        if (idx < 0 || idx > 63) idx = 0;
        for (int row = 0; row < 5; ++row) {
            const uint8_t bits = kFont[idx][row];
            for (int col = 0; col < 3; ++col) {
                if ((bits & (1 << (2 - col))) != 0) {
                    Rect(px + (col + 0.5f) * pixelSize,
                         y + (row + 0.5f) * pixelSize, cell, cell,
                         r, g, b, a);
                }
            }
        }
        px += 4.0f * pixelSize;
    }
}

float Renderer::TextWidth(const char* text, float pixelSize) const {
    if (text == nullptr) return 0.0f;
    int n = 0;
    for (const char* p = text; *p != '\0'; ++p) ++n;
    return n > 0 ? (n * 4 - 1) * pixelSize : 0.0f;
}

void Renderer::EndFrame() {
    if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE) return;
    if (!batch_.empty()) {
        if (vbo_ == 0) glGenBuffers(1, &vbo_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(batch_.size() * sizeof(float)),
                     batch_.data(), GL_DYNAMIC_DRAW);
        glVertexAttribPointer(aPos_, 2, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float),
                              reinterpret_cast<const void*>(0));
        const intptr_t colorOff = 2 * static_cast<intptr_t>(sizeof(float));
        glVertexAttribPointer(aColor_, 4, GL_FLOAT, GL_FALSE,
                              6 * sizeof(float),
                              reinterpret_cast<const void*>(colorOff));
        glEnableVertexAttribArray(aPos_);
        glEnableVertexAttribArray(aColor_);
        glDrawArrays(GL_TRIANGLES, 0,
                     static_cast<GLsizei>(batch_.size() / 6));
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    eglSwapBuffers(display_, surface_);
}

}  // namespace soccer


