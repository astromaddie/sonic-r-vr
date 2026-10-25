#include "vr/vr.h"
/**
 * r_gl_backend.c — OpenGL 1.x implementation of the immediate-mode render API.
 *
 * Implements r_state.h, r_draw.h, r_texture.h, and frame lifecycle.
 * Uses a lazy state tracker to minimize redundant GL calls.
 */

/* SOFT=1 builds replace this whole file with r_soft_backend.c. */
#ifndef SONICR_SOFT_RENDER

#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#ifdef __APPLE__
#include <OpenGL/gl.h>
#elif defined(_WIN32)
#include <GL/glew.h>
#include <GL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "r_types.h"
#include "r_state.h"
#include "r_draw.h"
#include "r_texture.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include <string.h>
#include <stdlib.h>

/* Forward declarations for existing frame functions in render_gl.c */
extern void BeginFrame(void);
extern void EndFrame(void);
extern void FlipD3D(void);
extern void ProcessTpageStates(void);

/* Forward declarations for existing GL_* texture functions in render_gl.c */
extern void GL_UploadTpage(int tpage);
extern void GL_UploadTpageRGBA(int tpage, unsigned char *rgba, int w, int h);
extern void GL_UploadTpageSubRect(int tpage, int destX, int destY, int width, int height);
extern void GL_MarkTpageDirty(int tpage);
extern void GL_ClearTpageDirty(int tpage);
extern void GL_FreezeTpage(int tpage);
extern void GL_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h);
extern void GL_SetNoColorKey(int tpage);
extern void GL_ClearNoColorKey(int tpage);
extern void GL_SetTpageGreen6(int tpage, int on);
extern void GL_InitTextures(void);
extern void GL_SetTpageRGBA8(int tpage, unsigned char *rgba);

/* =====================================================================
 * Internal state snapshot
 * ===================================================================== */

typedef struct {
    int          textureId;    /* tpage, -1 untextured, -2 external VR GL object */
    R_BlendMode  blendMode;
    int          depthTest;
    R_DepthFunc  depthFunc;
    int          depthWrite;
    R_TexEnvMode texEnv;
    R_FilterMode filter;
    R_CullMode   cullMode;
    int          alphaTest;
    float        alphaRef;
    int          scissorEnabled;
    int          scissorX, scissorY, scissorW, scissorH;
} R_StateSnapshot;

static R_StateSnapshot s_desired;
static R_StateSnapshot s_current;
#if SONICR_VR
/* Preserve the 60-byte public/diagnostic state snapshot layout. External GL
 * names use textureId=-2 and are tracked beside the snapshot. */
static unsigned s_desiredTextureObject,s_currentTextureObject;
static unsigned s_textureObjectStack[4];
#endif

/* State stack for R_PushState / R_PopState */
#define R_STATE_STACK_DEPTH 4
static R_StateSnapshot s_stateStack[R_STATE_STACK_DEPTH];
static int s_stackDepth = 0;

/* Default state values */
static const R_StateSnapshot s_defaults = {
    .textureId      = -1,
    .blendMode      = R_BLEND_ALPHA,
    .depthTest      = 1,
    .depthFunc      = R_DEPTH_LEQUAL,
    .depthWrite     = 1,
    .texEnv         = R_TEXENV_MODULATE,
    .filter         = R_FILTER_NEAREST,
    .cullMode       = R_CULL_NONE,
    .alphaTest      = 1,
    .alphaRef       = 0.01f,
    .scissorEnabled = 0,
    .scissorX       = 0,
    .scissorY       = 0,
    .scissorW       = 640,
    .scissorH       = 480,
};

/* =====================================================================
 * Texture backend state
 * ===================================================================== */

/* GL texture objects and dirty flags — still owned by render_gl.c,
 * accessed here via extern for texture binding during R_FlushState. */
extern GLuint s_glTextures[];       /* render_gl.c */

