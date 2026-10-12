/* Android EGL/GLES2 integration probe: all 28 ASTC LDR/sRGB formats must
 * upload and sample correctly, including on Vulkan GPUs without native ASTC.
 * Link to the deployed libEGL_angle/libGLESv2_angle; run inside Android.
 * Constant UNORM16 void-extent block follows astcenc_symbolic_physical.cpp.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s (GL=%x EGL=%x)\n", \
    #x, glGetError(), eglGetError()); return 1; } } while (0)

static GLuint shader(GLenum type, const char *source)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &source, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[2048]; glGetShaderInfoLog(sh, sizeof log, NULL, log);
        fprintf(stderr, "%s\n", log); exit(1); }
    return sh;
}

int main(void)
{
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    CHECK(dpy != EGL_NO_DISPLAY && eglInitialize(dpy, NULL, NULL));
    const EGLint attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint count;
    CHECK(eglChooseConfig(dpy, attrs, &cfg, 1, &count) && count);
    const EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    const EGLint pa[] = { EGL_WIDTH, 4, EGL_HEIGHT, 4, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pa);
    CHECK(ctx != EGL_NO_CONTEXT && surf != EGL_NO_SURFACE);
    CHECK(eglMakeCurrent(dpy, surf, surf, ctx));
    printf("Renderer: %s\n", glGetString(GL_RENDERER));
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    CHECK(ext && strstr(ext, "GL_KHR_texture_compression_astc_ldr"));
    GLuint program = glCreateProgram();
    GLuint vs = shader(GL_VERTEX_SHADER,
        "attribute vec2 p; varying vec2 uv; void main(){"
        "gl_Position=vec4(p,0.,1.); uv=p*.5+.5;}");
    GLuint fs = shader(GL_FRAGMENT_SHADER,
        "precision mediump float; varying vec2 uv; uniform sampler2D tex;"
        "void main(){gl_FragColor=texture2D(tex,uv);}");
    glAttachShader(program, vs); glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "p"); glLinkProgram(program);
    GLint linked; glGetProgramiv(program, GL_LINK_STATUS, &linked); CHECK(linked);
    glUseProgram(program);
    const GLfloat vertices[] = { -1,-1, 1,-1, -1,1, 1,1 };
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
    glEnableVertexAttribArray(0); glViewport(0, 0, 4, 4);
    glDisable(GL_DITHER);
    static const unsigned char green[16] = {
        0xfc,0xfd,0xff,0xff,0xff,0xff,0xff,0xff,
        0,0,0xff,0xff,0,0,0xff,0xff
    };
    const unsigned dims[][2] = { {4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},
        {8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12} };
    for (unsigned srgb = 0; srgb < 2; ++srgb) {
        for (unsigned i = 0; i < 14; ++i) {
            GLuint texture; glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            GLenum format = (srgb ? 0x93d0 : 0x93b0) + i;
            glCompressedTexImage2D(GL_TEXTURE_2D, 0, format, dims[i][0], dims[i][1],
                                   0, sizeof green, green);
            CHECK(glGetError() == GL_NO_ERROR);
            glClearColor(1, 0, 1, 1); glClear(GL_COLOR_BUFFER_BIT);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            unsigned char pixels[4*4*4];
            glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            CHECK(glGetError() == GL_NO_ERROR);
            for (unsigned p = 0; p < sizeof pixels; p += 4)
                CHECK(pixels[p] == 0 && pixels[p+1] == 255 &&
                      pixels[p+2] == 0 && pixels[p+3] == 255);
            glDeleteTextures(1, &texture);
        }
    }
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroySurface(dpy, surf); eglDestroyContext(dpy, ctx); eglTerminate(dpy);
    puts("PASS: 28 ASTC formats uploaded and sampled correctly");
    return 0;
}
