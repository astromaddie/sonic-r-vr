#include "vr/vr_xr.c"
#include <assert.h>
static int validPose=1,shouldRender=0,endLayers=-1,waitCalls,badFov,viewCount=2;
static int owned[2],available[2],acquireFail=-1,waitFail=-1,releaseFail=-1,timeouts;
static unsigned work,flushed,finished,flushCalls,finishCalls,releases,destroys;
static int chainEye(XrSwapchain s){int e=(int)(uintptr_t)s-1;assert(e>=0 && e<2);return e;}
void glFlush(void){flushed=work;flushCalls++;}
void glFinish(void){assert(flushed==work);finished=work;finishCalls++;}
XrResult xrAcquireSwapchainImage(XrSwapchain s,const XrSwapchainImageAcquireInfo *a,uint32_t *i){
 (void)a;int e=chainEye(s);assert(!owned[e]);if(acquireFail==e)return XR_ERROR_RUNTIME_FAILURE;owned[e]=1;*i=0;return XR_SUCCESS;
}
XrResult xrWaitSwapchainImage(XrSwapchain s,const XrSwapchainImageWaitInfo *w){
 int e=chainEye(s);assert(owned[e] && w->timeout==XR_INFINITE_DURATION);
 if(timeouts){timeouts--;return XR_TIMEOUT_EXPIRED;}if(waitFail==e){waitFail=-1;return XR_ERROR_RUNTIME_FAILURE;}
 available[e]=1;return XR_SUCCESS;
}
XrResult xrReleaseSwapchainImage(XrSwapchain s,const XrSwapchainImageReleaseInfo *r){
 (void)r;int e=chainEye(s);assert(owned[e] && available[e] && completed[e] && finished==work);
 if(releaseFail==e){releaseFail=-1;return XR_ERROR_RUNTIME_FAILURE;}
 owned[e]=available[e]=0;releases++;return XR_SUCCESS;
}
XrResult xrDestroySwapchain(XrSwapchain s){int e=chainEye(s);assert(!owned[e]);destroys++;return XR_SUCCESS;}
XrResult xrRequestExitSession(XrSession s){(void)s;assert(0);return XR_SUCCESS;}
XrResult xrDestroySpace(XrSpace s){(void)s;return XR_SUCCESS;}
XrResult xrDestroySession(XrSession s){(void)s;return XR_SUCCESS;}
XrResult xrDestroyInstance(XrInstance i){(void)i;return XR_SUCCESS;}
void vr_input_shutdown(void){}
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
XrResult xrEndFrame(XrSession s,const XrFrameEndInfo *i){(void)s;assert(!owned[0] && !owned[1]);endLayers=i->layerCount;return XR_SUCCESS;}
XrResult xrLocateViews(XrSession s,const XrViewLocateInfo *i,XrViewState *st,uint32_t cap,uint32_t *n,XrView *v){
 (void)s;(void)i;assert(cap==2);*n=viewCount;
 st->viewStateFlags=validPose?(XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT):0;
 for(int e=0;e<2;e++){v[e].pose.position.x=5+e;v[e].pose.orientation.w=1;v[e].fov=(XrFovf){-.5f-e*.1f,.7f-e*.1f,.8f,-.6f};
  if(badFov==1)v[e].fov.angleLeft=NAN;
  if(badFov==2)v[e].fov.angleUp=v[e].fov.angleDown;
  if(badFov==3)v[e].fov.angleRight=1.6f;}
 return XR_SUCCESS;
}
static void protocolReset(void){
 assert(!owned[0] && !owned[1]);
 memset(acquired,0,sizeof acquired);memset(waited,0,sizeof waited);memset(completed,0,sizeof completed);
 work=flushed=finished=flushCalls=finishCalls=releases=destroys=0;acquireFail=waitFail=releaseFail=-1;timeouts=0;
 begun=1;quit=0;offheadRender=0;runLimit=0;endLayers=-1;
 for(int e=0;e<2;e++){chain[e]=(XrSwapchain)(uintptr_t)(e+1);images[e]=calloc(1,sizeof **images);assert(images[e]);images[e][0].image=100+e;}
}
static void protocolFree(void){for(int e=0;e<2;e++){free(images[e]);images[e]=NULL;}}
static void drawEye(int e){assert(owned[e] && available[e] && waited[e] && !completed[e]);work++;glFlush();}
static void protocolTests(void){
 protocolReset();begun=0;vr_xr_end(0);vr_xr_release(0);assert(!finishCalls && !releases);protocolFree();
 protocolReset();
 for(int e=0;e<2;e++){assert(vr_xr_acquire(e)==100u+e);drawEye(e);vr_xr_release(e);}
 vr_xr_end(1);assert(finishCalls==2 && releases==2 && endLayers==1);protocolFree();
 protocolReset();
 for(int e=0;e<2;e++){assert(vr_xr_acquire(e)==100u+e);drawEye(e);}
 assert(!finishCalls && !releases);assert(vr_xr_complete_pair());
 assert(finishCalls==1 && completed[0] && completed[1] && finished==work && !releases);
 /* Optional read-only captures occur here, while both images remain owned. */
 assert(owned[0] && owned[1]);vr_xr_release(0);vr_xr_release(1);vr_xr_end(1);
 assert(finishCalls==1 && releases==2 && endLayers==1);protocolFree();
 protocolReset();assert(!vr_xr_complete_pair() && !finishCalls);
 assert(vr_xr_acquire(0));drawEye(0);acquireFail=1;assert(!vr_xr_acquire(1));
 vr_xr_end(1);assert(finishCalls==1 && releases==1 && endLayers==0);protocolFree();
 protocolReset();assert(vr_xr_acquire(0));drawEye(0);waitFail=1;assert(!vr_xr_acquire(1));
 assert(acquired[1] && !waited[1]);assert(!vr_xr_complete_pair());vr_xr_end(0);
 assert(releases==2 && finishCalls==2 && endLayers==0);protocolFree();
 protocolReset();timeouts=2;assert(vr_xr_acquire(0));assert(!timeouts && waited[0]);drawEye(0);
 releaseFail=0;vr_xr_release(0);assert(acquired[0] && completed[0] && !releases);
 vr_xr_end(0);assert(finishCalls==1 && releases==1);protocolFree();
 protocolReset();assert(vr_xr_acquire(0));drawEye(0);assert(!vr_xr_acquire(0));
 vr_xr_end(0);assert(releases==1);protocolFree();
 protocolReset();for(int e=0;e<2;e++){assert(vr_xr_acquire(e));drawEye(e);}assert(vr_xr_complete_pair());
 VREye viewsOut[2];int64_t time;running=1;validPose=shouldRender=1;
 assert(vr_xr_begin(viewsOut,&time)==1);assert(releases==2 && finishCalls==1 && endLayers==0);
 vr_xr_end(0);protocolFree();
 protocolReset();assert(vr_xr_acquire(0));drawEye(0);running=0;session=XR_NULL_HANDLE;
 vr_xr_shutdown();assert(releases==1 && finishCalls==1 && destroys==2 && !begun);
 puts("VR completion protocol: legacy 2 finishes / paired 1; waits, ownership, partial acquire/wait/release failures, timeout, abandonment and shutdown passed");
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
 assert(!vr_xr_runtime_fov(1));
 vr_xr_deadline(30,1,1);assert(vr_xr_runtime_fov(1));validPose=0;
 assert(vr_xr_begin(e,&time)==1);assert(e[0].pose.p.x==-.032f && e[1].pose.p.x==.032f);
 assert(e[0].fov.left==-.5f && e[1].fov.left==-.6f && e[0].fov.up==.8f);
 assert(runtimeFovLogged);vr_xr_end(1);assert(endLayers==0);
 for(badFov=1;badFov<=3;badFov++){
  quit=0;assert(vr_xr_begin(e,&time)==-1 && quit);vr_xr_end(0);
 }
 badFov=0;viewCount=1;quit=0;assert(vr_xr_begin(e,&time)==-1 && quit);vr_xr_end(0);
 viewCount=2;quit=0;vr_xr_deadline(30,0,0);assert(!vr_xr_runtime_fov(1));assert(vr_xr_runtime_fov(0));
 puts("Runtime FOV fixture: calibrated asymmetric FOV with invalid pose; NaN, unordered, out-of-range, missing views and invalid opt-in rejected; neutral zero layers retained");
 puts("VR offhead fixture: forced neutral is bounded; valid/fallback/forced counters separate; pacing and swapchain sizes retained; zero layers across focus change; normal tracking unchanged");
 protocolTests();
}