/* Per-tpage filter override, and the filter actually resident on each GL
 * texture object.
 *
 * GL keeps filter state on the TEXTURE OBJECT, not globally — so a single
 * cached "current filter" is wrong the moment a different texture is bound.
 * It is also wrong after any upload: every GL_UploadTpage* path hard-sets
 * GL_NEAREST (render_gl.c:139, :247), so a filter applied earlier is stomped
 * whenever the tpage is re-uploaded. Together those made the old global
 * R_SetFilter path reach only whichever texture happened to be bound at the
 * moment the value changed, and nothing else.
 *
 * The filter is therefore resolved and applied per tpage at BIND time. Both
 * arrays encode 0 = unset and (mode + 1) otherwise, so the zero initialiser
 * means "no override" / "resident filter unknown". s_glTextureFilter is
 * invalidated by R_MarkTextureDirty, which precedes every upload path. */
static unsigned char s_tpageFilter[52];
static unsigned char s_glTextureFilter[52];

/* Apply the effective filter to tpage `tp`, which must be bound already. */
static void GL_ApplyTpageFilter(int tp)
{
    unsigned char want = s_tpageFilter[tp];
    if (want == 0) {
        want = (unsigned char)(s_desired.filter + 1);
    }
    if (s_glTextureFilter[tp] != want) {
        GLenum f = (want == (unsigned char)(R_FILTER_LINEAR + 1)) ? GL_LINEAR
                                                                  : GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
        s_glTextureFilter[tp] = want;
    }
}

#if SONICR_VR
static int s_vrBatching=1,s_batchOpen,s_vrDepthReversed;
static unsigned s_batchVertices;
/* Bounded client storage; split only at triangle boundaries. No GPU resource
 * lifetime is introduced. Array pointers are consumed by glDrawArrays. */
