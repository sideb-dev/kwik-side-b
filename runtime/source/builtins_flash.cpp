#include "gml_runtime.h"
#include "engine_internal.h"
#include "render.h"

#include <glad/glad.h>

#include "rvm_flash/swf.h"
#include "rvm_flash/player.h"
#include "rvm_flash/renderer_gl.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace gml {

static double A(const Value* args, int argc, int i, double dflt = 0.0) {
    return i < argc ? (double)args[i] : dflt;
}
static std::string S(const Value* args, int argc, int i) {
    return i < argc ? (std::string)args[i] : std::string();
}

namespace {

struct FlashInstance {
    bool alive = false;
    swf::Movie movie;
    swf::Player player;
    swf::RendererGL renderer;
    GLuint fbo = 0, color_tex = 0, depth_stencil_rb = 0;
    int fb_w = 0, fb_h = 0;
    double frame_accum = 0.0;
};

std::vector<std::unique_ptr<FlashInstance>> g_flash;

FlashInstance* flash_of(const Value& v) {
    int i = (int)(double)v;
    if (i < 0 || (size_t)i >= g_flash.size() || !g_flash[i] || !g_flash[i]->alive) return nullptr;
    return g_flash[i].get();
}

void destroy_gl(FlashInstance& f) {
    if (f.fbo || f.color_tex || f.depth_stencil_rb) f.renderer.shutdown();
    if (f.depth_stencil_rb) glDeleteRenderbuffers(1, &f.depth_stencil_rb);
    if (f.color_tex) glDeleteTextures(1, &f.color_tex);
    if (f.fbo) glDeleteFramebuffers(1, &f.fbo);
    f.fbo = f.color_tex = f.depth_stencil_rb = 0;
}

struct GLStateGuard {
    GLint prev_fbo = 0;
    GLint prev_viewport[4] = {0, 0, 0, 0};
    GLint prev_blend_src = GL_SRC_ALPHA, prev_blend_dst = GL_ONE_MINUS_SRC_ALPHA;
    GLboolean prev_blend_enabled = GL_TRUE;

    GLStateGuard() {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
        glGetIntegerv(GL_VIEWPORT, prev_viewport);
        glGetIntegerv(GL_BLEND_SRC, &prev_blend_src);
        glGetIntegerv(GL_BLEND_DST, &prev_blend_dst);
        prev_blend_enabled = glIsEnabled(GL_BLEND);
    }
    ~GLStateGuard() {
        glUseProgram(0);
        glBindVertexArray(0);
        glDisable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilMask(0xFF);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBlendFunc(prev_blend_src, prev_blend_dst);
        if (prev_blend_enabled) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2], prev_viewport[3]);
    }
};

} // namespace

GMLFN(rvm_flash_load) {
    (void)self;
    std::string path = kwik_resolve_read(S(args, argc, 0));

    auto f = std::make_unique<FlashInstance>();
    std::string err;
    if (!swf::load(path, f->movie, err)) return Value(-1.0);

    f->player.init(f->movie);

    f->fb_w = std::max(1, (int)(f->movie.stage.width_px() + 0.5f));
    f->fb_h = std::max(1, (int)(f->movie.stage.height_px() + 0.5f));

    glGenFramebuffers(1, &f->fbo);
    glGenTextures(1, &f->color_tex);
    glGenRenderbuffers(1, &f->depth_stencil_rb);

    glBindTexture(GL_TEXTURE_2D, f->color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, f->fb_w, f->fb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindRenderbuffer(GL_RENDERBUFFER, f->depth_stencil_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, f->fb_w, f->fb_h);

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER,
                              f->depth_stencil_rb);
    bool fbo_ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);

    if (!fbo_ok) {
        destroy_gl(*f);
        return Value(-1.0);
    }

    f->renderer.init(f->movie);
    f->alive = true;
    g_flash.push_back(std::move(f));
    return Value((double)(g_flash.size() - 1));
}

GMLFN(rvm_flash_free) {
    (void)self;
    FlashInstance* f = argc > 0 ? flash_of(args[0]) : nullptr;
    if (f) {
        destroy_gl(*f);
        f->alive = false;
    }
    return Value();
}

GMLFN(rvm_flash_update) {
    (void)self;
    FlashInstance* f = argc > 0 ? flash_of(args[0]) : nullptr;
    if (!f) return Value();

    double frame_dt = f->movie.frame_rate > 0 ? 1.0 / f->movie.frame_rate : 1.0 / 30.0;
    f->frame_accum += render_delta_time();
    int guard = 8;
    while (f->frame_accum >= frame_dt && guard-- > 0) {
        f->player.tick();
        f->frame_accum -= frame_dt;
    }
    return Value();
}

GMLFN(rvm_flash_draw) {
    (void)self;
    FlashInstance* f = argc > 0 ? flash_of(args[0]) : nullptr;
    if (!f) return Value();

    double x = A(args, argc, 1);
    double y = A(args, argc, 2);
    double w = A(args, argc, 3, -1);
    double h = A(args, argc, 4, -1);
    if (w <= 0) w = f->fb_w;
    if (h <= 0) h = f->fb_h;

    {
        GLStateGuard guard;
        glBindFramebuffer(GL_FRAMEBUFFER, f->fbo);
        glViewport(0, 0, f->fb_w, f->fb_h);
        glEnable(GL_STENCIL_TEST);
        glEnable(GL_BLEND);
        glClearColor(0, 0, 0, 0);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        f->renderer.draw_player(f->player);
    }

    render_draw_quad(f->color_tex, x, y, (double)f->fb_w, (double)f->fb_h, 0, 0, w / f->fb_w,
                     h / f->fb_h, 0, 0.f, 1.f, 1.f, 0.f, 0xFFFFFF, render_get_alpha());
    return Value();
}

GMLFN(rvm_flash_width) {
    (void)self;
    FlashInstance* f = argc > 0 ? flash_of(args[0]) : nullptr;
    return Value(f ? (double)f->fb_w : 0.0);
}

GMLFN(rvm_flash_height) {
    (void)self;
    FlashInstance* f = argc > 0 ? flash_of(args[0]) : nullptr;
    return Value(f ? (double)f->fb_h : 0.0);
}

}
