/* Real presentation update -> VR_ApplyCamera -> sky/reflection producers ->
 * real GL emitter. The baseline camera is hash-checked d298137 source. */
#define COMFORT_LIBRARY 1
#include "test_pose_render.c"
#include "legacy_camera.h"

static void resetChase(void) {
 setup(1,1);haveCamera=havePresentation=0;chaseTime=0;displayTime=1000000000;
 fixturePose=0;chaseSmooth=1;clockState=(VRClock){0};anchorRotation=(VRQuat){0,0,0,1};anchor=(VRVec){0};
 g_trackId=1;g_mirrorMode=g_introCountdown=g_postRaceCameraMode=g_demoMode=0;
 memset(current,0,sizeof current);memset(previous,0,sizeof previous);memset(older,0,sizeof older);
 for(int e=0;e<2;e++){eyes[e].pose=(VRPose){{0,0,0,1},{0}};eyes[e].fov=(VRFov){-.9f,1,.95f,-.8f};}
}
static RenderCamera sampleNative(int tick,int scene) {
 float t=tick/30.0f;RenderCamera c={0};c.camFloatY=200;
 if(scene==0){ /* fast heading wrap and sudden reversal, translation corner */
  c.pitchFloat=3.05f+(t<1?2.8f*t:t<2?2.8f-5.6f*(t-1):-2.8f);
  c.camFloatX=t<1?120*t:120;c.camFloatZ=t<1?0:120*(t-1);
 }else{ /* pre-race zoom with a decelerating native chase and pitch */
  float f=1-expf(-t*2);c.camFloatZ=1000*(1-f);c.camFloatY=200+200*(1-f);c.yawFloat=.7f*(1-f);c.pitchFloat=.3f*f;
 }
 return c;
}
static float matrixHeading(const RenderCamera *c) { const float *m=(const float*)((const int*)c+0x22);return atan2f(-m[2],m[10]); }
typedef struct {double peakRate,peakAccel,travel;float finalHeading;} Motion;
static Motion trajectory(int hz,int scene,int baseline) {
 resetChase();if(scene)g_introCountdown=100;
 double time=0,previousTime=0;int lastTick=-1,count=0;float lastHeading=0,lastRate=0;double lastDt=0;Motion result={0};
 while(time<3.5) {
  /* Variable-rate path alternates 90/120 Hz; no wall-clock input. */
  double dt=hz?1.0/hz:1.0/(count%2?90:120);time+=dt;
  displayTime=1000000000+(int64_t)llround(time*1e9);
  int tick=vr_clock_step(&clockState,displayTime,1);
  if(tick)lastTick++;RenderCamera native=sampleNative(lastTick,scene);
  updatePresentationCamera(tick,(int*)&native);
  float heading=0;
  for(int pass=0;pass<4;pass++) {
   int e=(int[]){1,0,0,1}[pass];eye=e;cacheEyeProjection();int *cam=current;
   RenderCamera saved=presentation;float velocities[2];memcpy(velocities,chaseVelocity,sizeof velocities);
   if(baseline)Baseline_ApplyCamera(&cam);else VR_ApplyCamera(&cam);
   assert(!memcmp(&saved,&presentation,sizeof saved) && !memcmp(velocities,chaseVelocity,sizeof velocities));
   if(pass==0)heading=matrixHeading((RenderCamera*)cam);else assert(fabsf(vr_angle(heading,matrixHeading((RenderCamera*)cam),1)-heading)<1e-6f);
   /* Re-timing stays exactly on the latest segment, within its bounds. */
   RenderCamera *a=(RenderCamera*)previous,*b=(RenderCamera*)current,*out=(RenderCamera*)cam;
   float x=out->camFloatX,y=out->camFloatY,z=out->camFloatZ;
   assert(x>=fminf(a->camFloatX,b->camFloatX)-1e-4f && x<=fmaxf(a->camFloatX,b->camFloatX)+1e-4f);
   assert(y>=fminf(a->camFloatY,b->camFloatY)-1e-4f && y<=fmaxf(a->camFloatY,b->camFloatY)+1e-4f);
   assert(z>=fminf(a->camFloatZ,b->camFloatZ)-1e-4f && z<=fmaxf(a->camFloatZ,b->camFloatZ)+1e-4f);
  }
  if(count){float delta=remainderf(heading-lastHeading,6.28318530718f),rate=delta/(time-previousTime);
   result.peakRate=fmax(result.peakRate,fabs(rate));result.travel+=fabs(delta);
   if(count>1)result.peakAccel=fmax(result.peakAccel,fabs(rate-lastRate)/((time-previousTime+lastDt)*.5));lastRate=rate;lastDt=time-previousTime;
  }
  lastHeading=heading;previousTime=time;count++;
 }
 result.finalHeading=lastHeading;return result;
}
static void motionTests(void) {
 for(int scene=0;scene<2;scene++) {
  float finals[3];int i=0;
  for(int hz=90;hz<=150;hz+=30){int refresh=hz==150?0:hz;
   Motion old=trajectory(refresh,scene,1),now=trajectory(refresh,scene,0);finals[i++]=now.finalHeading;
   printf("comfort %s %s: baseline/new peak yaw deg/s %.3f/%.3f, accel deg/s2 %.3f/%.3f, travel deg %.3f/%.3f\n",scene?"intro-zoom":"wrap/reversal",refresh==90?"90Hz":refresh==120?"120Hz":"variable90/120",old.peakRate*57.29578,now.peakRate*57.29578,old.peakAccel*57.29578,now.peakAccel*57.29578,old.travel*57.29578,now.travel*57.29578);
   assert(now.peakRate<=3.1417 && now.peakAccel<13.0);
   assert(now.peakAccel<old.peakAccel);
   if(!scene)assert(now.peakRate<old.peakRate);
  }
  assert(fabsf(remainderf(finals[0]-finals[1],6.2831853f))<.015f);
  assert(fabsf(remainderf(finals[0]-finals[2],6.2831853f))<.015f);
 }
 /* C1 only on compatible collinear joins; reversal/corner safety dominates.
  * Real production segment retiming is monotone even at slope bounds. */
 resetChase();RenderCamera *o=(RenderCamera*)older,*a=(RenderCamera*)previous,*b=(RenderCamera*)current;
 for(int mode=0;mode<4;mode++) {
  o->camFloatX=mode==0?-10:mode==1?20:mode==2?-100:0;b->camFloatX=10;
  float last=-1;
  for(int i=0;i<=100;i++){float t=chasePositionFraction(i/100.f);assert(t>=last && t>=0 && t<=1);last=t;}
  if(mode==0)assert(fabsf(chasePositionFraction(.37f)-.37f)<1e-6f);
 }
 (void)a;puts("native-segment translation: monotone, confined; collinear speed retained; reversal/corner continuity intentionally not claimed");
}
static void trackingAndCuts(void) {
 resetChase();RenderCamera native={0};native.camFloatY=200;native.pitchFloat=.5f;
 updatePresentationCamera(1,(int*)&native);
 for(int frame=0;frame<40;frame++) {
  displayTime+=11111111;clockState.alpha=.6f;native.pitchFloat+=.02f;updatePresentationCamera(1,(int*)&native);
  RenderCamera saved=presentation;
  float p=presentation.yawFloat,y=presentation.pitchFloat,sp=sinf(p),cp=cosf(p),sy=sinf(y),cy=cosf(y);
  float base[9]={cy,sy*sp,-sy*cp,0,cp,sp,sy,-sp*cy,cp*cy};
  VRQuat head=vr_mul((VRQuat){0,sinf(frame*.02f),0,cosf(frame*.02f)},(VRQuat){0,0,sinf(frame*.01f),cosf(frame*.01f)});
  for(int e=0;e<2;e++)eyes[e].pose=(VRPose){head,{.04f+(e?.032f:-.032f),.02f,-.03f}};
  for(int pass=0;pass<4;pass++) {
   eye=(int[]){1,0,0,1}[pass];eyes[eye].fov.left=-.9f+frame*.00001f;cacheEyeProjection();int *cam=current;VR_ApplyCamera(&cam);
   float expected[9];vr_compose_basis(base,head,expected);float *actual=(float*)(cam+0x22);
   for(int r=0;r<3;r++)for(int c=0;c<3;c++)assert(fabsf(actual[r*4+c]-expected[r*3+c])<1e-6f);
   VRVec offset=vr_game_offset(base,eyes[eye].pose.p,units);RenderCamera *out=(RenderCamera*)cam;
   assert(fabsf(out->camFloatX-presentation.camFloatX-offset.x)<2e-5f);
   assert(!memcmp(&saved,&presentation,sizeof saved));assert(eyeProjection.left==tanf(eyes[eye].fov.left));
  }
 }
 /* Race/focus resume invalidate the same shared state. Test every explicit
  * context cut and teleport through the actual frame-boundary updater. */
 for(int mode=0;mode<9;mode++) {
  if(mode==0)haveCamera=havePresentation=0;
  if(mode==1)g_mirrorMode^=1;
  if(mode==2)g_trackId++;
  if(mode==3)g_introCountdown=100;
  if(mode==4)g_postRaceCameraMode=1;
  if(mode==5)g_demoMode=1;
  if(mode==6)native.camFloatX+=1000;
  if(mode==7)displayTime+=500000000;
  if(mode==8)g_flyoverWpIdx[0]++;
  native.pitchFloat+=.5f;displayTime+=11111111;clockState.alpha=1;updatePresentationCamera(1,(int*)&native);
  assert(fabsf(presentation.pitchFloat-native.pitchFloat)<1e-5f && chaseVelocity[0]==0 && chaseVelocity[1]==0);
 }
 /* Bypass/native/identity remain exact d298137 camera behavior. */
 resetChase();native.pitchFloat=.7f;updatePresentationCamera(1,(int*)&native);chaseSmooth=0;
 for(int control=0;control<3;control++) {
  eye=control==1?-1:0;identity=control==2;int *cam=current;Baseline_ApplyCamera(&cam);int old[50];memcpy(old,cam,sizeof old);
  cam=current;VR_ApplyCamera(&cam);assert(!memcmp(old,cam,sizeof old));
 }
 chaseSmooth=1;identity=0;puts("direct head rotation/translation and runtime FOV: exact composition, four eye orders; context/teleport/resume cuts; native/identity/disabled camera: exact baseline");
}
static void skyTemporal(void) {
 double peak[2]={0},steps[2]={0};
 for(int filtered=0;filtered<2;filtered++) {
  resetChase();skyFilter=filtered;RenderCamera native={0};updatePresentationCamera(1,(int*)&native);
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
 resetChase();RenderCamera native={0};native.camFloatY=200;updatePresentationCamera(1,(int*)&native);
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
  native.yawFloat=.6f-frame*.04f;native.camFloatY=200+frame*20;clockState.alpha=1;displayTime+=11111111;updatePresentationCamera(1,(int*)&native);
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
 motionTests();trackingAndCuts();skyTemporal();reflectionTest();return 0;
}