#define R_ARRAY_VERTICES 3072u
typedef struct { float xyzw[4],color[4],uv[2]; } R_ArrayVertex;
static R_ArrayVertex s_arrayVertices[R_ARRAY_VERTICES];
static int s_vrClientArrays,s_batchArrays;
static float s_arrayColor[4];
static void R_SubmitArray(void)
{
    if (!s_batchVertices) return;
    /* This backend is the sole array/buffer client in the desktop renderer.
     * Explicitly use client memory and texture unit0. Save all client vertex
     * state (including pointers, enables and buffer bindings), without queries. */
    glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
    glBindBuffer(GL_ARRAY_BUFFER,0);
    glClientActiveTexture(GL_TEXTURE0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(4,GL_FLOAT,sizeof(R_ArrayVertex),s_arrayVertices[0].xyzw);
    glColorPointer(4,GL_FLOAT,sizeof(R_ArrayVertex),s_arrayVertices[0].color);
    glTexCoordPointer(2,GL_FLOAT,sizeof(R_ArrayVertex),s_arrayVertices[0].uv);
    glDrawArrays(GL_TRIANGLES,0,(GLsizei)s_batchVertices);
    glPopClientAttrib();
    /* DrawArrays makes current enabled attributes unspecified. Preserve the
     * final immediate color/UV for raw GL and native/menu draws after barriers. */
    const R_ArrayVertex *last=s_arrayVertices+s_batchVertices-1;
    glColor4f(last->color[0],last->color[1],last->color[2],last->color[3]);
    glTexCoord2f(last->uv[0],last->uv[1]);
    VR_TraceArrayBatch(s_batchVertices);
    s_batchVertices=0;
}
static unsigned s_earlyFlushRemaining;
static uint32_t s_batchColor;
static int s_batchColorValid;
static R_StateSnapshot s_batchState;
static unsigned char s_batchFilter;
static unsigned s_batchTextureObject;
void R_EndBatch(void)
{
    if (!s_batchOpen) return;
    if (s_batchArrays) R_SubmitArray();
    else {glEnd();VR_TraceBatch(s_batchVertices);}
    s_batchOpen=0;s_batchArrays=0;s_batchColorValid=0;
    s_batchVertices=0;
}
void R_SetVRClientArrays(int enabled)
{
    R_EndBatch();
    s_vrClientArrays=enabled!=0;
}
void R_SetVRBatching(int enabled)
{
    R_EndBatch();
    s_vrBatching=enabled!=0;
}
void R_SetVREarlyFlush(unsigned draws)
{
    R_EndBatch();
    s_earlyFlushRemaining=draws;
}
/* Called once after a complete logical fan, never in the vertex loop. The
 * countdown is armed only for a race eye, and zero after its one early flush. */
static void R_EyeDrawSubmitted(void)
{
    if (--s_earlyFlushRemaining) return;
    R_EndBatch();
    glFlush();
}

#endif

static GLenum GL_DepthComparison(R_DepthFunc f)
{
#if SONICR_VR
    if (s_vrDepthReversed) {
        if (f==R_DEPTH_LEQUAL) return GL_GEQUAL;
        if (f==R_DEPTH_LESS) return GL_GREATER;
    }
#endif
    return f==R_DEPTH_LEQUAL?GL_LEQUAL:f==R_DEPTH_LESS?GL_LESS:GL_ALWAYS;
}
#if SONICR_VR
void R_SetVRDepthReversed(int enabled)
{
    R_EndBatch();
    s_vrDepthReversed=enabled!=0;
    glDepthFunc(GL_DepthComparison(s_desired.depthFunc));
    s_current.depthFunc=s_desired.depthFunc;
}
#endif

/* =====================================================================
 * R_FlushState — apply only the GL calls for state that changed
 * ===================================================================== */

void R_FlushState(void)
{
    R_EndBatch();
    /* Texture binding — skip if tpage unchanged.
     * Dirty-texture uploads still happen even on a cache hit (the tpage
     * data changed, not the binding). */
    int textureChanged=s_desired.textureId != s_current.textureId;
#if SONICR_VR
    if(!textureChanged && s_desired.textureId==-2)
        textureChanged=s_desiredTextureObject!=s_currentTextureObject;
#endif
    if (textureChanged) {
#if SONICR_VR
        if(s_desired.textureId==-2) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D,s_desiredTextureObject);
        } else
#endif
        if (s_desired.textureId < 0) {
            glDisable(GL_TEXTURE_2D);
        } else {
            int tp = s_desired.textureId;
            if (tp < 52) {
                if (g_tpagePixelBuf[tp] != NULL) {
                    if (s_glTextureDirty[tp]) {
                        GL_UploadTpage(tp);
                    }
                    glEnable(GL_TEXTURE_2D);
                    glBindTexture(GL_TEXTURE_2D, s_glTextures[tp]);
                }
                else {
                    glDisable(GL_TEXTURE_2D);
                }
            }
        }
        s_current.textureId = s_desired.textureId;
#if SONICR_VR
        s_currentTextureObject=s_desired.textureId==-2?s_desiredTextureObject:0;
#endif
    }
    else if (s_desired.textureId >= 0 && s_desired.textureId < 52) {
        /* Same tpage, but check if pixel data was updated */
        if (s_glTextureDirty[s_desired.textureId]) {
            GL_UploadTpage(s_desired.textureId);
        }
    }

    /* Filter, resolved per tpage against the texture actually bound. Placed
     * after the whole binding block so it covers all three ways the resident
     * filter can go stale: a fresh bind, an in-place re-upload of the same
     * tpage, and a global R_SetFilter while the same tpage stays bound. */
    if (s_desired.textureId >= 0 && s_desired.textureId < 52 &&
        g_tpagePixelBuf[s_desired.textureId] != NULL) {
        GL_ApplyTpageFilter(s_desired.textureId);
    }

    /* Depth test */
    if (s_desired.depthTest != s_current.depthTest) {
        if (s_desired.depthTest) {
            glEnable(GL_DEPTH_TEST);
        }
        else {
            glDisable(GL_DEPTH_TEST);
        }
        s_current.depthTest = s_desired.depthTest;
    }

    /* Depth function */
    if (s_desired.depthFunc != s_current.depthFunc) {
        glDepthFunc(GL_DepthComparison(s_desired.depthFunc));
        s_current.depthFunc = s_desired.depthFunc;
    }

    /* Depth write */
    if (s_desired.depthWrite != s_current.depthWrite) {
        glDepthMask(s_desired.depthWrite ? GL_TRUE : GL_FALSE);
        s_current.depthWrite = s_desired.depthWrite;
    }

    /* Texture environment (overbright) */
    if (s_desired.texEnv != s_current.texEnv) {
#ifdef __EMSCRIPTEN__
        /* WebGL legacy GL emulation doesn't support GL_COMBINE/GL_ADD_SIGNED.
         * Fall back to plain GL_MODULATE for all modes. */
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
#else
        if (s_desired.texEnv == R_TEXENV_ADD_SIGNED) {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_ADD_SIGNED);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE0_RGB, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SOURCE1_RGB, GL_PREVIOUS);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND0_RGB, GL_SRC_COLOR);
            glTexEnvi(GL_TEXTURE_ENV, GL_OPERAND1_RGB, GL_SRC_COLOR);
        }
        else {
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
            glTexEnvf(GL_TEXTURE_ENV, GL_RGB_SCALE, 1.0f);
        }
