#include "vr.h"
#if SONICR_VR
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vr_xr.h"
#include "vr_input.h"
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "platform.h"
#include "r_state.h"
#include "pad_bits.h"
static int initialized,diagnostic,eye=-1,repeat,layer,identity,presented,mirrorDirty;
static int ticks,frames,extraPasses,dumpTick=90,autoRace,drive,runtimeDump;
static int traceDraw,stereoCheck;
static uint64_t drawHash;
static unsigned drawCount;
static float units=80,diagYaw,diagRoll;
static VREye eyes[2];
static VRVec anchor;
static VRQuat anchorRotation={0,0,0,1};
static int anchored,wasFocused,modeRace;
static int64_t focusSince;
static VRClock clockState;
static int64_t displayTime;
static GLuint framebuffer,depth[2],diagTexture[2],menuTexture;
static int depthW[2],depthH[2];
static int previous[50],current[50],eyeCamera[50],haveCamera;
static float roll;
static void die(const char *why);
static const char *dumpDir;
extern int g_glBackingWidth, g_glBackingHeight;
extern void SetViewportFromConfig(int *config);
extern void SetViewportClipRect(int *camera);
static uint64_t hashBytes(uint64_t h,const void *data,size_t size) {
    const unsigned char *p=data;
    for(size_t i=0;i<size;i++)h=(h^p[i])*1099511628211ULL;
    return h;
}
void VR_TraceDraw(const RenderVertex *v,int count,const void *state,size_t size) {
    if(!traceDraw)return;
    drawHash=hashBytes(drawHash,&count,sizeof(count));
    drawHash=hashBytes(drawHash,state,size);
    for(int i=0;i<count;i++) {
        /* Reserved specular is not emitted and some native paths leave it unset. */
        drawHash=hashBytes(drawHash,&v[i],offsetof(RenderVertex,specular));
        drawHash=hashBytes(drawHash,&v[i].u,sizeof(float)*2);
    }
    drawCount++;
}
static void traceStart(void) { traceDraw=1;drawHash=1469598103934665603ULL;drawCount=0; }
static void traceEnd(const char *label) {
    traceDraw=0;fprintf(stderr,"[vr-diag] draw_stream=%s count=%u hash=%016llx\n",label,drawCount,(unsigned long long)drawHash);
}
static void attachment(const char *label,int fbo) {
    GLint encoding=0,component=0,depthBits=0,redBits=0,samples=0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,fbo?GL_COLOR_ATTACHMENT0:GL_BACK_LEFT,GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,&encoding);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,fbo?GL_COLOR_ATTACHMENT0:GL_BACK_LEFT,GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE,&component);
    glGetIntegerv(GL_DEPTH_BITS,&depthBits);glGetIntegerv(GL_RED_BITS,&redBits);glGetIntegerv(GL_SAMPLES,&samples);
    fprintf(stderr,"[vr-diag] attachment=%s encoding=0x%x component=0x%x depth=%d red=%d samples=%d srgb=%d\n",label,encoding,component,depthBits,redBits,samples,glIsEnabled(GL_FRAMEBUFFER_SRGB));
}
static uint64_t simulationFingerprint(void) {
    uint64_t h=1469598103934665603ULL;
    h=hashBytes(h,g_playerBase,sizeof(Player)*g_numPlayers);
    h=hashBytes(h,g_camStateTable,sizeof(CamStateEntry)*4);
    h=hashBytes(h,g_collectEffectBuf,sizeof(CollectEffect)*COLLECT_EFFECT_COUNT);
    h=hashBytes(h,g_precipParticles,sizeof(PrecipParticle)*128);
    h=hashBytes(h,&g_totalFrames,sizeof(g_totalFrames));
    h=hashBytes(h,&g_randomRingIdx,sizeof(g_randomRingIdx));
    return h;
}
static void repeatRender(void) {
    uint64_t before=diagnostic?simulationFingerprint():0;
    RenderHUD();
    if(diagnostic && before!=simulationFingerprint())die("repeat render mutated simulation state");
}
static void die(const char *why) { fprintf(stderr,"[vr] fatal: %s\n",why);exit(2); }
static float setting(const char *name,float fallback,float lo,float hi) {
    const char *s=getenv(name);if(!s)return fallback;
    char *end;float v=strtof(s,&end);
    if(end==s || *end || !isfinite(v) || v<lo || v>hi)die(name);
    return v;
}
static void capture(const char *name,int w,int h) {
    char path[1024];snprintf(path,sizeof(path),"%s/%s.ppm",dumpDir,name);
    unsigned char *pixels=malloc((size_t)w*h*3);if(!pixels)die("capture allocation");
    glPixelStorei(GL_PACK_ALIGNMENT,1);glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,pixels);
    if(glGetError()!=GL_NO_ERROR)die("capture GL readback");
    FILE *f=fopen(path,"wb");if(!f)die("capture directory must exist and be writable");
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(int y=h-1;y>=0;y--)if(fwrite(pixels+(size_t)y*w*3,3,w,f)!=(size_t)w)die("capture write");
    fclose(f);free(pixels);fprintf(stderr,"[vr-diag] capture=%s size=%dx%d\n",path,w,h);
}
/* Opt-in observer: full-resolution, synchronous and deliberately excluded
 * from normal rendering/performance runs. A second unchanged read validates
 * the observer before comparing repeated production scene passes. */
