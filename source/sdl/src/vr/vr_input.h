#ifndef SONICR_VR_INPUT_H
#define SONICR_VR_INPUT_H
#include <openxr/openxr.h>
/* One virtual split gamepad; SDL device bindings remain owned by SDL. */
int vr_input_has_frame_extension(void);
int vr_input_init(XrInstance instance,XrSession session,int frameExtension);
void vr_input_shutdown(void);
void vr_input_clear(void);
void vr_input_sync(int focused);
void vr_input_profiles_changed(void);
void vr_input_tick(void);
unsigned short vr_input_read(const short *bindings);
#endif