#endif
        s_current.texEnv = s_desired.texEnv;
    }

    /* Blend mode */
    if (s_desired.blendMode != s_current.blendMode) {
        switch (s_desired.blendMode) {
            case R_BLEND_NONE:
                glDisable(GL_BLEND);
                break;
            case R_BLEND_ALPHA:
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                break;
            case R_BLEND_ADDITIVE:
                glEnable(GL_BLEND);
                glBlendFunc(GL_ONE, GL_ONE);
                break;
        }
        s_current.blendMode = s_desired.blendMode;
    }

    /* Filter is applied per tpage at bind time above — see GL_ApplyTpageFilter.
     * GL keeps it on the texture object, so it cannot be driven from here. The
     * snapshot field is still synced so the two fields never disagree, which
     * would trip up any whole-struct compare added later (the GLES2 backend
     * already has one). */
    s_current.filter = s_desired.filter;

    glDisable(GL_CULL_FACE);

    /* Alpha test */
    if (s_desired.alphaTest != s_current.alphaTest) {
        if (s_desired.alphaTest) {
            glEnable(GL_ALPHA_TEST);
        }
        else {
            glDisable(GL_ALPHA_TEST);
        }
        s_current.alphaTest = s_desired.alphaTest;
    }

    /* Alpha ref */
    if (s_desired.alphaRef != s_current.alphaRef) {
        glAlphaFunc(GL_GREATER, s_desired.alphaRef);
        s_current.alphaRef = s_desired.alphaRef;
    }

    /* Scissor enable/disable */
    if (s_desired.scissorEnabled != s_current.scissorEnabled) {
        if (s_desired.scissorEnabled) {
            glEnable(GL_SCISSOR_TEST);
        }
        else {
            glDisable(GL_SCISSOR_TEST);
        }
        s_current.scissorEnabled = s_desired.scissorEnabled;
    }

    /* Scissor rect */
    if (s_desired.scissorEnabled &&
        (s_desired.scissorX != s_current.scissorX ||
         s_desired.scissorY != s_current.scissorY ||
         s_desired.scissorW != s_current.scissorW ||
         s_desired.scissorH != s_current.scissorH))
    {
        glScissor(s_desired.scissorX, s_desired.scissorY,
                  s_desired.scissorW, s_desired.scissorH);
        s_current.scissorX = s_desired.scissorX;
        s_current.scissorY = s_desired.scissorY;
        s_current.scissorW = s_desired.scissorW;
        s_current.scissorH = s_desired.scissorH;
    }
}

/* =====================================================================
 * Render state API implementation
 * ===================================================================== */

void R_SetTexture(int tpageIndex)
{
    s_desired.textureId = tpageIndex;
#if SONICR_VR
    s_desiredTextureObject=0;
#endif
}

#if SONICR_VR
void R_SetTextureObject(unsigned int textureObject)
{
    s_desired.textureId=textureObject?-2:-1;
    s_desiredTextureObject=textureObject;
}
#endif

void R_SetBlendMode(R_BlendMode mode)
{
    s_desired.blendMode = mode;
}

void R_SetDepthTest(int enable)
{
    s_desired.depthTest = enable;
}

void R_SetDepthFunc(R_DepthFunc func)
{
    s_desired.depthFunc = func;
}

void R_SetDepthWrite(int enable) 
{
    s_desired.depthWrite = enable;
}

void R_SetTexEnv(R_TexEnvMode mode)
{
    s_desired.texEnv = mode;
}

