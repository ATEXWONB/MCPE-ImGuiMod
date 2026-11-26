#include "BlurBg.h"
#include "imgui/imgui.h"
#include <GLES3/gl3.h>
#include <stdio.h>
#include <math.h>

namespace BlurBg {

static GLuint s_src = 0, s_texA = 0, s_texB = 0;
static GLuint s_fboA = 0, s_fboB = 0;
static GLuint s_prog = 0, s_vao = 0, s_vbo = 0;
static int    s_w = 0, s_h = 0, s_srcW = 0, s_srcH = 0;
static int    s_ok = 0, s_tried = 0, s_drawn = 0, s_fail = 0;
static int    s_haveSrc = 0;
static GLint  u_tex = -1, u_texel = -1, u_dir = -1, u_rad = -1;

#define BLOG(...) do { FILE* _b = fopen("/data/data/com.mojang.minecraftpe/files/mcstage.log", "a"); if (_b) { fprintf(_b, "[blur] "); fprintf(_b, __VA_ARGS__); fputc(10, _b); fflush(_b); fclose(_b); } } while (0)

static const char *VS =
    "#version 300 es\n"
    "precision highp float;\n"
    "layout(location=0) in vec2 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "out vec2 v_uv;\n"
    "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *FS =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv; out vec4 o;\n"
    "uniform sampler2D u_tex; uniform vec2 u_texel; uniform vec2 u_dir; uniform float u_rad;\n"
    "void main(){\n"
    "  vec4 sum = vec4(0.0); float wsum = 0.0;\n"
    "  for (int i = -8; i <= 8; i++) {\n"
    "    float fi = float(i);\n"
    "    float w = exp(-(fi*fi) / 18.0);\n"
    "    sum += texture(u_tex, v_uv + u_dir * u_texel * (fi * u_rad)) * w;\n"
    "    wsum += w;\n"
    "  }\n"
    "  o = sum / wsum;\n"
    "}\n";

static GLuint mk_shader(GLenum t, const char *src) {
    GLuint s = glCreateShader(t);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char lg[512]; lg[0] = 0; glGetShaderInfoLog(s, sizeof lg, NULL, lg); BLOG("shader fail: %s", lg); glDeleteShader(s); return 0; }
    return s;
}

static int init_program(void) {
    GLuint vs = mk_shader(GL_VERTEX_SHADER, VS);
    GLuint fs = mk_shader(GL_FRAGMENT_SHADER, FS);
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return 0; }
    s_prog = glCreateProgram();
    glAttachShader(s_prog, vs);
    glAttachShader(s_prog, fs);
    glLinkProgram(s_prog);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(s_prog, GL_LINK_STATUS, &ok);
    if (!ok) { BLOG("link fail"); glDeleteProgram(s_prog); s_prog = 0; return 0; }
    u_tex = glGetUniformLocation(s_prog, "u_tex");
    u_texel = glGetUniformLocation(s_prog, "u_texel");
    u_dir = glGetUniformLocation(s_prog, "u_dir");
    u_rad = glGetUniformLocation(s_prog, "u_rad");

    static const float quad[] = {
        -1,-1, 0,0,   1,-1, 1,0,  -1,1, 0,1,
         1,-1, 1,0,   1, 1, 1,1,  -1,1, 0,1,
    };
    glGenVertexArrays(1, &s_vao);
    glGenBuffers(1, &s_vbo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void *)8);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    BLOG("program ready");
    return 1;
}

static void mk_rt(GLuint *tex, GLuint *fbo, int w, int h) {
    if (*tex) glDeleteTextures(1, tex);
    if (*fbo) glDeleteFramebuffers(1, fbo);
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void shutdown(void) {
    if (s_src) glDeleteTextures(1, &s_src);
    if (s_texA) glDeleteTextures(1, &s_texA);
    if (s_texB) glDeleteTextures(1, &s_texB);
    if (s_fboA) glDeleteFramebuffers(1, &s_fboA);
    if (s_fboB) glDeleteFramebuffers(1, &s_fboB);
    if (s_prog) glDeleteProgram(s_prog);
    if (s_vao) glDeleteVertexArrays(1, &s_vao);
    if (s_vbo) glDeleteBuffers(1, &s_vbo);
    s_src = s_texA = s_texB = s_fboA = s_fboB = s_prog = s_vao = s_vbo = 0;
    s_ok = s_tried = 0; s_w = s_h = s_srcW = s_srcH = 0;
}

void onContextReset(void)
{
    s_src = s_texA = s_texB = s_fboA = s_fboB = s_prog = s_vao = s_vbo = 0;
    s_ok = 0; s_tried = 0; s_w = s_h = s_srcW = s_srcH = 0;
    s_haveSrc = 0; s_drawn = 0; s_fail = 0;
}

static void clamp_rect(float dispW, float dispH, float x, float y, float w, float h,
                       int& ix, int& iy, int& iw, int& ih)
{
    iw = (int)w; ih = (int)h; ix = (int)x; iy = (int)y;
    if (iw > (int)dispW) iw = (int)dispW;
    if (ih > (int)dispH) ih = (int)dispH;
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix + iw > (int)dispW) iw = (int)dispW - ix;
    if (iy + ih > (int)dispH) ih = (int)dispH - iy;
}

