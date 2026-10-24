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
#include "sonicr_paths.h"
#include "fileio.h"
#ifndef GL_ZERO_TO_ONE
#define GL_ZERO_TO_ONE 0x935F
#define GL_NEGATIVE_ONE_TO_ONE 0x935E
#endif
static int initialized,diagnostic,eye=-1,repeat,layer,identity,presented,mirrorDirty;
static int ticks,frames,extraPasses,dumpTick=90,autoRace,drive,runtimeDump;
static int traceDraw,stereoCheck;
static int fixtureReplay;
static unsigned short *replayPads;
static unsigned replayCount,replayIndex;
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
static float roll,rollCos=1,rollSin;
static VRProjection eyeProjection;
static VRVec hudCenter;
static VRQuat hudOrientation,eyeInverse;
/* Refresh on every target, including menu and diagnostic/reversed repeats.
 * Camera-derived roll is refreshed separately by VR_ApplyCamera. */
static void cacheEyeProjection(void) {
    eyeProjection=vr_projection(eyes[eye].fov);
    hudCenter=vr_scale(vr_add(eyes[0].pose.p,eyes[1].pose.p),0.5f);
    hudOrientation=vr_without_roll(eyes[0].pose.q);
    eyeInverse=vr_inverse(eyes[eye].pose.q);
}
static void die(const char *why);
static const char *dumpDir;
extern int g_glBackingWidth, g_glBackingHeight;
extern void SetViewportFromConfig(int *config);
extern void SetViewportClipRect(int *camera);
/* Opt-in CPU wall-time attribution. No GL queries, readbacks or added waits.
 * Draw time is command submission; release includes the existing GL completion
 * bridge. Nested weather time is included in the corresponding render span. */