void R_SetFilter(R_FilterMode mode)
{
    s_desired.filter = mode;
}

/* Per-tpage filter pin. Takes priority over the global R_SetFilter value;
 * applied to the texture object at bind time by GL_ApplyTpageFilter. */
#if SONICR_VR
int R_GetTpageFilter(int tpage) {
    return tpage>=0 && tpage<52 ? (int)s_tpageFilter[tpage]-1 : -1;
}
#endif
void R_SetTpageFilter(int tpage, R_FilterMode mode)
{
    if (tpage >= 0 && tpage < 52) {
        R_EndBatch();
        s_tpageFilter[tpage] = (unsigned char)(mode + 1);
    }
}

void R_ClearTpageFilter(int tpage)
{
    if (tpage >= 0 && tpage < 52) {
        R_EndBatch();
        s_tpageFilter[tpage] = 0;
    }
}

/* Chroma gain is a DC-only upload-time transform — GL uploads RGBA directly
 * and the SDL path renders true Add Signed, so there is nothing to correct. */
void R_SetTpageSatBoost(int tpage, int k256)
{
    (void)tpage;
    (void)k256;
}

void R_SetCullMode(R_CullMode mode)
{
    s_desired.cullMode = mode;
}

void R_SetAlphaTest(int enable)
{
    s_desired.alphaTest = enable;
}

void R_SetAlphaRef(float ref)
{
    s_desired.alphaRef = ref;
}

void R_SetScissor(int x, int y, int w, int h)
{
    s_desired.scissorEnabled = 1;
    s_desired.scissorX = x;
    s_desired.scissorY = y;
    s_desired.scissorW = w;
    s_desired.scissorH = h;
}

void R_DisableScissor(void)
{
    s_desired.scissorEnabled = 0;
}

void R_PushState(void)
{
    if (s_stackDepth < R_STATE_STACK_DEPTH) {
        s_stateStack[s_stackDepth] = s_desired;
#if SONICR_VR
        s_textureObjectStack[s_stackDepth]=s_desiredTextureObject;
#endif
        s_stackDepth++;
    }
}

void R_PopState(void)
{
    if (s_stackDepth > 0) {
        s_desired = s_stateStack[--s_stackDepth];
#if SONICR_VR
        s_desiredTextureObject=s_textureObjectStack[s_stackDepth];
#endif
    }
}

void R_DebugGetState(int *depthTest, int *depthWrite, int *depthFunc,
                     int *blendMode, int *alphaTest, float *alphaRef)
{
    if (depthTest)  *depthTest  = s_desired.depthTest;
    if (depthWrite) *depthWrite = s_desired.depthWrite;
    if (depthFunc)  *depthFunc  = (int)s_desired.depthFunc;
    if (blendMode)  *blendMode  = (int)s_desired.blendMode;
    if (alphaTest)  *alphaTest  = s_desired.alphaTest;
    if (alphaRef)   *alphaRef   = s_desired.alphaRef;
}

void R_ResetState(void)
{
    R_EndBatch();
    s_desired = s_defaults;
#if SONICR_VR
    s_desiredTextureObject=s_currentTextureObject=s_batchTextureObject=0;
#endif
    /* Force full re-sync on next R_FlushState by invalidating current.
     * Can't use plain memset(0xFF) because 0xFFFFFFFF == -1, which matches
     * s_defaults.textureId — so textureId would appear "unchanged" and
     * R_FlushState would skip the glDisable(GL_TEXTURE_2D) call. */
    memset(&s_current, 0xFF, sizeof(s_current));
    s_current.textureId = -99;   /* won't match any valid id or -1 */
    s_current.alphaRef = -1.0f;  /* ensure float comparison triggers */
    /* Resident texture-object filters are not part of this snapshot; drop
     * what we believe about them so the next bind re-applies. */
    memset(s_glTextureFilter, 0, sizeof(s_glTextureFilter));
    s_stackDepth = 0;
}

/* =====================================================================
 * Geometry submission — immediate draw
 * ===================================================================== */

/* Emit a single vertex via GL immediate mode.
 * Uses glVertex4f with W=1/RHW for perspective-correct interpolation. */
