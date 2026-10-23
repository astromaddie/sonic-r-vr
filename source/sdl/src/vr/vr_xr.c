#include "vr.h"
#if SONICR_VR
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL
#include <X11/Xlib.h>
#include <GL/glx.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "vr_xr.h"
#include "vr_input.h"
extern SDL_Window *g_sdlWindow;
static XrInstance instance;
static XrSession session;
static XrSpace space;
static XrSwapchain chain[2];
static XrSwapchainImageOpenGLKHR *images[2];
static int width[2],height[2],running,begun,quit,acquired[2],waited[2];
static XrFrameState frame;
static int focused;
static uint32_t runStart,runLimit,lastProgress;
static unsigned frameWaits,standbyFrames,invalidFrames,validFrames,submittedFrames,renderedEyes,fallbackFrames;
static int offheadRender,lastPoseKind=-1;
int vr_xr_offhead(void) { return offheadRender; }
void vr_xr_deadline(unsigned seconds,int offhead) { runStart=lastProgress=SDL_GetTicks();runLimit=seconds*1000;offheadRender=offhead; }
static void progress(const char *reason) {
    fprintf(stderr,"[vr-runtime] %s elapsed_ms=%u running=%d focused=%d waits=%u standby=%u invalid_pose=%u valid_pose=%u synthetic_pose=%u rendered_eyes=%u submitted_layers=%u offhead=%d\n",
        reason,SDL_GetTicks()-runStart,running,focused,frameWaits,standbyFrames,invalidFrames,validFrames,fallbackFrames,renderedEyes,submittedFrames,offheadRender);
}
int vr_xr_focused(void) { return focused; }
static XrView views[2];
static int check(XrResult result,const char *call) {
    if(XR_SUCCEEDED(result)) return 1;
    char str[XR_MAX_RESULT_STRING_SIZE]={0};
    if(instance) xrResultToString(instance,result,str);
    fprintf(stderr,"[vr] %s failed: %d %s\n",call,result,str);
    quit=1;
    return 0;
}
#define TRY(call) do { if(!check((call),#call)) return 0; } while(0)
int vr_xr_init(void) {
    SDL_SysWMinfo wm; SDL_VERSION(&wm.version);
    if(!SDL_GetWindowWMInfo(g_sdlWindow,&wm) || wm.subsystem!=SDL_SYSWM_X11) {
        fprintf(stderr,"[vr] OpenXR requires SDL X11/GLX\n"); return 0;
    }
    Display *display=wm.info.x11.display;
    GLXContext context=glXGetCurrentContext();
    int id=0,count=0;
    if(!context || glXQueryContext(display,context,GLX_FBCONFIG_ID,&id)!=Success) return 0;
    const int attr[]={GLX_FBCONFIG_ID,id,None};
    GLXFBConfig *configs=glXChooseFBConfig(display,DefaultScreen(display),attr,&count);
    if(!configs || count!=1) { if(configs)XFree(configs); return 0; }
    GLXFBConfig config=configs[0]; XFree(configs);
    XWindowAttributes wa;
    if(!XGetWindowAttributes(display,wm.info.x11.window,&wa))return 0;
    int visual=0; glXGetFBConfigAttrib(display,config,GLX_VISUAL_ID,&visual);
    if((VisualID)visual!=XVisualIDFromVisual(wa.visual))return 0;
    int frameExtension=vr_input_has_frame_extension();if(frameExtension<0)return 0;
    const char *extensions[]={XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,"XR_VALVE_frame_controller_interaction"};
    XrInstanceCreateInfo ci={.type=XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName,"Sonic R VR");
    ci.applicationInfo.apiVersion=XR_API_VERSION_1_0;
    ci.enabledExtensionCount=frameExtension?2:1; ci.enabledExtensionNames=extensions;
    TRY(xrCreateInstance(&ci,&instance));
    XrSystemGetInfo si={.type=XR_TYPE_SYSTEM_GET_INFO,.formFactor=XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    XrSystemId system; TRY(xrGetSystem(instance,&si,&system));
    PFN_xrGetOpenGLGraphicsRequirementsKHR requirements=NULL;
    TRY(xrGetInstanceProcAddr(instance,"xrGetOpenGLGraphicsRequirementsKHR",(PFN_xrVoidFunction*)&requirements));
    XrGraphicsRequirementsOpenGLKHR req={.type=XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    TRY(requirements(instance,system,&req));
    int major=0,minor=0;
    const char *version=(const char*)glGetString(GL_VERSION);
    if(!version || sscanf(version,"%d.%d",&major,&minor)!=2)return 0;
    XrVersion glVersion=XR_MAKE_VERSION(major,minor,0);
    if(glVersion<req.minApiVersionSupported || glVersion>req.maxApiVersionSupported) {
        fprintf(stderr,"[vr] GL %s outside runtime requirements\n",version);return 0;
    }
    XrGraphicsBindingOpenGLXlibKHR binding={.type=XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR,
        .xDisplay=display,.visualid=(uint32_t)visual,.glxFBConfig=config,
        .glxDrawable=glXGetCurrentDrawable(),.glxContext=context};
    XrSessionCreateInfo sci={.type=XR_TYPE_SESSION_CREATE_INFO,.next=&binding,.systemId=system};
    TRY(xrCreateSession(instance,&sci,&session));
    if(!vr_input_init(instance,session,frameExtension))return 0;
    XrReferenceSpaceCreateInfo rs={.type=XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .referenceSpaceType=XR_REFERENCE_SPACE_TYPE_LOCAL,.poseInReferenceSpace.orientation.w=1};
    TRY(xrCreateReferenceSpace(session,&rs,&space));
    uint32_t n=0;
    TRY(xrEnumerateSwapchainFormats(session,0,&n,NULL));
    int64_t *formats=calloc(n,sizeof(*formats)); if(!formats)return 0;
    XrResult result=xrEnumerateSwapchainFormats(session,n,&n,formats);
    int srgb=0;for(uint32_t i=0;i<n;i++)if(formats[i]==GL_SRGB8_ALPHA8)srgb=1;
    free(formats); if(!check(result,"xrEnumerateSwapchainFormats") || !srgb)return 0;
    XrViewConfigurationView vc[2]={{.type=XR_TYPE_VIEW_CONFIGURATION_VIEW},{.type=XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    TRY(xrEnumerateViewConfigurationViews(instance,system,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,2,&n,vc));
    if(n!=2)return 0;
    for(int e=0;e<2;e++) {
        width[e]=vc[e].recommendedImageRectWidth; height[e]=vc[e].recommendedImageRectHeight;
        XrSwapchainCreateInfo sc={.type=XR_TYPE_SWAPCHAIN_CREATE_INFO,
            .usageFlags=XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT|XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
            .format=GL_SRGB8_ALPHA8,.sampleCount=1,.width=width[e],.height=height[e],
            .faceCount=1,.arraySize=1,.mipCount=1};
        TRY(xrCreateSwapchain(session,&sc,&chain[e]));
        TRY(xrEnumerateSwapchainImages(chain[e],0,&n,NULL));
        images[e]=calloc(n,sizeof(*images[e]));if(!images[e])return 0;
        for(uint32_t i=0;i<n;i++)images[e][i].type=XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        TRY(xrEnumerateSwapchainImages(chain[e],n,&n,(XrSwapchainImageBaseHeader*)images[e]));
        fprintf(stderr,"[vr] eye%d sRGB %dx%d images=%u\n",e,width[e],height[e],n);
    }
    fprintf(stderr,"[vr] GL %s renderer=%s\n",version,glGetString(GL_RENDERER));
    return 1;
}
static void events(void) {
    XrEventDataBuffer ev={.type=XR_TYPE_EVENT_DATA_BUFFER};
    XrResult r;
    while((r=xrPollEvent(instance,&ev))==XR_SUCCESS) {
        if(ev.type==XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrSessionState state=((XrEventDataSessionStateChanged*)&ev)->state;
            fprintf(stderr,"[vr] session state=%d\n",state);
            focused=state==XR_SESSION_STATE_FOCUSED;
            if(!focused)vr_input_clear();
            else vr_input_profiles_changed();
            if(state==XR_SESSION_STATE_READY && !running) {
                XrSessionBeginInfo bi={.type=XR_TYPE_SESSION_BEGIN_INFO,.primaryViewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                running=check(xrBeginSession(session,&bi),"xrBeginSession");
            } else if(state==XR_SESSION_STATE_STOPPING && running) {
                check(xrEndSession(session),"xrEndSession");running=0;
            } else if(state==XR_SESSION_STATE_EXITING || state==XR_SESSION_STATE_LOSS_PENDING) quit=1;
        } else if(ev.type==XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED)vr_input_profiles_changed();
        else if(ev.type==XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING){vr_input_clear();quit=1;}
        ev.type=XR_TYPE_EVENT_DATA_BUFFER;
    }
    if(r!=XR_EVENT_UNAVAILABLE)check(r,"xrPollEvent");
}
int vr_xr_begin(VREye eyes[2],int64_t *time) {
    if(begun)vr_xr_end(0); /* A native mode transition may abandon a race draw. */
    while(!quit) {
        platform_pump_events();events();if(quit)return -1;
        uint32_t now=SDL_GetTicks();
        if(runLimit && now-runStart>=runLimit){progress("deadline");quit=1;return -1;}
        if(runLimit && now-lastProgress>=2000){progress("progress");lastProgress=now;}
        if(!running){SDL_Delay(10);continue;}
        XrFrameWaitInfo wi={.type=XR_TYPE_FRAME_WAIT_INFO};
        frame=(XrFrameState){.type=XR_TYPE_FRAME_STATE};
        if(!check(xrWaitFrame(session,&wi,&frame),"xrWaitFrame"))return -1;
        XrFrameBeginInfo bi={.type=XR_TYPE_FRAME_BEGIN_INFO};
        if(!check(xrBeginFrame(session,&bi),"xrBeginFrame"))return -1;
        begun=1;*time=frame.predictedDisplayTime;frameWaits++;
        vr_input_sync(focused);
        if(!frame.shouldRender){standbyFrames++;if(!offheadRender)return 0;}
        XrViewLocateInfo li={.type=XR_TYPE_VIEW_LOCATE_INFO,.viewConfigurationType=XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            .displayTime=frame.predictedDisplayTime,.space=space};
        XrViewState vs={.type=XR_TYPE_VIEW_STATE};uint32_t n=0;
        for(int e=0;e<2;e++)views[e]=(XrView){.type=XR_TYPE_VIEW};
        if(!check(xrLocateViews(session,&li,&vs,2,&n,views),"xrLocateViews"))return -1;
        XrViewStateFlags valid=XR_VIEW_STATE_ORIENTATION_VALID_BIT|XR_VIEW_STATE_POSITION_VALID_BIT;
        int poseValid=n==2 && (vs.viewStateFlags&valid)==valid;
        if(!poseValid){invalidFrames++;if(!offheadRender)return 0;fallbackFrames++;}
        else validFrames++;
        if(offheadRender && (lastPoseKind!=poseValid || frameWaits==1)) {
            fprintf(stderr,"[vr-runtime] offhead shouldRender=%d pose=%s layers=0\n",frame.shouldRender,poseValid?"located":"synthetic-neutral-64mm-fov90x74");
            lastPoseKind=poseValid;
        }
        for(int e=0;e<2;e++) {
            XrPosef p=views[e].pose;XrFovf f=views[e].fov;
            if(poseValid) {
                eyes[e].pose=(VRPose){{p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w},{p.position.x,p.position.y,p.position.z}};
                eyes[e].fov=(VRFov){f.angleLeft,f.angleRight,f.angleUp,f.angleDown};
            } else {
                /* Bounded GPU diagnostic only, never submitted as tracked views. */
                eyes[e].pose=(VRPose){{0,0,0,1},{e?0.032f:-0.032f,0,0}};
                eyes[e].fov=(VRFov){-0.7853981634f,0.7853981634f,0.6435011088f,-0.6435011088f};
            }
            eyes[e].width=width[e];eyes[e].height=height[e];
        }
        return 1;
    }
    return -1;
}
unsigned vr_xr_acquire(int eye) {
    XrSwapchainImageAcquireInfo ai={.type=XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};uint32_t index=0;
    if(!check(xrAcquireSwapchainImage(chain[eye],&ai,&index),"xrAcquireSwapchainImage"))return 0;
    acquired[eye]=1;
    XrSwapchainImageWaitInfo wi={.type=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,.timeout=XR_INFINITE_DURATION};
    if(!check(xrWaitSwapchainImage(chain[eye],&wi),"xrWaitSwapchainImage"))return 0;
    waited[eye]=1;return images[eye][index].image;
}
void vr_xr_release(int eye) {
    if(!acquired[eye])return;
    if(!waited[eye]) {
        XrSwapchainImageWaitInfo wi={.type=XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,.timeout=XR_INFINITE_DURATION};
        if(!check(xrWaitSwapchainImage(chain[eye],&wi),"xrWaitSwapchainImage(cleanup)"))return;
    }
    /* The verified Frame Zink bridge requires completion before release. No
     * CPU readback; revisit this explicit fence only with matched device data. */
    glFlush();glFinish();
    XrSwapchainImageReleaseInfo ri={.type=XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if(check(xrReleaseSwapchainImage(chain[eye],&ri),"xrReleaseSwapchainImage"))acquired[eye]=waited[eye]=0;
}
void vr_xr_end(int rendered) {
    if(!begun)return;
    if(rendered)renderedEyes+=2;
    if(offheadRender)rendered=0; /* Never display diagnostic imagery, even after a focus change. */
    XrCompositionLayerProjectionView pv[2];memset(pv,0,sizeof(pv));
    for(int e=0;e<2;e++) {
        pv[e].type=XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;pv[e].pose=views[e].pose;pv[e].fov=views[e].fov;
        pv[e].subImage.swapchain=chain[e];pv[e].subImage.imageRect.extent=(XrExtent2Di){width[e],height[e]};
    }
    XrCompositionLayerProjection layer={.type=XR_TYPE_COMPOSITION_LAYER_PROJECTION,.space=space,.viewCount=2,.views=pv};
    const XrCompositionLayerBaseHeader *layers[]={(const XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei={.type=XR_TYPE_FRAME_END_INFO,.displayTime=frame.predictedDisplayTime,
        .environmentBlendMode=XR_ENVIRONMENT_BLEND_MODE_OPAQUE,.layerCount=rendered?1:0,.layers=layers};
    if(check(xrEndFrame(session,&ei),"xrEndFrame") && rendered)submittedFrames++;
    begun=0;
}
void vr_xr_shutdown(void) {
    if(runLimit)progress("shutdown");
    for(int e=0;e<2;e++)vr_xr_release(e);
    vr_xr_end(0);
    if(session && running) {
        xrRequestExitSession(session);uint32_t start=SDL_GetTicks();
        while(running && SDL_GetTicks()-start<2000){events();SDL_Delay(10);}
    }
    for(int e=0;e<2;e++){free(images[e]);images[e]=NULL;if(chain[e])xrDestroySwapchain(chain[e]);chain[e]=XR_NULL_HANDLE;}
    if(space)xrDestroySpace(space);if(session)xrDestroySession(session);
    vr_input_shutdown();
    if(instance)xrDestroyInstance(instance);
    space=XR_NULL_HANDLE;session=XR_NULL_HANDLE;instance=XR_NULL_HANDLE;
}
#endif
