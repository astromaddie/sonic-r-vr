#ifndef SONICR_VR_H
#define SONICR_VR_H
#ifndef SONICR_VR
#define SONICR_VR 0
#endif
#if SONICR_VR
#include "r_types.h"
int VR_Init(void);
void VR_Shutdown(void);
int VR_Diagnostic(void);
unsigned short VR_PadBits(void);
int VR_AutoRace(void);
void VR_ConfigureAutoRace(void);
void VR_PerfWeather(int start);
void VR_DiagnosticInput(void);
void VR_TraceBatch(unsigned vertices);
void VR_TraceDraw(const RenderVertex *v,int count,const void *state,size_t size);
void VR_RaceStart(void);
int VR_RaceFrame(void); /* -1 exit, 0 render-only, 1 native tick */
void VR_RenderRace(int tick);
int VR_Present(void); /* intercept FlipD3D, menus use finite stereo panel */
int VR_EyeActive(void);
int VR_DepthReversed(void);
int VR_Repeat(void);
void VR_ApplyCamera(int **camera);
void VR_Viewport(int *w,int *h);
void VR_Layer(int layer); /* 0 world, 1 HUD, 2 sky, 3 full-eye fade */
void VR_EmitVertex(const RenderVertex *v);
float VR_LerpWorldDepth(float a,float b,float t);
void VR_SkyBegin(void);
void VR_SkyEnd(void);
#endif
#endif