static inline void R_EmitVertex(const RenderVertex *v)
{
    uint32_t argb = v->color;
#if SONICR_VR
    /* Current GL color persists within Begin/End. Only cache inside our own
     * eye batch; every new batch emits its first color after external GL. */
    if (!s_batchOpen || !s_batchColorValid || argb!=s_batchColor) {
#endif
        float a = (float)((argb >> 24) & 0xFF) / 255.0f;
        float r = (float)((argb >> 16) & 0xFF) / 255.0f;
        float g = (float)((argb >>  8) & 0xFF) / 255.0f;
        float b = (float)((argb      ) & 0xFF) / 255.0f;
#if SONICR_VR
        if (s_batchArrays) {
            s_arrayColor[0]=r;s_arrayColor[1]=g;s_arrayColor[2]=b;s_arrayColor[3]=a;
        } else
#endif
        glColor4f(r, g, b, a);
#if SONICR_VR
        if (s_batchOpen) { s_batchColor=argb;s_batchColorValid=1; }
    }
    if (s_batchArrays) {
        R_ArrayVertex *out=s_arrayVertices+s_batchVertices++;
        memcpy(out->color,s_arrayColor,sizeof out->color);
        out->uv[0]=v->u;out->uv[1]=v->v;
        VR_TransformVertex(v,out->xyzw);
        return;
    }
#endif
    glTexCoord2f(v->u, v->v);

#if SONICR_VR
    if (VR_EyeActive()) { VR_EmitVertex(v); return; }
#endif
    /* VR_EmitVertex computes its own W. Keep the native reciprocal after the
     * early return: GCC -Os otherwise evaluates it twice for each eye vertex. */
    float w = (v->rhw > 0.0f) ? (1.0f / v->rhw) : 1.0f;
    glVertex4f(v->sx * w, v->sy * w, v->sz * w, w);
}

void R_DrawTriFan(const RenderVertex *v, int count)
{
    if (count < 3) {
        return;
    }

#if SONICR_VR
    VR_TraceDraw(v,count,&s_desired,sizeof(s_desired));
    if (s_vrBatching && VR_EyeActive()) {
        int tp=s_desired.textureId;
        int externalTexture=tp==-2;
        int textured=externalTexture || (tp>=0 && tp<52);
        unsigned char filter=0;
        if(tp>=0 && tp<52) {
            filter=s_tpageFilter[tp];
            if(!filter)filter=(unsigned char)(s_desired.filter+1);
        }
        /* Keep only adjacent compatible primitives together. Snapshot desired
         * state, not s_current: the latter intentionally ignores cull requests.
         * A dirty tpage must upload before vertices using its new contents. */
        if (!s_batchOpen || memcmp(&s_batchState,&s_desired,sizeof(s_desired)) ||
            filter!=s_batchFilter ||
            (externalTexture && s_batchTextureObject!=s_desiredTextureObject) ||
            (!externalTexture && textured && s_glTextureDirty[tp])) {
            R_FlushState();
            s_batchArrays=s_vrClientArrays;
            if (!s_batchArrays) glBegin(GL_TRIANGLES);
            s_batchOpen=1;s_batchVertices=0;s_batchColorValid=0;
            s_batchState=s_desired;s_batchFilter=filter;
            s_batchTextureObject=externalTexture?s_desiredTextureObject:0;
        }
        /* Exactly the original fan's triangle/vertex order, including its
         * provoking vertex. Transform immediately with the current eye/layer. */
        for (int i=1;i<count-1;i++) {
            if (s_batchArrays && s_batchVertices+3>R_ARRAY_VERTICES) R_SubmitArray();
            R_EmitVertex(v);R_EmitVertex(v+i);R_EmitVertex(v+i+1);
            if (!s_batchArrays) s_batchVertices+=3;
        }
        if (s_earlyFlushRemaining) R_EyeDrawSubmitted();
        return;
    }
#endif
    R_FlushState();

    glBegin(GL_TRIANGLE_FAN);
    for (int i = 0; i < count; i++) {
        R_EmitVertex(&v[i]);
    }
    glEnd();
#if SONICR_VR
    VR_TraceBatch((unsigned)count);
    if (s_earlyFlushRemaining && VR_EyeActive()) R_EyeDrawSubmitted();
#endif
}

