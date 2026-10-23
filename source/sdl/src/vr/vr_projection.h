#ifndef SONICR_VR_PROJECTION_H
#define SONICR_VR_PROJECTION_H
#include "vr.h"
/* Keep the native integer raster grid in flat mode. In an eye pass a logical
 * pixel covers several display pixels: rounding each sprite or ground-decal corner here can
 * collapse its area in only one eye, before GL even sees the quad. */
static inline float vr_billboard_coord(int center,int scale,int coord,int depth)
{
#if SONICR_VR
    if (VR_EyeActive()) return (float)center + (float)scale * coord / depth;
#endif
    return (float)(center + scale * coord / depth);
}
/* Ground quads use their projected corners for culling before emitting float
 * RenderVertices. Double locals retain every native int32 corner exactly,
 * including near-plane points far outside the viewport. Keep native products
 * integer as well: float cancellation can turn a negative cross into zero. */
static inline double vr_decal_coord(int center,int scale,int coord,int depth)
{
#if SONICR_VR
    if (VR_EyeActive()) return (double)center + (double)scale * coord / depth;
#endif
    return (double)(center + scale * coord / depth);
}
static inline double vr_decal_cross(double a,double b,double c,double d)
{
#if SONICR_VR
    if (VR_EyeActive()) return a*b-c*d;
#endif
    return (double)((int)a*(int)b-(int)c*(int)d);
}
#if SONICR_VR
/* The atlas has six full strips and a half strip. Anchor its angular origin
 * to the eye's optical axis, not its left clipping edge. Walk backwards when
 * a wider/asymmetric eye can see a preceding strip. */
static inline float vr_panorama_start(int scroll,int *strip,int center,int focal,int left)
{
    float x=(float)left-(float)focal*(scroll&255)/256;
    if (VR_EyeActive()) {
        x=(float)center-focal-(float)focal*(scroll&255)/256;
        while (x>left) {
            *strip=(*strip+6)%7;
            x-=*strip==6?(float)focal*0.5f:(float)focal;
        }
    }
    return x;
}
#endif
#endif
