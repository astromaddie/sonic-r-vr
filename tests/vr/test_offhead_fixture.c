#include "vr/vr_xr.c"
#include <assert.h>
static int validPose=1,shouldRender=0,endLayers=-1,waitCalls;
Uint32 SDL_GetTicks(void){return 100;}
void SDL_Delay(Uint32 ms){(void)ms;assert(0);}
void platform_pump_events(void){}
void vr_input_sync(int f){(void)f;}
void vr_input_clear(void){}
void vr_input_profiles_changed(void){}
XrResult xrPollEvent(XrInstance i,XrEventDataBuffer *e){(void)i;(void)e;return XR_EVENT_UNAVAILABLE;}
XrResult xrBeginSession(XrSession s,const XrSessionBeginInfo *i){(void)s;(void)i;return XR_SUCCESS;}
XrResult xrEndSession(XrSession s){(void)s;return XR_SUCCESS;}
XrResult xrResultToString(XrInstance i,XrResult r,char b[XR_MAX_RESULT_STRING_SIZE]){(void)i;(void)r;b[0]=0;return XR_SUCCESS;}
XrResult xrWaitFrame(XrSession s,const XrFrameWaitInfo *i,XrFrameState *f){(void)s;(void)i;waitCalls++;f->predictedDisplayTime=123456;f->shouldRender=shouldRender;return XR_SUCCESS;}
XrResult xrBeginFrame(XrSession s,const XrFrameBeginInfo *i){(void)s;(void)i;return XR_SUCCESS;}
XrResult xrEndFrame(XrSession s,const XrFrameEndInfo *i){(void)s;endLayers=i->layerCount;return XR_SUCCESS;}
XrResult xrLocateViews(XrSession s,const XrViewLocateInfo *i,XrViewState *st,uint32_t cap,uint32_t *n,XrView *v){
 (void)s;(void)i;assert(cap==2);*n=2;
 st->viewStateFlags=validPose?(XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT):0;
 for(int e=0;e<2;e++){v[e].pose.position.x=5+e;v[e].pose.orientation.w=1;v[e].fov.angleLeft=-0.5f;}
 return XR_SUCCESS;
}
int main(void){
 VREye e[2];int64_t time;running=1;width[0]=2160;width[1]=2000;height[0]=height[1]=2160;
 vr_xr_deadline(30,1,1);assert(vr_xr_begin(e,&time)==1);
 assert(waitCalls==1 && time==123456 && e[0].width==2160 && e[1].width==2000);
 assert(e[0].pose.p.x==-0.032f && e[1].pose.p.x==0.032f && e[0].fov.left<-0.78f);
 assert(forcedFrames==1 && validFrames==0 && fallbackFrames==0);
 vr_xr_end(1);assert(endLayers==0 && submittedFrames==0);
 shouldRender=1;focused=1;assert(vr_xr_begin(e,&time)==1);vr_xr_end(1);assert(endLayers==0);
 vr_xr_deadline(30,1,0);assert(vr_xr_begin(e,&time)==1);assert(e[0].pose.p.x==5 && validFrames==1);vr_xr_end(1);assert(endLayers==0);
 validPose=0;assert(vr_xr_begin(e,&time)==1);assert(fallbackFrames==1 && invalidFrames==1);vr_xr_end(1);
 vr_xr_deadline(30,0,1);assert(!forceNeutral);validPose=1;assert(vr_xr_begin(e,&time)==1);assert(e[0].pose.p.x==5);vr_xr_end(1);assert(endLayers==1);
 vr_xr_deadline(0,1,1);assert(!forceNeutral);
 puts("VR offhead fixture: forced neutral is bounded; valid/fallback/forced counters separate; pacing and swapchain sizes retained; zero layers across focus change; normal tracking unchanged");
}