void capture(float dispW, float dispH, float x, float y, float w, float h) {
    s_haveSrc = 0;
    if (w < 24.f || h < 24.f || dispW < 8.f || dispH < 8.f) return;
    int ix, iy, iw, ih;
    clamp_rect(dispW, dispH, x, y, w, h, ix, iy, iw, ih);
    if (iw < 24 || ih < 24) return;
    if (iw != s_srcW || ih != s_srcH) {
        if (s_src) glDeleteTextures(1, &s_src);
        glGenTextures(1, &s_src);
        glBindTexture(GL_TEXTURE_2D, s_src);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_srcW = iw; s_srcH = ih;
    }
    GLint prevTex = 0; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glBindTexture(GL_TEXTURE_2D, s_src);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ix, (GLint)(dispH - (float)(iy + ih)), iw, ih);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    s_haveSrc = (glGetError() == GL_NO_ERROR) ? 1 : 0;
}

void draw(float dispW, float dispH, float x, float y, float w, float h,
          float cx, float cy, float cw, float ch,
          float rounding, int strength, bool enabled) {
    if (!enabled || w < 24.f || h < 24.f || dispW < 8.f || dispH < 8.f) return;
    if (!s_tried) { s_tried = 1; s_ok = init_program(); }
    if (!s_ok) return;

    int iw = (int)w, ih = (int)h;
    if (iw > (int)dispW) iw = (int)dispW;
    if (ih > (int)dispH) ih = (int)dispH;
    int ix = (int)x, iy = (int)y;
    if (ix < 0) ix = 0;
    if (iy < 0) iy = 0;
    if (ix + iw > (int)dispW) iw = (int)dispW - ix;
    if (iy + ih > (int)dispH) ih = (int)dispH - iy;
    if (iw < 24 || ih < 24) return;

    int bw = iw / 3; if (bw < 2) bw = 2;
    int bh = ih / 3; if (bh < 2) bh = 2;
    if (bw != s_w || bh != s_h) { mk_rt(&s_texA, &s_fboA, bw, bh); mk_rt(&s_texB, &s_fboB, bw, bh); s_w = bw; s_h = bh; }
    if (iw != s_srcW || ih != s_srcH) {
        if (s_src) glDeleteTextures(1, &s_src);
        glGenTextures(1, &s_src);
        glBindTexture(GL_TEXTURE_2D, s_src);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, iw, ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        s_srcW = iw; s_srcH = ih;
    }

    GLint prevFbo = 0, prevVp[4] = {0,0,0,0}, prevTex = 0, prevProg = 0, prevVao = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, prevVp);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProg);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    GLboolean bBlend = glIsEnabled(GL_BLEND), bDepth = glIsEnabled(GL_DEPTH_TEST),
              bSciss = glIsEnabled(GL_SCISSOR_TEST), bCull = glIsEnabled(GL_CULL_FACE);

    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_SCISSOR_TEST); glDisable(GL_CULL_FACE);

    float rad = 0.f; /* 中间模糊半径 */
    GLenum err = GL_NO_ERROR;
    if (!s_haveSrc) {
        glBindTexture(GL_TEXTURE_2D, s_src);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, ix, (GLint)(dispH - (float)(iy + ih)), iw, ih);
        err = glGetError();
    }
    if (err != GL_NO_ERROR) {
        if (s_fail < 4) { s_fail++; BLOG("copy err=%#x fbo=%d %dx%d", (unsigned)err, prevFbo, iw, ih); }
        goto restore;
    }

    rad = (float)strength * 0.12f + 0.5f;
    if (rad < 0.5f) rad = 0.5f;
    if (rad > 8.0f) rad = 8.0f;
    glBindVertexArray(s_vao);
    glUseProgram(s_prog);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(u_tex, 0);
    glUniform2f(u_texel, 1.0f / (float)bw, 1.0f / (float)bh);
    glUniform1f(u_rad, rad);

    glBindFramebuffer(GL_FRAMEBUFFER, s_fboA);
    glViewport(0, 0, bw, bh);
    glUniform2f(u_dir, 1.f, 0.f);
    glBindTexture(GL_TEXTURE_2D, s_src);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindFramebuffer(GL_FRAMEBUFFER, s_fboB);
    glUniform2f(u_dir, 0.f, 1.f);
    glBindTexture(GL_TEXTURE_2D, s_texA);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (s_drawn < 3) { s_drawn++; BLOG("draw ok %dx%d rad=%.2f", iw, ih, rad); }

restore:
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(prevVp[0], prevVp[1], prevVp[2], prevVp[3]);
    glBindVertexArray((GLuint)prevVao);
    glUseProgram((GLuint)prevProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    if (bBlend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (bDepth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if (bSciss) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (bCull)  glEnable(GL_CULL_FACE);  else glDisable(GL_CULL_FACE);

    if (err != GL_NO_ERROR) return;

    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    if (!dl) return;
    dl->PushClipRect(ImVec2(cx, cy), ImVec2(cx + cw, cy + ch), true);
    dl->AddImageRounded((ImTextureID)(size_t)s_texB, ImVec2((float)ix, (float)iy),
                        ImVec2((float)(ix + iw), (float)(iy + ih)),
                        ImVec2(0.f, 1.f), ImVec2(1.f, 0.f),
                        IM_COL32(255, 255, 255, 255), rounding);
    dl->PopClipRect();
    s_haveSrc = 0;
}

} // namespace BlurBg