static int perfEnabled,perfWeatherActive;
static double perfWindow,perfLastFrame,perfSimStart,perfWeatherStart,perfMaxInterval;
static double perfMs[8],perfWeatherMs[3];
static unsigned perfFrames,perfTicks,perfDraws[3],perfWeatherDraws[3],perfSubmits[3],perfVertices[3];
static int batching=1;
static int pairedComplete;
static int perspectiveDepth;
static float depthNear;
static int depthReversed;
static GLenum depthFormat[2];
static void (*clipControl)(GLenum,GLenum);
static void setDepthMode(int enabled);
static double perfNow(void) {
    return perfEnabled ? (double)SDL_GetPerformanceCounter()*1000.0/SDL_GetPerformanceFrequency() : 0;
}
static void perfAdd(int stage,double start) { if(perfEnabled)perfMs[stage]+=perfNow()-start; }
void VR_PerfWeather(int start) {
    if(!perfEnabled)return;
    int slot=eye<0?0:eye+1;
    if(start){perfWeatherActive=1;perfWeatherStart=perfNow();}
    else {perfWeatherMs[slot]+=perfNow()-perfWeatherStart;perfWeatherActive=0;}
}
static void perfReport(void) {
    if(!perfEnabled)return;
    double now=perfNow(),elapsed=now-perfWindow;
    if(elapsed<1000 || !perfFrames)return;
    fprintf(stderr,"[vr-perf] track=%d weather=%d tod=%d frames=%u ticks=%u wall_ms=%.3f fps=%.2f tick_hz=%.2f max_interval_ms=%.3f begin_ms=%.3f sim_ms=%.3f native_ms=%.3f acquire_ms=%.3f eyes_ms=%.3f release_ms=%.3f end_ms=%.3f mirror_ms=%.3f draws=%u,%u,%u weather_draws=%u,%u,%u gl_submits=%u,%u,%u gl_vertices=%u,%u,%u batch=%d paired_complete=%d fixture_replay=%d replay_pad=%u weather_ms=%.3f,%.3f,%.3f synthetic=%d observer=%d\n",
        g_trackId,g_weatherType,g_timeOfDay,perfFrames,perfTicks,elapsed,perfFrames*1000/elapsed,perfTicks*1000/elapsed,perfMaxInterval,
        perfMs[0]/perfFrames,perfMs[1]/perfFrames,perfMs[2]/perfFrames,perfMs[3]/perfFrames,perfMs[4]/perfFrames,perfMs[5]/perfFrames,perfMs[6]/perfFrames,perfMs[7]/perfFrames,
        perfDraws[0],perfDraws[1],perfDraws[2],perfWeatherDraws[0],perfWeatherDraws[1],perfWeatherDraws[2],
        perfSubmits[0],perfSubmits[1],perfSubmits[2],perfVertices[0],perfVertices[1],perfVertices[2],batching,pairedComplete,fixtureReplay,replayIndex,
        perfWeatherMs[0]/perfFrames,perfWeatherMs[1]/perfFrames,perfWeatherMs[2]/perfFrames,diagnostic,diagnostic || runtimeDump || stereoCheck);
    perfWindow=now;perfFrames=perfTicks=0;perfMaxInterval=0;
    memset(perfMs,0,sizeof perfMs);memset(perfWeatherMs,0,sizeof perfWeatherMs);
    memset(perfDraws,0,sizeof perfDraws);memset(perfWeatherDraws,0,sizeof perfWeatherDraws);
    memset(perfSubmits,0,sizeof perfSubmits);memset(perfVertices,0,sizeof perfVertices);
}
static uint64_t hashBytes(uint64_t h,const void *data,size_t size) {
    const unsigned char *p=data;
    for(size_t i=0;i<size;i++)h=(h^p[i])*1099511628211ULL;
    return h;
}
void VR_TraceBatch(unsigned vertices) {
    if(perfEnabled && modeRace){int slot=eye<0?0:eye+1;perfSubmits[slot]++;perfVertices[slot]+=vertices;}
}
void VR_TraceDraw(const RenderVertex *v,int count,const void *state,size_t size) {
    if(perfEnabled && modeRace){int slot=eye<0?0:eye+1;perfDraws[slot]++;if(perfWeatherActive)perfWeatherDraws[slot]++;}
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
    R_EndBatch();
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
    /* Ground/water particles have render-time native culling; eye repeats
     * must not consume their lifetime or remove them for the other eye. */
    extern int g_renderStateBlock[16];
    extern GroundParticle g_particleSlots[16];
    h=hashBytes(h,g_renderStateBlock,sizeof(g_renderStateBlock));
    h=hashBytes(h,g_particleSlots,sizeof(g_particleSlots));
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
    R_EndBatch();
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
    R_EndBatch();
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
static void resetReplay(void) {
    free(replayPads);replayPads=NULL;replayCount=replayIndex=0;
}
static int readReplay(FILE *file) {
    /* Native Island DEM has fifteen LE16 header words, then one P1 stream.
     * Keep a private copy: normal race input overwrites g_taGhostSource. */
    static const unsigned short header[14]={1,5,1,1,0,0,1,0,1,0,1,2,3,4};
    unsigned char bytes[30];resetReplay();
    if(fRead(bytes,1,sizeof bytes,file)!=sizeof bytes)return 0;
    for(int i=0;i<14;i++)if((unsigned)(bytes[i*2]|bytes[i*2+1]<<8)!=header[i])return 0;
    unsigned count=bytes[28]|bytes[29]<<8;
    if(!count || count>0x8000)return 0;
    unsigned short *pads=malloc(count*sizeof *pads);if(!pads)return 0;
    for(unsigned i=0;i<count;i++) {
        unsigned char word[2];
        if(fRead(word,1,2,file)!=2){free(pads);return 0;}
        pads[i]=(unsigned short)(word[0]|word[1]<<8);
    }
    if(fgetc(file)!=EOF || fError(file)){free(pads);return 0;}
    replayPads=pads;replayCount=count;return 1;
}
void VR_DiagnosticInput(void) {
    if(fixtureReplay) {
        unsigned short pad=0;
        if(!g_isPaused && g_introCountdown<61 && !g_postRaceCameraMode && replayIndex<replayCount) {
            pad=replayPads[replayIndex++];
            if(replayIndex==replayCount)fprintf(stderr,"[vr] fixture_replay exhausted pads=%u tick=%d\n",replayCount,ticks);
        }
        /* Only gameplay actions; the fixture cannot inject pause/menu input.
         * Intro masking matches normal native recording at countdown60..1. */
        pad&=PAD_DIRECTIONS|PAD_ACCEL|PAD_ABILITY|PAD_DRIFTL|PAD_DRIFTR|PAD_CAMERA;
        if(g_introCountdown>0)pad&=0x3957;
        g_perPlayerInput[0]=pad;g_combinedInputState=pad;g_inputBits=0;
        return;
    }
    if(diagnostic && drive)g_perPlayerInput[0]|=PAD_ACCEL;
}
int VR_DepthReversed(void) { return depthReversed; }
int VR_EyeActive(void) { return eye>=0 && !identity; }
int VR_Repeat(void) { return repeat; }
void VR_Layer(int value) { if(layer!=value)R_EndBatch();layer=value; }
void VR_Viewport(int *w,int *h) { if(eye>=0){*w=eyes[eye].width;*h=eyes[eye].height;} }
void VR_ConfigureAutoRace(void) {
    if(!VR_AutoRace())return;
    const char *weather=getenv("SONICR_VR_FIXTURE_WEATHER");
    const char *track=getenv("SONICR_VR_FIXTURE_TRACK");
    const char *time=getenv("SONICR_VR_FIXTURE_TIME");
    if(!weather && !track && !time && !fixtureReplay)return;
    /* Native LoadTrackSinglePlayer selects weather and cycles time before
     * texture/particle initialization. Time attack would suppress precipitation. */
    g_raceType=RACE_TYPE_GP;g_numPlayers=MAX_PLAYERS;
    for(int i=0;i<MAX_PLAYERS;i++)g_playerBase[i].charId=(short)i;
    Srand(0x4BEE);
    g_trackId=(int)setting("SONICR_VR_FIXTURE_TRACK",1,1,5);
    if(fixtureReplay && g_trackId!=TRACK_RESORT_ISLAND)die("FIXTURE_REPLAY requires Resort Island track1");
    g_weatherConfig=1+(int)setting("SONICR_VR_FIXTURE_WEATHER",0,0,2);
    g_timeOfDay=(int)setting("SONICR_VR_FIXTURE_TIME",0,0,3)-1;
    fprintf(stderr,"[vr] fixture track=%d weather_config=%d requested_tod=%d race_type=%d\n",g_trackId,g_weatherConfig,g_timeOfDay+1,g_raceType);
}
int VR_Init(void) {
    perspectiveDepth=!(getenv("SONICR_VR_DEPTH") && !strcmp(getenv("SONICR_VR_DEPTH"),"0"));
    fprintf(stderr,"[vr] perspective_world_depth=%d (SONICR_VR_DEPTH=0 disables)\n",perspectiveDepth);
    pairedComplete=!(getenv("SONICR_VR_PAIRED_COMPLETE") && !strcmp(getenv("SONICR_VR_PAIRED_COMPLETE"),"0"));
    fprintf(stderr,"[vr] paired_race_completion=%d (SONICR_VR_PAIRED_COMPLETE=0 disables)\n",pairedComplete);
    batching=!(getenv("SONICR_VR_BATCH") && !strcmp(getenv("SONICR_VR_BATCH"),"0"));
    R_SetVRBatching(batching);
    fprintf(stderr,"[vr] adjacent_eye_batching=%d (SONICR_VR_BATCH=0 disables)\n",batching);
    perfEnabled=getenv("SONICR_VR_PERF") && !strcmp(getenv("SONICR_VR_PERF"),"1");
    diagnostic=getenv("SONICR_VR_DIAGNOSTIC") && strcmp(getenv("SONICR_VR_DIAGNOSTIC"),"1")==0;
    stereoCheck=diagnostic && getenv("SONICR_VR_DIAG_STEREO_CHECK") && strcmp(getenv("SONICR_VR_DIAG_STEREO_CHECK"),"1")==0;
    autoRace=getenv("SONICR_VR_AUTORACE") && strcmp(getenv("SONICR_VR_AUTORACE"),"1")==0;
    drive=diagnostic && getenv("SONICR_VR_DIAG_DRIVE") && strcmp(getenv("SONICR_VR_DIAG_DRIVE"),"1")==0;
    runtimeDump=!diagnostic && getenv("SONICR_VR_DUMP_TICK") && getenv("SONICR_VR_DUMP_DIR");
    unsigned runSeconds=(unsigned)setting("SONICR_VR_RUN_SECONDS",0,1,300);
    int offhead=getenv("SONICR_VR_OFFHEAD_RENDER") && strcmp(getenv("SONICR_VR_OFFHEAD_RENDER"),"1")==0;
    if(offhead && (diagnostic || !autoRace || !runSeconds))die("OFFHEAD_RENDER requires runtime AUTORACE=1 and RUN_SECONDS=1..300");
    int neutral=getenv("SONICR_VR_OFFHEAD_NEUTRAL") && !strcmp(getenv("SONICR_VR_OFFHEAD_NEUTRAL"),"1");
    if(neutral && !offhead)die("OFFHEAD_NEUTRAL requires bounded runtime OFFHEAD_RENDER");
    fixtureReplay=getenv("SONICR_VR_FIXTURE_REPLAY") && !strcmp(getenv("SONICR_VR_FIXTURE_REPLAY"),"1");
    if(fixtureReplay && !offhead)die("FIXTURE_REPLAY requires bounded runtime OFFHEAD_RENDER and AUTORACE");
    vr_xr_deadline(runSeconds,offhead,neutral);
    int runtimeFov=getenv("SONICR_VR_OFFHEAD_RUNTIME_FOV") && !strcmp(getenv("SONICR_VR_OFFHEAD_RUNTIME_FOV"),"1");
    if(!vr_xr_runtime_fov(runtimeFov))die("OFFHEAD_RUNTIME_FOV requires bounded OFFHEAD_NEUTRAL");
    units=setting("SONICR_VR_WORLD_SCALE",80,8,800);
    diagYaw=setting("SONICR_VR_DIAG_YAW",0,-360,360)*0.01745329251994f;
    diagRoll=setting("SONICR_VR_DIAG_ROLL",0,-90,90)*0.01745329251994f;
    dumpTick=(int)setting("SONICR_VR_DUMP_TICK",90,1,900);
    dumpDir=getenv("SONICR_VR_DUMP_DIR");if(!dumpDir)dumpDir=".";
    if(!diagnostic && !vr_xr_init()){vr_xr_shutdown();return 0;}
    if(perspectiveDepth) {
        clipControl=(void (*)(GLenum,GLenum))SDL_GL_GetProcAddress("glClipControl");
        if(!clipControl || !SDL_GL_ExtensionSupported("GL_ARB_clip_control"))die("world depth requires ARB_clip_control");
    }
    memset(depthFormat,0,sizeof depthFormat);
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
    R_EndBatch();
    setDepthMode(0);
    resetReplay();fixtureReplay=0;
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
    resetReplay();
    if(fixtureReplay) {
        if(g_demoMode!=DEMO_NONE || g_trackId!=TRACK_RESORT_ISLAND || g_numPlayers!=MAX_PLAYERS)die("fixture replay requires native Island GP5 race");
        FILE *file=fOpen(PATH_DEMOS "ISLAND.DEM","rb");
        int loaded=file && readReplay(file);if(file)fClose(file);
        if(!loaded)die("invalid or missing Island fixture replay");
        fprintf(stderr,"[vr] fixture_replay=Island pads=%u countdown=60 demo_mode=%d native_chase=1\n",replayCount,g_demoMode);
    }
    clockState=(VRClock){0};haveCamera=0;ticks=frames=extraPasses=0;presented=0;
    modeRace=1;anchored=0;focusSince=0;
    perfWindow=perfNow();perfLastFrame=0;perfFrames=perfTicks=0;perfMaxInterval=0;
    memset(perfMs,0,sizeof perfMs);memset(perfWeatherMs,0,sizeof perfWeatherMs);
    memset(perfDraws,0,sizeof perfDraws);memset(perfWeatherDraws,0,sizeof perfWeatherDraws);
    memset(perfSubmits,0,sizeof perfSubmits);memset(perfVertices,0,sizeof perfVertices);
    if(VR_AutoRace()) {
        /* Start at the actual race camera; the deterministic test needs neither
         * title/demo timers nor the flyover spline. Game data is read normally. */
        g_introCountdown=fixtureReplay?60:0;g_introTimer=0;g_fadeLevel=0;g_fadeState=FADE_VISIBLE;
        fprintf(stderr,"[vr] autorace ready track=%d players=%d synthetic=%d drive=%d weather=%d tod=%d precip_range=%d:%d particle_texture=%d snow_size=%d,%d\n",g_trackId,g_numPlayers,diagnostic,drive,g_weatherType,g_timeOfDay,g_precipVpRange[0],g_precipVpRange[1],g_tpageParticle1,g_precipParticles[0].sizeW,g_precipParticles[0].sizeH);
    }
}
int VR_RaceFrame(void) {
    double waitStart=perfNow();
    for(;;) {
        int visible=begin();if(visible<0)return -1;
        int tick=vr_clock_step(&clockState,displayTime,visible);
        if(clockState.cut)haveCamera=0;
        if(!visible){vr_xr_end(0);continue;}
        frames++;
        if(perfEnabled) {
            double now=perfNow();perfAdd(0,waitStart);
            if(perfLastFrame && now-perfLastFrame>perfMaxInterval)perfMaxInterval=now-perfLastFrame;
            perfLastFrame=now;perfFrames++;perfTicks+=tick;perfSimStart=now;
        }
        if(tick)vr_input_tick();
        return tick;
    }
}
static void setDepthMode(int enabled) {
    if(depthReversed==enabled)return;
    R_EndBatch();
    glMatrixMode(GL_PROJECTION);
    if(enabled)glPushMatrix();else glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    depthReversed=enabled;
    clipControl(GL_LOWER_LEFT,enabled?GL_ZERO_TO_ONE:GL_NEGATIVE_ONE_TO_ONE);
    glClearDepth(enabled?0:1);
    R_SetVRDepthReversed(enabled);
}
static void target(int e) {
    R_EndBatch();
    eye=e;cacheEyeProjection();R_ResetState();
    setDepthMode(perspectiveDepth && modeRace && !identity);
    depthNear=vr_depth_near(g_farClipFloat);
    GLuint color=diagnostic?diagTexture[e]:vr_xr_acquire(e);
    if(!color)die("eye acquisition");
    eyes[e].texture=color;
    glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
    glBindRenderbuffer(GL_RENDERBUFFER,depth[e]);
    GLenum format=depthReversed?GL_DEPTH_COMPONENT32F:GL_DEPTH_COMPONENT24;
    int allocate=depthW[e]!=eyes[e].width || depthH[e]!=eyes[e].height || depthFormat[e]!=format;
    if(allocate) {
        depthW[e]=eyes[e].width;depthH[e]=eyes[e].height;
        glRenderbufferStorage(GL_RENDERBUFFER,format,depthW[e],depthH[e]);
        depthFormat[e]=format;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth[e]);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);glReadBuffer(GL_COLOR_ATTACHMENT0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)die("eye framebuffer incomplete");
    if(allocate && depthReversed) {
        GLint bits=0,type=0;
        glGetRenderbufferParameteriv(GL_RENDERBUFFER,GL_RENDERBUFFER_DEPTH_SIZE,&bits);
        glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE,&type);
        if(bits<32 || type!=GL_FLOAT)die("world depth requires a 32-bit floating depth attachment");
        fprintf(stderr,"[vr] reversed_depth eye=%d bits=%d component=0x%x near_camera_units=1 clip=zero-to-one\n",e,bits,type);
    }
    glDisable(GL_FRAMEBUFFER_SRGB);
    glViewport(0,0,eyes[e].width,eyes[e].height);glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
}
static void unbindEye(void) {
    R_EndBatch();
    setDepthMode(0);
    glBindFramebuffer(GL_FRAMEBUFFER,0);eye=-1;
    glDrawBuffer(GL_BACK);glReadBuffer(GL_BACK);
}
static void release(void) {
    R_EndBatch();
    setDepthMode(0);
    glBindFramebuffer(GL_FRAMEBUFFER,0);if(!diagnostic)vr_xr_release(eye);eye=-1;
    glDrawBuffer(GL_BACK);glReadBuffer(GL_BACK);
}
static void captureCompletedPair(void) {
    /* Called only after the intended shared finish, before either release.
     * Do not target()/clear/reacquire here or read back between the eye draws. */
    R_EndBatch();glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    for(int e=0;e<2;e++) {
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,eyes[e].texture,0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth[e]);
        capture(e?"eye1":"eye0",eyes[e].width,eyes[e].height);
    }
    unbindEye();
}
static int cameraSlot(void) {
    return (g_isNetworkGame || g_netSessionActive)?(unsigned short)g_localPlayerIndex:0;
}
void VR_RenderRace(int tick) {
    if(tick)perfAdd(1,perfSimStart);
    double stageStart=perfNow();
    int *native=(int*)((char*)g_viewportConfigArray+cameraSlot()*0xC8);
    if(tick) {
        memcpy(previous,current,sizeof(current));memcpy(current,native,sizeof(current));
        if(!haveCamera)memcpy(previous,current,sizeof(current));haveCamera=1;
        repeat=0;layer=0;eye=-1;
        /* Canonical pass owns all game-facing render side effects exactly once
         * per tick (weather visibility, wrong-way warning). Also desktop mirror. */
        RenderHUD();perfAdd(2,stageStart);ticks++;mirrorDirty=1;
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
    int pair=pairedComplete && !diagnostic;
    int checkStereo=stereoCheck && ticks>=dumpTick-3;
    uint64_t images[2]={0},streams[2]={0};unsigned counts[2]={0};
    for(int e=0;e<2;e++) {
        stageStart=perfNow();target(e);perfAdd(3,stageStart);layer=0;if(checkStereo)traceStart();
        stageStart=perfNow();repeatRender();perfAdd(4,stageStart);
        if(checkStereo) {
            traceDraw=0;streams[e]=drawHash;counts[e]=drawCount;
            images[e]=imageFingerprint(e);
            if(images[e]!=imageFingerprint(e))die("stereo observer changed without a draw");
            char name[64];snprintf(name,sizeof(name),"frame%04d-eye%d",frames,e);
            capture(name,eyes[e].width,eyes[e].height);
        }
        if(dump && !pair)capture(e?"eye1":"eye0",eyes[e].width,eyes[e].height);
        stageStart=perfNow();if(pair)unbindEye();else release();perfAdd(5,stageStart);
    }
    if(pair) {
        stageStart=perfNow();
        if(!vr_xr_complete_pair())die("paired eye completion without two waited images");
        if(dump)captureCompletedPair();
        vr_xr_release(0);vr_xr_release(1);perfAdd(5,stageStart);
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
    stageStart=perfNow();if(!diagnostic)vr_xr_end(1);perfAdd(6,stageStart);
    presented=1;
    if(dump) {
        fprintf(stderr,"[vr-diag] complete ticks=%d display_frames=%d stereo_passes=%d yaw=%.1f roll=%.1f units_per_metre=%.3f synthetic=%d repeat_state=%s\n",ticks,frames,frames*2+extraPasses,diagYaw/0.01745329251994f,diagRoll/0.01745329251994f,units,diagnostic,diagnostic?"unchanged":"unchecked");
        exit(0);
    }
}
void VR_ApplyCamera(int **camera) {
    R_EndBatch();
    if(eye<0 || identity)return;
    depthNear=vr_depth_near(g_farClipFloat);
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
    rollCos=cosf(roll);rollSin=sinf(roll);
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
void VR_SkyBegin(void) { VR_Layer(2); }
void VR_SkyEnd(void) { VR_Layer(0); }
float VR_LerpWorldDepth(float a,float b,float t) {
    if(!depthReversed || !VR_EyeActive() || layer!=0)return a+(b-a)*t;
    /* CPU screen clipping must interpolate the eventual window depth, not
     * the old linear camera depth. UV/fog interpolation remains native. */
    if(a>=depthNear && b>=depthNear)
        return (float)(1.0/((1.0-t)/(double)a+t/(double)b));
    float da=vr_depth_map(a,depthNear),db=vr_depth_map(b,depthNear);
    return vr_depth_unmap(da+(db-da)*t,depthNear);
}
void VR_EmitVertex(const RenderVertex *v) {
    float w=v->rhw>0?1/v->rhw:1,x=v->sx,y=v->sy,z=v->sz;
    if(!identity && layer==0) {
        if(depthReversed)z=vr_depth_map(z,depthNear);
        VRVec p={(x-g_screenCenterX)*w/g_projScaleXCurrent,(g_screenCenterY-y)*w/g_projScaleY,-w};
        float nx,ny;vr_project_cached(p,eyeProjection,&nx,&ny);x=(nx+1)*320;y=(1-ny)*240;
    } else if(!identity && layer==1) {
        /* Existing UI on a 1.6 x 1.2 m head-relative plane at 2 m. Eye
         * separation remains runtime supplied; no flattening to both eyes. */
        VRVec p={(x-320)/400,(240-y)/400,-2};
        p=vr_sub(vr_add(hudCenter,vr_rotate(hudOrientation,p)),eyes[eye].pose.p);
        p=vr_rotate(eyeInverse,p);
        float nx,ny;vr_project_cached(p,eyeProjection,&nx,&ny);x=(nx+1)*320;y=(1-ny)*240;w=-p.z;z=0;
    } else if(!identity && layer==2) {
        /* Native panorama already uses eye yaw/pitch. Rotate its angular
         * plane for roll, then use the same exact asymmetric FOV correction
         * as world geometry. No eye translation/parallax at infinity. */
        float dx=(x-g_screenCenterX)/g_projScaleXCurrent;
        float dy=(g_screenCenterY-y)/g_projScaleY;
        VRVec direction={rollCos*dx+rollSin*dy,
                         -rollSin*dx+rollCos*dy,-1};
        float nx,ny;vr_project_cached(direction,eyeProjection,&nx,&ny);
        x=(nx+1)*320;y=(1-ny)*240;
    }
    if(depthReversed && layer!=0)z=1-z;
    glVertex4f(x*w,y*w,z*w,w);
}
int VR_Present(void) {
    if(!initialized)return 0;
    R_EndBatch();
    if(presented){
        presented=0;double start=perfNow();
        if(mirrorDirty){platform_gl_swap();mirrorDirty=0;}
        perfAdd(7,start);perfReport();return 1;
    }
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
