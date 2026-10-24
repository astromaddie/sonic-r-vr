#ifndef SONICR_VR_WORLD_H
#define SONICR_VR_WORLD_H
#include "vr.h"
#if SONICR_VR
#include "sonicr_globals.h"
/* Shared eye camera origin/basis and addition order. Track, grid and coplanar
 * overlays must describe the same plane before applying their authored bias. */
static inline void vr_world_camera(float wx,float wy,float wz,float p[3]) {
    const float *m=(const float *)g_floatMtxDest;
    float x=wx-g_camFloatX,y=wy-g_camFloatY,z=wz-g_camFloatZ;
    p[0]=x*m[0]+(z*m[8]+y*m[4]);
    p[1]=x*m[1]+(z*m[9]+y*m[5]);
    p[2]=x*m[2]+(z*m[10]+y*m[6]);
}
static inline void vr_world_screen(const float p[3],double *x,double *y) {
    *x=(double)g_screenCenterX+(double)g_projScaleXCurrent*p[0]/p[2];
    *y=(double)g_screenCenterY-(double)g_projScaleY*p[1]/p[2];
}
#endif
#endif
