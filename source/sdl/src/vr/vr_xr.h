#ifndef SONICR_VR_XR_H
#define SONICR_VR_XR_H
#include "vr_math.h"
typedef struct { VRPose pose; VRFov fov; unsigned texture; int width,height; } VREye;
int vr_xr_init(void);
void vr_xr_deadline(unsigned seconds,int offhead,int neutral);
int vr_xr_offhead(void);
int vr_xr_focused(void);
void vr_xr_shutdown(void);
int vr_xr_begin(VREye eyes[2],int64_t *time); /* -1 exit, 0 standby, 1 visible */
unsigned vr_xr_acquire(int eye);
void vr_xr_release(int eye);
void vr_xr_end(int rendered);
#endif