static uint64_t imageFingerprint(int e) {
    size_t size=(size_t)eyes[e].width*eyes[e].height*3;
    unsigned char *pixels=malloc(size);if(!pixels)die("stereo check allocation");
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,eyes[e].width,eyes[e].height,GL_RGB,GL_UNSIGNED_BYTE,pixels);
    if(glGetError()!=GL_NO_ERROR)die("stereo check GL readback");
    uint64_t hash=hashBytes(1469598103934665603ULL,pixels,size);
    free(pixels);return hash;
}
int VR_Diagnostic(void) { return diagnostic; }
unsigned short VR_PadBits(void) { return vr_input_read(g_joystickConfigWords); }
int VR_AutoRace(void) { return diagnostic || autoRace; }
void VR_DiagnosticInput(void) { if(diagnostic && drive)g_perPlayerInput[0]|=PAD_ACCEL; }
int VR_EyeActive(void) { return eye>=0 && !identity; }
int VR_Repeat(void) { return repeat; }
void VR_Layer(int value) { layer=value; }
void VR_Viewport(int *w,int *h) { if(eye>=0){*w=eyes[eye].width;*h=eyes[eye].height;} }
int VR_Init(void) {
    diagnostic=getenv("SONICR_VR_DIAGNOSTIC") && strcmp(getenv("SONICR_VR_DIAGNOSTIC"),"1")==0;
    stereoCheck=diagnostic && getenv("SONICR_VR_DIAG_STEREO_CHECK") && strcmp(getenv("SONICR_VR_DIAG_STEREO_CHECK"),"1")==0;
    autoRace=getenv("SONICR_VR_AUTORACE") && strcmp(getenv("SONICR_VR_AUTORACE"),"1")==0;
    drive=diagnostic && getenv("SONICR_VR_DIAG_DRIVE") && strcmp(getenv("SONICR_VR_DIAG_DRIVE"),"1")==0;
    runtimeDump=!diagnostic && getenv("SONICR_VR_DUMP_TICK") && getenv("SONICR_VR_DUMP_DIR");
    unsigned runSeconds=(unsigned)setting("SONICR_VR_RUN_SECONDS",0,1,300);
    int offhead=getenv("SONICR_VR_OFFHEAD_RENDER") && strcmp(getenv("SONICR_VR_OFFHEAD_RENDER"),"1")==0;
    if(offhead && (diagnostic || !autoRace || !runSeconds))die("OFFHEAD_RENDER requires runtime AUTORACE=1 and RUN_SECONDS=1..300");
    vr_xr_deadline(runSeconds,offhead);
    units=setting("SONICR_VR_WORLD_SCALE",80,8,800);
    diagYaw=setting("SONICR_VR_DIAG_YAW",0,-360,360)*0.01745329251994f;
    diagRoll=setting("SONICR_VR_DIAG_ROLL",0,-90,90)*0.01745329251994f;
    dumpTick=(int)setting("SONICR_VR_DUMP_TICK",90,1,900);
    dumpDir=getenv("SONICR_VR_DUMP_DIR");if(!dumpDir)dumpDir=".";
    if(!diagnostic && !vr_xr_init()){vr_xr_shutdown();return 0;}
    glGenFramebuffers(1,&framebuffer);glGenRenderbuffers(2,depth);glGenTextures(1,&menuTexture);
    if(diagnostic) {
        glGenTextures(2,diagTexture);
        for(int e=0;e<2;e++) {
            eyes[e].width=640;eyes[e].height=480;
            glBindTexture(GL_TEXTURE_2D,diagTexture[e]);
            glTexImage2D(GL_TEXTURE_2D,0,GL_SRGB8_ALPHA8,640,480,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        }
        fprintf(stderr,"[vr-diag] synthetic yaw=%.1f tick=%d output=%s; OpenXR disabled\n",diagYaw/0.01745329251994f,dumpTick,dumpDir);
    }
    SDL_GL_SetSwapInterval(0);initialized=1;atexit(VR_Shutdown);
    return 1;
}
void VR_Shutdown(void) {
    if(!initialized)return;
    initialized=0;
    fprintf(stderr,"[vr] shutdown race_ticks=%d race_display_frames=%d stereo_passes=%d diagnostic=%d\n",ticks,frames,frames*2+extraPasses,diagnostic);
    if(!diagnostic)vr_xr_shutdown();
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    glDeleteFramebuffers(1,&framebuffer);glDeleteRenderbuffers(2,depth);
    glDeleteTextures(2,diagTexture);glDeleteTextures(1,&menuTexture);
}
static int begin(void) {
    int visible;
    if(diagnostic) {
        displayTime+=1000000000LL/90;
        float yaw=diagYaw+(stereoCheck?0.02f*sinf(frames/15.0f):0);
        VRQuat q=vr_mul((VRQuat){0,sinf(yaw/2),0,cosf(yaw/2)},(VRQuat){0,0,sinf(diagRoll/2),cosf(diagRoll/2)});
        for(int e=0;e<2;e++) {
            eyes[e].pose=(VRPose){q,vr_rotate(q,(VRVec){e?0.032f:-0.032f,0,0})};
            eyes[e].fov=(VRFov){-0.7853981634f,0.7853981634f,0.6435011088f,-0.6435011088f};
            if(stereoCheck)eyes[e].fov=(VRFov){atanf(e?-0.8f:-1.2f),atanf(e?1.2f:0.8f),atanf(0.85f),atanf(-0.65f)};
        }
        visible=1;
    } else visible=vr_xr_begin(eyes,&displayTime);
    int focused=diagnostic || vr_xr_focused();
    if(focused && !wasFocused){anchored=0;haveCamera=0;focusSince=0;}
    wasFocused=focused;
    if(!focused)focusSince=0;
    int ready=diagnostic || vr_xr_offhead() || vr_anchor_ready(focused,displayTime,&focusSince);
    if(visible==1 && !anchored && ready) {
        anchor=vr_scale(vr_add(eyes[0].pose.p,eyes[1].pose.p),0.5f);anchored=1;
        if(!diagnostic) {
            VRVec forward=vr_rotate(eyes[0].pose.q,(VRVec){0,0,-1});
            float yaw=atan2f(-forward.x,-forward.z);
            anchorRotation=(VRQuat){0,-sinf(yaw/2),0,cosf(yaw/2)};
        }
    }
    /* During focus settling, submit empty frames instead of showing a scene
     * anchored to Steam's launch-screen heading. Simulation remains frozen. */
    if(!diagnostic && !anchored && visible==1)return 0;
    return visible;
}
void VR_RaceStart(void) {
    clockState=(VRClock){0};haveCamera=0;ticks=frames=extraPasses=0;presented=0;
    modeRace=1;anchored=0;focusSince=0;
    if(VR_AutoRace()) {
        /* Start at the actual race camera; the deterministic test needs neither
         * title/demo timers nor the flyover spline. Game data is read normally. */
        g_introCountdown=0;g_introTimer=0;g_fadeLevel=0;g_fadeState=FADE_VISIBLE;
        fprintf(stderr,"[vr] autorace ready track=%d players=%d synthetic=%d drive=%d\n",g_trackId,g_numPlayers,diagnostic,drive);
    }
}
int VR_RaceFrame(void) {
    for(;;) {
        int visible=begin();if(visible<0)return -1;
        int tick=vr_clock_step(&clockState,displayTime,visible);
        if(clockState.cut)haveCamera=0;
        if(!visible){vr_xr_end(0);continue;}
        frames++;
        if(tick)vr_input_tick();
        return tick;
    }
}
static void target(int e) {
    eye=e;R_ResetState();
    GLuint color=diagnostic?diagTexture[e]:vr_xr_acquire(e);
    if(!color)die("eye acquisition");
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
    glBindRenderbuffer(GL_RENDERBUFFER,depth[e]);
    if(depthW[e]!=eyes[e].width || depthH[e]!=eyes[e].height) {
        depthW[e]=eyes[e].width;depthH[e]=eyes[e].height;
        glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,depthW[e],depthH[e]);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth[e]);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);glReadBuffer(GL_COLOR_ATTACHMENT0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)die("eye framebuffer incomplete");
    glDisable(GL_FRAMEBUFFER_SRGB);
    glViewport(0,0,eyes[e].width,eyes[e].height);glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
}
static void release(void) {
    glBindFramebuffer(GL_FRAMEBUFFER,0);if(!diagnostic)vr_xr_release(eye);eye=-1;
    glDrawBuffer(GL_BACK);glReadBuffer(GL_BACK);
}
static int cameraSlot(void) {
    return (g_isNetworkGame || g_netSessionActive)?(unsigned short)g_localPlayerIndex:0;
}
void VR_RenderRace(int tick) {
    int *native=(int*)((char*)g_viewportConfigArray+cameraSlot()*0xC8);
    if(tick) {
        memcpy(previous,current,sizeof(current));memcpy(current,native,sizeof(current));
        if(!haveCamera)memcpy(previous,current,sizeof(current));haveCamera=1;
        repeat=0;layer=0;eye=-1;
        /* Canonical pass owns all game-facing render side effects exactly once
         * per tick (weather visibility, wrong-way warning). Also desktop mirror. */
        RenderHUD();ticks++;mirrorDirty=1;
        if((diagnostic || runtimeDump || autoRace) && (ticks==1 || ticks%30==0))fprintf(stderr,"[vr-diag] tick=%d display=%d player=(%d,%d,%d) camera=(%d,%d,%d)\n",ticks,frames,g_playerBase[0].posX,g_playerBase[0].posY,g_playerBase[0].posZ,native[3],native[4],native[5]);
    }
    int dump=(diagnostic || runtimeDump) && tick && ticks==dumpTick;
    if(dump && diagnostic) {
        capture("flat",640,480);
        repeat=1;traceStart();repeatRender();traceEnd("flat-repeat");capture("flat-repeat",640,480);
        attachment("desktop",0);
        /* Exact current native camera and projection, same renderer. Identity
         * bypasses all presentation remapping, exposing accidental global/state
         * contamination independently of the real stereo captures below. */
        repeat=1;identity=1;target(0);attachment("identity-srgb",1);
        traceStart();repeatRender();traceEnd("identity-srgb");capture("identity",640,480);release();
        /* Isolate framebuffer colour format from the native draw stream. */
        glBindTexture(GL_TEXTURE_2D,diagTexture[0]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,640,480,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        target(0);attachment("identity-linear",1);
        traceStart();repeatRender();traceEnd("identity-linear");capture("identity-linear",640,480);release();
        glBindTexture(GL_TEXTURE_2D,diagTexture[0]);
        glTexImage2D(GL_TEXTURE_2D,0,GL_SRGB8_ALPHA8,640,480,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
        identity=0;
    }
    repeat=1;
    int checkStereo=stereoCheck && ticks>=dumpTick-3;
    uint64_t images[2]={0},streams[2]={0};unsigned counts[2]={0};
    for(int e=0;e<2;e++) {
        target(e);layer=0;if(checkStereo)traceStart();repeatRender();
        if(checkStereo) {
            traceDraw=0;streams[e]=drawHash;counts[e]=drawCount;
            images[e]=imageFingerprint(e);
            if(images[e]!=imageFingerprint(e))die("stereo observer changed without a draw");
            char name[64];snprintf(name,sizeof(name),"frame%04d-eye%d",frames,e);
            capture(name,eyes[e].width,eyes[e].height);
        }
        if(dump)capture(e?"eye1":"eye0",eyes[e].width,eyes[e].height);
        release();
    }
    if(checkStereo) {
        /* Reverse the eye order, then repeat it normally, without a native
         * update or pose change. All view scratch must be rebuilt each time. */
        const int order[4]={1,0,0,1};
        for(int i=0;i<4;i++) {
            int e=order[i];target(e);layer=0;traceStart();repeatRender();traceDraw=0;extraPasses++;
            if(streams[e]!=drawHash || counts[e]!=drawCount)die("eye order changed draw stream");
            if(images[e]!=imageFingerprint(e)) {
                capture("stereo-repeat-failure",eyes[e].width,eyes[e].height);
                die("eye order changed image");
            }
            release();
        }
        fprintf(stderr,"[vr-diag] stereo_check frame=%d tick=%d repeats=4 observer=stable draw_stream=exact image=exact asymmetric=1\n",frames,ticks);
    }
    repeat=0;layer=0;
    /* Restore the canonical camera/projection before the next simulation tick. */
    g_currentRenderCam=native;SetViewportFromConfig(g_viewportArray+cameraSlot()*21);SetViewportClipRect(native);
    if(!diagnostic)vr_xr_end(1);
    presented=1;
    if(dump) {
        fprintf(stderr,"[vr-diag] complete ticks=%d display_frames=%d stereo_passes=%d yaw=%.1f roll=%.1f units_per_metre=%.3f synthetic=%d repeat_state=%s\n",ticks,frames,frames*2+extraPasses,diagYaw/0.01745329251994f,diagRoll/0.01745329251994f,units,diagnostic,diagnostic?"unchanged":"unchecked");
        exit(0);
    }
}
void VR_ApplyCamera(int **camera) {
    if(eye<0 || identity)return;
    memcpy(eyeCamera,current,sizeof(eyeCamera));
    RenderCamera *rc=(RenderCamera*)eyeCamera,*a=(RenderCamera*)previous,*b=(RenderCamera*)current;
    float t=clockState.alpha;
    if(diagnostic && eye==0 && (ticks==1 || ticks%30==0))
        fprintf(stderr,"[vr-diag] presentation tick=%d frame=%d alpha=%.6f chase=(%.3f,%.3f,%.3f) previous=(%.3f,%.3f,%.3f) current=(%.3f,%.3f,%.3f)\n",
            ticks,frames,t,a->camFloatX+(b->camFloatX-a->camFloatX)*t,a->camFloatY+(b->camFloatY-a->camFloatY)*t,a->camFloatZ+(b->camFloatZ-a->camFloatZ)*t,
            a->camFloatX,a->camFloatY,a->camFloatZ,b->camFloatX,b->camFloatY,b->camFloatZ);
    /* Native chase damping remains tick-owned; interpolate position and the
     * shortest angle arc, rebuild the rigid basis, then add unfiltered tracking. */
    rc->camFloatX=a->camFloatX+(b->camFloatX-a->camFloatX)*t;
    rc->camFloatY=a->camFloatY+(b->camFloatY-a->camFloatY)*t;
    rc->camFloatZ=a->camFloatZ+(b->camFloatZ-a->camFloatZ)*t;
    float p=vr_angle(a->yawFloat,b->yawFloat,t),y=vr_angle(a->pitchFloat,b->pitchFloat,t);
    float sp=sinf(p),cp=cosf(p),sy=sinf(y),cy=cosf(y);
    float base[9]={cy,sy*sp,-sy*cp, 0,cp,sp, sy,-sp*cy,cp*cy};
    if(g_mirrorMode){base[0]=-base[0];base[6]=-base[6];}
    VRVec center=vr_scale(vr_add(eyes[0].pose.p,eyes[1].pose.p),0.5f);
    VRVec lean=vr_sub(center,anchor);
    float distance=sqrtf(lean.x*lean.x+lean.y*lean.y+lean.z*lean.z);
    if(distance>0.35f)lean=vr_scale(lean,0.35f/distance);
    VRVec relative=vr_rotate(anchorRotation,vr_add(lean,vr_sub(eyes[eye].pose.p,center)));
    VRVec offset=vr_game_offset(base,relative,units);
    rc->camFloatX+=offset.x;rc->camFloatY+=offset.y;rc->camFloatZ+=offset.z;
    rc->intX=(int)lrintf(rc->camFloatX);rc->intY=(int)lrintf(rc->camFloatY);rc->intZ=(int)lrintf(rc->camFloatZ);
    rc->worldX=(int)(rc->camFloatX*4096);rc->worldY=(int)(rc->camFloatY*4096);rc->worldZ=(int)(rc->camFloatZ*4096);
    float headBasis[9],composed[9];
    vr_compose_basis(base,anchorRotation,headBasis);
    vr_compose_basis(headBasis,eyes[eye].pose.q,composed);
    int *im=eyeCamera+0x12;float *fm=(float*)(eyeCamera+0x22);
    for(int r=0;r<3;r++)for(int c=0;c<3;c++){fm[r*4+c]=composed[r*3+c];im[r*4+c]=(int)lrintf(composed[r*3+c]*4096);}
    /* Sky scroll uses historical swapped pitch/yaw fields; derive from the
     * complete eye basis. World geometry retains all three rotation axes. */
    rc->yawFloat=asinf(fmaxf(-1,fminf(1,composed[5])));
    rc->pitchFloat=atan2f(-composed[2],composed[8]);
    rc->yaw=(int)(rc->yawFloat*651.8986469f)&4095;
    rc->pitch=(int)(rc->pitchFloat*651.8986469f)&4095;
    roll=atan2f(composed[3],composed[4]);
    *camera=eyeCamera;
    VRFov f=eyes[eye].fov;
    float l=tanf(f.left),r=tanf(f.right),u=tanf(f.up),d=tanf(f.down);
    /* Conservative integer CPU frustum; final backend maps this projection
     * exactly to the runtime's floating asymmetric FOV. */
    g_projScaleXCurrent=g_projScaleX=(int)floorf(640/(r-l));
    g_projScaleY=(int)floorf(480/(u-d));
    g_screenCenterX=(int)lrintf(-l*640/(r-l));g_screenCenterY=(int)lrintf(u*480/(u-d));
    g_clipLeft=0;g_clipTop=0;g_clipRight=639;g_clipBottom=479;
}
void VR_SkyBegin(void) { layer=2; }
void VR_SkyEnd(void) { layer=0; }
void VR_EmitVertex(const RenderVertex *v) {
    float w=v->rhw>0?1/v->rhw:1,x=v->sx,y=v->sy,z=v->sz;
    if(!identity && layer==0) {
        VRVec p={(x-g_screenCenterX)*w/g_projScaleXCurrent,(g_screenCenterY-y)*w/g_projScaleY,-w};
        float nx,ny;vr_project(p,eyes[eye].fov,&nx,&ny);x=(nx+1)*320;y=(1-ny)*240;
    } else if(!identity && layer==1) {
        /* Existing UI on a 1.6 x 1.2 m head-relative plane at 2 m. Eye
         * separation remains runtime supplied; no flattening to both eyes. */
        VRVec center=vr_scale(vr_add(eyes[0].pose.p,eyes[1].pose.p),0.5f);
        VRVec p={(x-320)/400,(240-y)/400,-2};
        p=vr_sub(vr_add(center,vr_rotate(vr_without_roll(eyes[0].pose.q),p)),eyes[eye].pose.p);
        p=vr_rotate(vr_inverse(eyes[eye].pose.q),p);
        float nx,ny;vr_project(p,eyes[eye].fov,&nx,&ny);x=(nx+1)*320;y=(1-ny)*240;w=-p.z;z=0;
    } else if(!identity && layer==2) {
        /* Native panorama already uses eye yaw/pitch. Rotate its angular
         * plane for roll, then use the same exact asymmetric FOV correction
         * as world geometry. No eye translation/parallax at infinity. */
        float dx=(x-g_screenCenterX)/g_projScaleXCurrent;
        float dy=(g_screenCenterY-y)/g_projScaleY;
        VRVec direction={cosf(roll)*dx+sinf(roll)*dy,
                         -sinf(roll)*dx+cosf(roll)*dy,-1};
        float nx,ny;vr_project(direction,eyes[eye].fov,&nx,&ny);
        x=(nx+1)*320;y=(1-ny)*240;
    }
    glVertex4f(x*w,y*w,z*w,w);
}
int VR_Present(void) {
    if(!initialized)return 0;
    if(presented){presented=0;if(mirrorDirty){platform_gl_swap();mirrorDirty=0;}return 1;}
    /* All existing menu/logo/results pixels are preserved in one finite plane.
     * Refresh it at native cadence, but locate/render it at XR display cadence. */
    haveCamera=0;
    if(modeRace){modeRace=0;anchored=0;focusSince=0;clockState=(VRClock){0};}
    /* BeginFrame already calculated the native 4:3 viewport. Exclude the
     * drawable's letterbox/pillarbox bars when copying the menu panel. */
    int w=g_glBackingWidth,h=g_glBackingHeight;
    glBindTexture(GL_TEXTURE_2D,menuTexture);
    glCopyTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,g_glViewportOffsetX,g_glViewportOffsetY,w,h,0);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    int due=0;
    do {
        int visible=begin();if(visible<0)exit(0);
        due=vr_clock_step(&clockState,displayTime,visible);
        if(!visible){vr_xr_end(0);continue;}
        for(int e=0;e<2;e++) {
            target(e);BeginFrame();R_SetDepthTest(0);R_SetDepthWrite(0);R_SetTexture(-1);R_FlushState();
            glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,menuTexture);glColor4f(1,1,1,1);layer=1;
            const float xy[4][2]={{0,480},{640,480},{640,0},{0,0}};
            const float uv[4][2]={{0,0},{1,0},{1,1},{0,1}};
            glBegin(GL_QUADS);
            for(int i=0;i<4;i++){RenderVertex v={0};v.sx=xy[i][0];v.sy=xy[i][1];v.rhw=1;glTexCoord2fv(uv[i]);VR_EmitVertex(&v);}
            glEnd();release();
        }
        if(!diagnostic)vr_xr_end(1);
    } while(!due);
    vr_input_tick();
    layer=0;R_ResetState();return 1;
}
#endif
