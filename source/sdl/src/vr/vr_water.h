#ifndef SONICR_VR_WATER_H
#define SONICR_VR_WATER_H
#include "vr_math.h"
/* One context-owned texture; draw is read-only with respect to animation. */
void vr_water_init(void);
void vr_water_shutdown(void);
void vr_water_draw(const float view[16],VRVec camera,VRFov fov,
                   unsigned tick,int frozen,float farClip);
#endif
