/* Exercise the real BeginFrame projection setup without an SDL window or GL
 * context. Only the GL/platform boundary and unrelated state reset are faked. */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "sonicr_globals.h"

extern void BeginFrame(void);

static GLenum matrixMode;
static GLdouble orthoLeft, orthoRight, orthoBottom, orthoTop;
static GLfloat translateX, translateY, scaleX, scaleY;

void platform_get_drawable_size(int *width, int *height)
{
    *width = 1280;
    *height = 960;
}

void R_ResetState(void) {}

void glGenTextures(GLsizei count, GLuint *textures)
{
    for (GLsizei i = 0; i < count; ++i) textures[i] = (GLuint)i + 1;
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{ (void)x; (void)y; (void)width; (void)height; }
void glMatrixMode(GLenum mode) { matrixMode = mode; }
void glLoadIdentity(void)
{
    if (matrixMode == GL_MODELVIEW) {
        translateX = translateY = 0.0f;
        scaleX = scaleY = 1.0f;
    }
}
void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble nearValue, GLdouble farValue)
{
    assert(matrixMode == GL_PROJECTION);
    assert(nearValue == 0.0 && farValue == -1.0);
    orthoLeft = left; orthoRight = right;
    orthoBottom = bottom; orthoTop = top;
}
void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{ (void)z; translateX = x; translateY = y; }
void glScalef(GLfloat x, GLfloat y, GLfloat z)
{ (void)z; scaleX = x; scaleY = y; }

static double screen_x_to_ndc(double x)
{
    double clipX = translateX + scaleX * x;
    return 2.0 * (clipX - orthoLeft) / (orthoRight - orthoLeft) - 1.0;
}

static double screen_y_to_ndc(double y)
{
    double clipY = translateY + scaleY * y;
    return 2.0 * (clipY - orthoBottom) / (orthoTop - orthoBottom) - 1.0;
}

static void check_projection(int fullHeight, int expectedClipHeight)
{
    g_screenWidth = 640;
    g_screenHeight = 480;
    g_screenHeightFull = fullHeight;
    BeginFrame();

    /* glOrtho rejects equal bounds; also check the exact established clip
     * values so a changed positive configuration path is caught. */
    assert(orthoRight > orthoLeft);
    assert(orthoTop > orthoBottom);
    assert(orthoLeft == -1.0 && orthoRight == 1.0);
    assert(orthoBottom == -expectedClipHeight);
    assert(orthoTop == expectedClipHeight);

    assert(fabs(screen_x_to_ndc(0) + 1.0) < 1e-6);
    assert(fabs(screen_x_to_ndc(320)) < 1e-6);
    assert(fabs(screen_x_to_ndc(640) - 1.0) < 1e-6);
    assert(fabs(screen_y_to_ndc(0) - 1.0) < 1e-6);
    assert(fabs(screen_y_to_ndc(240)) < 1e-6);
    assert(fabs(screen_y_to_ndc(480) + 1.0) < 1e-6);
}

int main(void)
{
    /* Startup: main has established logical dimensions, but configured
     * viewport dimensions are still zero. */
    check_projection(0, 480);

    /* A later configured positive height retains the existing projection
     * bounds and mapping. */
    check_projection(720, 720);

    puts("BeginFrame menu projection: startup fallback and configured viewport mapping passed");
    return 0;
}
