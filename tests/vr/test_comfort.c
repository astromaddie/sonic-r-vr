/* Real native camera history -> VR_ApplyCamera -> sky/reflection producers ->
 * real GL emitter. The baseline camera is hash-checked d298137 source. */
#define COMFORT_LIBRARY 1
#include "test_pose_render.c"
#include "legacy_camera.h"
typedef union { int words[50]; RenderCamera camera; } NativeCameraFixture;
_Static_assert(sizeof(RenderCamera)<=sizeof(((NativeCameraFixture *)0)->words),
 "native camera fixture must include the full production 50-int bank");

static void resetChase(void) {
 setup(1,1);haveCamera=0;displayTime=1000000000;
 fixturePose=0;clockState=(VRClock){0};anchorRotation=(VRQuat){0,0,0,1};anchor=(VRVec){0};
 g_trackId=1;g_mirrorMode=0;
 memset(current,0,sizeof current);memset(previous,0,sizeof previous);
 for(int e=0;e<2;e++){eyes[e].pose=(VRPose){{0,0,0,1},{0}};eyes[e].fov=(VRFov){-.9f,1,.95f,-.8f};}
}
static RenderCamera sampleNative(int tick,int scene) {
 float t=tick/30.0f;RenderCamera c={0};c.camFloatY=200;
 if(scene==0){ /* heading wrap/reversal and a translation corner */
  c.pitchFloat=3.05f+(t<1?2.8f*t:t<2?2.8f-5.6f*(t-1):-2.8f);
  c.camFloatX=t<1?120*t:120;c.camFloatZ=t<1?0:120*(t-1);
 }else{ /* intro zoom and pitching native chase */
  float f=1-expf(-t*2);c.camFloatZ=1000*(1-f);c.camFloatY=200+200*(1-f);c.yawFloat=.7f*(1-f);c.pitchFloat=.3f*f;
 }
 return c;
}
static void cameraRegression(void) {
 for(int scene=0;scene<2;scene++)for(int schedule=0;schedule<3;schedule++) {
  resetChase();if(scene)g_introCountdown=100;
  double time=0;int lastTick=-1,count=0,expectedHaveCamera=0;
  int expectedPrevious[50]={0},expectedCurrent[50]={0};
  while(time<3.5) {
   double dt=schedule==0?1.0/90:schedule==1?1.0/120:1.0/(count%2?90:120);time+=dt;
   displayTime=1000000000+(int64_t)llround(time*1e9);
   int tick=vr_clock_step(&clockState,displayTime,1);
   if(tick)lastTick++;
   NativeCameraFixture native={0};native.camera=sampleNative(lastTick<0?0:lastTick,scene);
   /* Model d298137's original tick rule independently. Include no-tick
    * display frames and a mid-run camera-history invalidation. */
   if(count==100){haveCamera=0;expectedHaveCamera=0;}
   if(tick) {
    memcpy(expectedPrevious,expectedCurrent,sizeof expectedPrevious);
    memcpy(expectedCurrent,native.words,sizeof expectedCurrent);
    if(!expectedHaveCamera)memcpy(expectedPrevious,expectedCurrent,sizeof expectedPrevious);
    expectedHaveCamera=1;
   }
   updateCameraHistory(tick,native.words);
   assert(!memcmp(previous,expectedPrevious,sizeof expectedPrevious));
   assert(!memcmp(current,expectedCurrent,sizeof expectedCurrent));
   assert(haveCamera==expectedHaveCamera);
   float headYaw=.18f*sinf((float)time*1.7f),headRoll=.06f*cosf((float)time*.8f);
   VRQuat head=vr_mul((VRQuat){0,sinf(headYaw/2),0,cosf(headYaw/2)},(VRQuat){0,0,sinf(headRoll/2),cosf(headRoll/2)});
   for(int e=0;e<2;e++) {
    eyes[e].pose=(VRPose){head,{.02f+(e?.032f:-.032f),.01f,-.025f}};
    eyes[e].fov=(VRFov){-.9f+count*.00001f+e*.015f,1+count*.00002f-e*.01f,.95f,-.8f+e*.005f};
   }
   /* Compare the real history updater and production VR_ApplyCamera with the
    * hash-checked d298137 camera at each display frame and all eye orders. */
   for(int pass=0;pass<4;pass++) {
    eye=(int[]){1,0,0,1}[pass];cacheEyeProjection();int *cam=current;
    int before[50];memcpy(before,current,sizeof before);
    Baseline_ApplyCamera(&cam);int baseline[50];memcpy(baseline,cam,sizeof baseline);
    memcpy(current,before,sizeof current);cam=current;VR_ApplyCamera(&cam);
    assert(!memcmp(baseline,cam,sizeof baseline));
    assert(eyeProjection.left==tanf(eyes[eye].fov.left));
   }
   count++;
  }
  printf("restored native chase matches hash-checked d298 camera: %s %s, %d display frames, direct tracking/FOV, stereo order 1/0/0/1\n",
   scene?"intro-zoom":"wrap/reversal",schedule==0?"90Hz":schedule==1?"120Hz":"variable90/120",count);
 }
 /* FIXTURE_POSE1 keeps its historical exact-tick alpha bypass. */
 resetChase();fixturePose=1;clockState.alpha=.1f;NativeCameraFixture native={0};native.camera=sampleNative(20,0);
 updateCameraHistory(1,native.words);eye=0;cacheEyeProjection();int *cam=current;int before[50];memcpy(before,current,sizeof before);
 Baseline_ApplyCamera(&cam);int baseline[50];memcpy(baseline,cam,sizeof baseline);
 memcpy(current,before,sizeof current);cam=current;VR_ApplyCamera(&cam);assert(!memcmp(baseline,cam,sizeof baseline));
 puts("FIXTURE_POSE1 exact-tick camera remains byte-exact to d298137");
}
static void skyTemporal(void) {
 double peak[2]={0},steps[2]={0};
 for(int filtered=0;filtered<2;filtered++) {
  resetChase();skyFilter=filtered;NativeCameraFixture native={0};updateCameraHistory(1,native.words);
  g_tpageCount=0;g_tpageStateArray[0]=4;g_parallaxWidth=1664;g_parallaxExtraX=128;g_viewportArray[6]=512;
  float previousSample=0;
  for(int frame=0;frame<150;frame++) {
   float yaw=frame*.0005f;for(int e=0;e<2;e++)eyes[e].pose.q=(VRQuat){0,sinf(yaw/2),0,cosf(yaw/2)};
   eye=0;cacheEyeProjection();int *cam=current;VR_ApplyCamera(&cam);SetViewportClipRect(cam);ComputeParallaxAndPlayfieldState(0,cam);
   /* Include a pre-existing opposing texture pin. Scoped panorama filter
    * must win during its draw and restore the pin for subsequent materials. */
   R_SetTpageFilter(0,filtered?R_FILTER_NEAREST:R_FILTER_LINEAR);
   eventCount=0;VR_SkyBegin();RenderParallaxStripsD3D(0);R_EndBatch();VR_SkyEnd();
   assert(R_GetTpageFilter(0)==(filtered?R_FILTER_NEAREST:R_FILTER_LINEAR));
   for(unsigned i=0;i<eventCount;i++)assert(events[i].vertex.texture.min==(filtered?GL_LINEAR:GL_NEAREST) && !events[i].vertex.state.depth);
   float uv[2],x=-eyeProjection.left/(eyeProjection.right-eyeProjection.left)*640;
   float y=(eyeProjection.up-.25f)/(eyeProjection.up-eyeProjection.down)*480;
   assert(uvAt(x,y,uv));
   /* CPU sampling of a one-texel contrast signal using real emitted UV and
    * effective sampler state isolates filtering from camera/FOV motion.
    * This is a causal fixture, not a screenshot or a claim about GPU output. */
   float u=uv[0]*256-.5f;int i=(int)floorf(u);float f=u-i;
   float sampled=filtered?((i&1)?1-f:f):(((int)floorf(u+.5f))&1);
   if(frame){float delta=fabsf(sampled-previousSample);peak[filtered]=fmax(peak[filtered],delta);steps[filtered]+=delta>.5f;}
   previousSample=sampled;
  }
 }
 assert(peak[0]==1 && peak[1]<.1 && steps[0]>0 && steps[1]==0);
 printf("production sky UV/effective sampler, slow head sweep + CPU contrast signal: peak adjacent sample %.4f/%.4f nearest/linear, >0.5 jumps %.0f/%.0f; scoped atlas pin restored\n",peak[0],peak[1],steps[0],steps[1]);
 skyFilter=1;R_ClearTpageFilter(0);
}
static void reflectionTest(void) {
 resetChase();NativeCameraFixture native={0};native.camera.camFloatY=200;updateCameraHistory(1,native.words);
 eye=0;cacheEyeProjection();int *cam=current;VR_ApplyCamera(&cam);SetViewportClipRect(cam);
 g_tpageCount=0;g_tpageStateArray[0]=4;g_parallaxWidth=1664;g_parallaxExtraX=128;
 g_viewportHeight=g_screenHeight=480;g_numHumans=1;g_scissorEdge=SCISSOR_NONE;g_screenWidthFull=640;g_d3dSurfaceMode=4;
 g_clipLeft=g_clipTop=0;g_clipRight=639;g_clipBottom=479;g_weatherType=0;g_totalFrames=80;waterSurfaceEnabled=0;
 for(int i=0;i<4096;i++)g_sinTable[i]=(int)(16384*sin(i*6.283185307179586/4096));
 ComputeParallaxAndPlayfieldState(0,cam);
 /* Material upload is a resource event, not a reflection behavior change. */
 eye=-1;VR_SkyBegin();RenderWaterReflectionD3D(0);R_EndBatch();VR_SkyEnd();
 uint64_t nativeHash[2];
 for(int control=0;control<2;control++)for(int fade=0;fade<2;fade++) {
  identity=control;eye=control?0:-1;reflectionFade=fade;eventCount=0;VR_SkyBegin();RenderWaterReflectionD3D(0);R_EndBatch();VR_SkyEnd();
  uint64_t h=hashBytes(0,events,eventCount*sizeof *events);if(!fade)nativeHash[control]=h;else assert(h==nativeHash[control]);
 }
 identity=0;eye=0;cacheEyeProjection();reflectionFade=1;
 for(int frame=0;frame<12;frame++) {
  /* The pre-race camera pitches and approaches the water. */
  native.camera.yawFloat=.6f-frame*.04f;native.camera.camFloatY=200+frame*20;clockState.alpha=1;displayTime+=11111111;updateCameraHistory(1,native.words);
  cam=current;VR_ApplyCamera(&cam);SetViewportClipRect(cam);ComputeParallaxAndPlayfieldState(0,cam);
  VR_SkyBegin();R_SetDepthWrite(1);R_SetDepthTest(1);R_SetAlphaTest(1);R_SetBlendMode(R_BLEND_ADDITIVE);
  R_StateSnapshot before=s_desired;
  eventCount=0;RenderWaterReflectionD3D(0);R_EndBatch();assert(!memcmp(&before,&s_desired,sizeof before));
  unsigned alpha=0,clear=0,opaque=0,tint=0;assert(eventCount>6);
  for(unsigned i=0;i<eventCount;i++) {
   Output *v=&events[i].vertex;assert(!v->state.depth);
   if(v->state.src==GL_SRC_ALPHA && v->state.dst==GL_ONE_MINUS_SRC_ALPHA && (v->state.enabled&4)) {
    assert(i>=6);assert(!(v->state.enabled&(2|16)));alpha++;clear+=v->color[3]==0;opaque+=v->color[3]==1;
   }else if(v->state.src==GL_ONE && v->state.dst==GL_ONE && (v->state.enabled&4))tint++;
  }
  assert(alpha && clear && opaque && tint);VR_SkyEnd();
 }
 reflectionFade=1;waterSurfaceEnabled=1;puts("production reflection: native/identity exact controls; 12 intro pitch/height samples, opaque native base before alpha fade, full-to-zero endpoints, native additive tint afterward, no depth writes, exact state restore");
}
int main(void) {
 cameraRegression();skyTemporal();reflectionTest();return 0;
}