void R_DrawTri(const RenderVertex v[3])
{
    R_DrawTriFan(v, 3);
}

void R_DrawQuad(const RenderVertex v[4])
{
    R_DrawTriFan(v, 4);
}

/* =====================================================================
 * 2D quad helpers
 * ===================================================================== */

void R_DrawQuad2D(float x0, float y0, float x1, float y1,
                  float u0, float v0, float u1, float v1,
                  float z, uint32_t color)
{
    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, u0, v0 };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, u1, v0 };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, u1, v1 };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, u0, v1 };
    R_DrawQuad(v);
}

void R_DrawQuad2DSolid(float x0, float y0, float x1, float y1,
                       float z, uint32_t color)
{
    /* Temporarily force untextured rendering */
    int savedTex = s_desired.textureId;
#if SONICR_VR
    unsigned savedTextureObject=s_desiredTextureObject;
#endif
    s_desired.textureId = -1;
#if SONICR_VR
    s_desiredTextureObject=0;
#endif

    float rhw = (z > 0.0f) ? (1.0f / z) : 1.0f;
    float farSafe = (g_farClipFloat > 0.0f) ? g_farClipFloat : 1.0f;
    float normZ = z / farSafe;

    RenderVertex v[4];
    v[0] = (RenderVertex){ x0, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[1] = (RenderVertex){ x1, y0, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[2] = (RenderVertex){ x1, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    v[3] = (RenderVertex){ x0, y1, normZ, rhw, color, 0, 0.0f, 0.0f };
    R_DrawQuad(v);

    /* Restore previous texture state */
    s_desired.textureId = savedTex;
#if SONICR_VR
    s_desiredTextureObject=savedTextureObject;
#endif
}

/* =====================================================================
 * Texture API (forward to render_gl.c)
 * ===================================================================== */

void R_InitTextures(void)
{
    GL_InitTextures();
}

void R_UploadTexture(int tpage)
{
    GL_UploadTpage(tpage);
}

void R_MarkTextureDirty(int tpage)
{
    /* The upload this schedules will hard-set GL_NEAREST on the texture
     * object (render_gl.c:139, :247), so forget the resident filter and let
     * the next bind re-apply it. */
    if (tpage >= 0 && tpage < 52) {
        s_glTextureFilter[tpage] = 0;
    }
    GL_MarkTpageDirty(tpage);
}

void R_ClearTextureDirty(int tpage)
{
    GL_ClearTpageDirty(tpage);
}

void R_FreezeTexture(int tpage)
{
    GL_FreezeTpage(tpage);
}

void R_ThawTexture(int tpage)
{
    (void)tpage; /* GL has no persistent freeze flag */
}

void R_SetNoColorKey(int tpage)
{
    GL_SetNoColorKey(tpage);
}

void R_ClearNoColorKey(int tpage)
{
    GL_ClearNoColorKey(tpage);
}

void R_SetTpageGreen6(int tpage, int on)
{
    GL_SetTpageGreen6(tpage, on);
}

void R_SetTpageRGBA8(int tpage, unsigned char *rgba) {
    GL_SetTpageRGBA8(tpage, rgba);
}

void R_UploadTextureRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    GL_UploadTpageRGBA(tpage, rgba, w, h);
}

void R_UploadTextureSubRect(int tpage, int x, int y, int w, int h)
{
    GL_UploadTpageSubRect(tpage, x, y, w, h);
}

void R_SetPendingRGBA(int tpage, unsigned char *rgba, int w, int h)
{
    GL_SetPendingRGBA(tpage, rgba, w, h);
}

void R_EndFrame(void)
{
    EndFrame();
    /* EndFrame is just glFlush — no GL state changes, tracker stays valid. */
}

void R_Flip(void)
{
    FlipD3D();
}

void R_ClearAndReset(void)
{
    ProcessTpageStates();
}

void R_ClearDepth(void)
{
    R_FlushState();
    glClear(GL_DEPTH_BUFFER_BIT);
}

#endif /* !SONICR_SOFT_RENDER */
