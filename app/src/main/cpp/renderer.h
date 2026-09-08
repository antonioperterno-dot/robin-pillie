#pragma once

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <cstdint>
#include <vector>

struct android_app;

namespace soccer {

// Pocket Soccer: minimal GLES2 batched renderer. Everything is emitted as
// triangles with per-vertex color, so lobby, HUD text (3x5 pixel font) and
// the match pitch all draw through one dynamic vertex buffer.
//
// Two views are supported:
//   SetFieldView(): x/y in meters, origin at the pitch center (render pitch).
//   SetPixelView(): x right / y down, origin top-left, units are pixels
//                   (render lobby UI / HUD overlay).
class Renderer {
public:
    // Safe to call repeatedly: performs one-time EGL init, then (re)creates
    // the window surface whenever `app->window` exists and no surface does.
    bool Prepare(android_app* app);
    void Shutdown();

    // Call when APP_CMD_TERM_WINDOW is received.
    void ReleaseSurface();
    bool HasSurface() const { return surface_ != EGL_NO_SURFACE; }

    int Width() const { return width_; }
    int Height() const { return height_; }

    void BeginFrame();
    void SetFieldView();
    void SetPixelView();
    void Rect(float cx, float cy, float w, float h,
              float r, float g, float b, float a = 1.0f);
    void Circle(float cx, float cy, float radius,
                float r, float g, float b, float a = 1.0f, int segments = 28);
    void Ring(float cx, float cy, float radius, float thickness,
              float r, float g, float b, float a = 1.0f, int segments = 40);
    // (x, y) is the top-left of the first glyph; pixelSize is the size of
    // one font pixel in current-view units.
    void Text(float x, float y, const char* text, float pixelSize,
              float r, float g, float b, float a = 1.0f);
    float TextWidth(const char* text, float pixelSize) const;
    void EndFrame();  // upload + draw + swap buffers

private:
    void PushVert(float x, float y, float r, float g, float b, float a);
    bool EnsureSurface(android_app* app);
    bool EnsureProgram();

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;

    GLuint program_ = 0;
    GLuint vbo_ = 0;
    GLint aPos_ = -1;
    GLint aColor_ = -1;

    std::vector<float> batch_;  // interleaved x, y, r, g, b, a
    int width_ = 0;
    int height_ = 0;
    // Current view affine transform: n = p * s + o.
    float sx_ = 1.0f;
    float sy_ = 1.0f;
    float ox_ = 0.0f;
    float oy_ = 0.0f;
};

}  // namespace soccer
