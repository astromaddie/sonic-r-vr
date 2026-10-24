/* Actual grid/track/shadow producers -> production backend + VR emitter.
 * Reuse the strict GL recorder; raster depth samples below are CPU evidence,
 * not a claim about the driver's rasterizer or the recorded bridge scene. */
#define main batch_fixture_main
#include "test_adjacent_batch.c"
#undef main
#include "render_grid_d3d.c"
#include "render_track_sdl.c"
int g_farClipTimes8;
static int depthScale=1;

static uint64_t streamHash;
static unsigned oldLeaks,newLeaks,samples,nearCases,xywChecks;
static float projectedZ(const Output *v){
 /* Float GL Z transform/divide; the interpolated sample is stored as the
  * actual attachment type below (legacy D24 or reversed float32). */
 float z=v->state.clipDepth==GL_ZERO_TO_ONE?v->xyzw[2]/v->xyzw[3]:
     (2*v->xyzw[2]-v->xyzw[3])/v->xyzw[3]*.5f+.5f;
 return z;
}
static int sampleDepth(unsigned first,unsigned end,float x,float y,double *depth){
 for(unsigned i=first;i+2<end;i+=3){
  assert(events[i].type==1 && events[i+1].type==1 && events[i+2].type==1);
  Output *a=&events[i].vertex,*b=&events[i+1].vertex,*c=&events[i+2].vertex;
  double ax=(double)a->xyzw[0]/a->xyzw[3],ay=(double)a->xyzw[1]/a->xyzw[3];
  double bx=(double)b->xyzw[0]/b->xyzw[3],by=(double)b->xyzw[1]/b->xyzw[3];
  double cx=(double)c->xyzw[0]/c->xyzw[3],cy=(double)c->xyzw[1]/c->xyzw[3];
  double d=(by-cy)*(ax-cx)+(cx-bx)*(ay-cy);if(fabs(d)<1e-10)continue;
  double wa=((by-cy)*(x-cx)+(cx-bx)*(y-cy))/d;
  double wb=((cy-ay)*(x-cx)+(ax-cx)*(y-cy))/d,wc=1-wa-wb;
  if(wa>=-1e-5 && wb>=-1e-5 && wc>=-1e-5){
   double z=wa*projectedZ(a)+wb*projectedZ(b)+wc*projectedZ(c);
   *depth=a->state.depthFormat==GL_DEPTH_COMPONENT32F?(double)(float)z:round(z*16777215)/16777215;return 1;
  }
 }
 return 0;
}
static void initDepth(int enabled,int e){
 setup(1,1);g_farClipFloat=8000*depthScale;g_farClipTimes8=8000*depthScale;g_farClipDepth=1000*depthScale;
 g_tpageCharBase=0;g_tpageStateArray[0]=4;
 perspectiveDepth=enabled;depthNear=vr_depth_near(g_farClipFloat);target(e);BeginFrame();
 assert(gpu.clipDepth==(enabled?GL_ZERO_TO_ONE:GL_NEGATIVE_ONE_TO_ONE));
 assert(gpu.depthFormat==(enabled?GL_DEPTH_COMPONENT32F:GL_DEPTH_COMPONENT24));
 R_SetTexture(-1);R_SetBlendMode(R_BLEND_NONE);R_SetAlphaTest(0);R_SetDepthWrite(1);
 eventCount=0;g_cpuClipEdge=SCISSOR_NONE;
 g_clipLeft=g_clipTop=0;g_clipRight=639;g_clipBottom=479;
}
static float planeY(int x,int z,int tilt){return -200*depthScale+(tilt==1?x:tilt==2?z:0)*.25f;}
static void surface(int camera,int tilt,int clipped){
 NearClipVert road[4]={0};int xx[4]={-2000,2000,2000,-2000};
 int zz[4]={6000,6000,clipped?-32:1000,clipped?-32:1000};
 for(int i=0;i<4;i++){
  xx[i]*=depthScale;zz[i]*=depthScale;
  road[i].camX=xx[i]-camera;road[i].camY=(int)planeY(xx[i],zz[i],tilt);road[i].depth=zz[i];
  road[i].colorR=road[i].colorG=road[i].colorB=255<<13;
 }
 assert(TrackClipAndEmitQuad(road,-1,g_farClipFloat,4));
}
static void terrain(int camera,int tilt,int z){
 GridProjection gp={g_farClipFloat+64,320,320,320,240,0xffffff};
 GridClipVert tile[4];int xx[4]={-200,200,200,-200},zz[4]={z+200*depthScale,z+200*depthScale,z,z};
 for(int i=0;i<4;i++){xx[i]*=depthScale;tile[i]=(GridClipVert){xx[i]-camera,planeY(xx[i],zz[i],tilt),zz[i],i&1,i>>1};}
 assert(GridClipAndEmitQuad(tile,-1,&gp));
}
static uint64_t geometryHash(int includeDepth){
 uint64_t h=1469598103934665603ULL;
 for(unsigned i=0;i<eventCount;i++){
  Output v=events[i].vertex;
  if(!includeDepth){
   v.xyzw[2]=0;v.state.projection[10]=v.state.projection[14]=0;
   v.state.depthFunc=0;v.state.clearDepth=0;v.state.clipDepth=v.state.depthFormat=0;
  }
  h=hashBytes(h,&v,sizeof v);
 }
 streamHash=hashBytes(streamHash,&h,sizeof h);
 return h;
}
static void terrainTest(void){
 const int scales[]={1,4,11};
 for(unsigned scale=0;scale<3;scale++){depthScale=scales[scale];
 for(int frame=0;frame<21;frame++)for(int tilt=0;tilt<3;tilt++)for(int clip=0;clip<2;clip++){
  uint64_t saved[2]={0};
  const int order[4]={0,1,1,0};
  for(int pass=0;pass<4;pass++){
   int e=order[pass],camera=(frame-10)*4*depthScale+(e?4:-4);uint64_t baseline=0;
   for(int enabled=0;enabled<2;enabled++){
    initDepth(enabled,e);surface(camera,tilt,clip);R_EndBatch();unsigned roadEnd=eventCount;
    for(int z=1200*depthScale;z<5800*depthScale;z+=200*depthScale){
     unsigned start=eventCount;terrain(camera,tilt,z);R_EndBatch();
     /* Interior point projected by the same exact asymmetric-eye mapping. */
     RenderVertex p={320-camera*320.0f/(z+100*depthScale),240-planeY(0,z+100*depthScale,tilt)*320/(z+100*depthScale),.5f,1.0f/(z+100*depthScale),0,0,0,0};
     float x=p.sx,y=p.sy;VRVec ray={(x-320)*(z+100*depthScale)/320,(240-y)*(z+100*depthScale)/320,-(z+100*depthScale)};
     float nx,ny;vr_project_cached(ray,eyeProjection,&nx,&ny);x=(nx+1)*320;y=(1-ny)*240;
     double road,grid;assert(sampleDepth(0,roadEnd,x,y,&road));assert(sampleDepth(start,eventCount,x,y,&grid));
     if(enabled?grid>=road:grid<=road){if(enabled){if(newLeaks<6)fprintf(stderr,"leak scale=%d tilt=%d clip=%d cam=%d z=%d road=%.9g grid=%.9g\n",depthScale,tilt,clip,camera,z,road,grid);newLeaks++;}else oldLeaks++;}if(enabled){samples++;nearCases+=clip;}
    }
    uint64_t h=geometryHash(0);if(!enabled)baseline=h;else{assert(h==baseline);xywChecks++;}
    if(enabled){uint64_t full=geometryHash(1);if(pass<2)saved[e]=full;else assert(saved[e]==full);}
   }
  }
 }
 }
 depthScale=1;
 printf("world depth actual grid/track: samples=%u clipped_track=%u old_leaks=%u new_leaks=%u exact_XYW_UV_color_nonDepthState=%u eye_repeats=exact far=8000,32000,88000\n",samples,nearCases,oldLeaks,newLeaks,xywChecks);fflush(stdout);
 assert(oldLeaks>0 && !newLeaks);
}
static void shadowTest(void){
 unsigned checks=0;
 for(int f=0;f<21;f++)for(int e=0;e<2;e++){
  initDepth(1,e);int cam=(f-10)*4+(e?4:-4);surface(cam,0,0);R_EndBatch();unsigned end=eventCount;
  g_camIntX=cam;g_camIntY=200;g_camIntZ=0;
  memset(g_viewMtx,0,sizeof(int)*16);g_viewMtx[0]=g_viewMtx[5]=g_viewMtx[10]=4096;
  int corners[12]={-30,0,5030,30,0,5030,30,0,4970,-30,0,4970};
  SubmitGroundShadow((intptr_t)corners);R_EndBatch();assert(eventCount>end);
  for(unsigned i=end;i<eventCount;i+=3){
   float x=0,y=0;for(int j=0;j<3;j++){Output *v=&events[i+j].vertex;x+=v->xyzw[0]/v->xyzw[3]/3;y+=v->xyzw[1]/v->xyzw[3]/3;}
   double road,decal;assert(sampleDepth(0,end,x,y,&road));assert(sampleDepth(end,eventCount,x,y,&decal));assert(decal>=road);checks++;
  }
 }
 printf("world depth actual pickup shadow ordering: %u samples passed\n",checks);
}
static void gridNearTest(void){
 unsigned checks=0;
 for(int e=0;e<2;e++)for(int back=-32;back<=0;back+=16){
  uint64_t baseline=0;
  for(int enabled=0;enabled<2;enabled++){
   initDepth(enabled,e);
   GridProjection gp={g_farClipFloat+64,320,320,320,240,0xffffff};
   GridClipVert tile[4]={{-2,-2,32,0,0},{2,-2,32,1,0},{2,-2,back,1,1},{-2,-2,back,0,1}};
   assert(GridClipAndEmitQuad(tile,-1,&gp));R_EndBatch();assert(eventCount);
   for(unsigned i=0;i<eventCount;i++)for(int j=0;j<4;j++)assert(isfinite(events[i].vertex.xyzw[j]));
   uint64_t h=geometryHash(0);if(!enabled)baseline=h;else {assert(h==baseline);checks++;}
  }
 }
 printf("world depth actual grid near-plane clipping: %u exact non-depth streams passed\n",checks);
}
static void rangeAndClipTest(void){
 initDepth(1,0);float n=depthNear;
 assert(isfinite(vr_depth_map(0,n)) && vr_depth_map(0,n)>1 && vr_depth_map(-1,n)>1);
 assert(vr_depth_map(n,n)==1 && vr_depth_map(1,n)==0 && vr_depth_map(2,n)<0);
 assert(vr_depth_map(n/2,n)>1 && vr_depth_map(n*2,n)<1);
 RenderVertex a={20,30,.01f,.005f,0x12345678,0,.1f,.2f},b={300,100,.7f,.0002f,0xabcdef12,0,.8f,.9f},before,after;
 for(int i=0;i<=20;i++){
  float t=i*.05f;setDepthMode(0);grid_screen_lerp(&a,&b,&before,t);
  setDepthMode(1);grid_screen_lerp(&a,&b,&after,t);
  float expected=vr_depth_map(a.sz,n)+(vr_depth_map(b.sz,n)-vr_depth_map(a.sz,n))*t;
  assert(fabsf(vr_depth_map(after.sz,n)-expected)<2e-7f);
  before.sz=after.sz;assert(!memcmp(&before,&after,sizeof before));
 }
 /* Within reverse eyes, non-world depths are the exact complementary native
  * depth (HUD first sets z=0). With reversed comparisons this retains order. */
 for(int mode=0;mode<4;mode++){
  Output saved;
  for(int enabled=0;enabled<2;enabled++){
   initDepth(enabled,0);layer=mode;
   glBegin(GL_TRIANGLES);for(int i=0;i<3;i++)R_EmitVertex(&a);glEnd();
   Output v=events[0].vertex;
   if(!enabled)saved=v;
   else {
    assert(v.xyzw[0]==saved.xyzw[0] && v.xyzw[1]==saved.xyzw[1] && v.xyzw[3]==saved.xyzw[3]);
    if(mode)assert(v.xyzw[2]==(1-saved.xyzw[2]/saved.xyzw[3])*saved.xyzw[3]);
    else assert(v.xyzw[2]!=saved.xyzw[2]);
   }
  }
 }
 setDepthMode(0);
 puts("world depth range/near/far, grid scissor UV/fog, XYW and HUD/sky/fade complementary depths passed");
}
static void lifecycleTest(void){
 setup(1,0);BeginFrame();R_FlushState();
 double nativeProjection[16];memcpy(nativeProjection,gpu.projection,sizeof nativeProjection);
 perspectiveDepth=1;
 /* Exact native vertices and matrices are retained in the identity target. */
 RenderVertex v={120,90,.3f,.01f,0xffffffff,0,.2f,.4f};Output native;
 eventCount=0;glBegin(GL_TRIANGLES);for(int i=0;i<3;i++)R_EmitVertex(&v);glEnd();native=events[0].vertex;
 identity=1;target(0);BeginFrame();R_FlushState();
 assert(!depthReversed && gpu.depthFormat==GL_DEPTH_COMPONENT24 && !projectionPushed);
 eventCount=0;glBegin(GL_TRIANGLES);for(int i=0;i<3;i++)R_EmitVertex(&v);glEnd();
 Output control=events[0].vertex;assert(!memcmp(native.xyzw,control.xyzw,sizeof native.xyzw));
 assert(!memcmp(native.color,control.color,sizeof native.color) && !memcmp(native.uv,control.uv,sizeof native.uv));
 assert(!memcmp(nativeProjection,gpu.projection,sizeof nativeProjection));release();
 unsigned allocations=storageCalls;identity=0;target(0);BeginFrame();R_FlushState();
 assert(storageCalls==allocations+1 && depthReversed && projectionPushed);
 assert(gpu.clipDepth==GL_ZERO_TO_ONE && gpu.clearDepth==0 && gpu.depthFormat==GL_DEPTH_COMPONENT32F);
 assert(gpu.projection[10]==1 && gpu.projection[14]==0 && gpu.depthFunc==GL_GEQUAL);
 for(int i=0;i<16;i++)if(i!=10 && i!=14)assert(gpu.projection[i]==nativeProjection[i]);
 R_SetDepthFunc(R_DEPTH_LESS);R_FlushState();assert(gpu.depthFunc==GL_GREATER);
 R_PushState();R_SetDepthFunc(R_DEPTH_ALWAYS);R_FlushState();assert(gpu.depthFunc==GL_ALWAYS);
 R_PopState();R_FlushState();assert(gpu.depthFunc==GL_GREATER);
 R_SetDepthFunc(R_DEPTH_LEQUAL);R_SetDepthWrite(1);R_ClearDepth();
 assert(events[eventCount-1].type==GL_DEPTH_BUFFER_BIT && events[eventCount-1].vertex.state.clearDepth==0 && gpu.depth);
 for(int l=0;l<4;l++){VR_Layer(l);draw(4);}R_EndBatch();
 release();assert(!depthReversed && !projectionPushed && gpu.clearDepth==1 && gpu.clipDepth==GL_NEGATIVE_ONE_TO_ONE && gpu.depthFunc==GL_LEQUAL);
 assert(!memcmp(nativeProjection,gpu.projection,sizeof nativeProjection));
 allocations=storageCalls;identity=1;target(0);BeginFrame();R_FlushState();assert(storageCalls==allocations+1 && gpu.depthFormat==GL_DEPTH_COMPONENT24);release();
 /* Menu targets and subsequent native frames retain the ordinary domain. */
 identity=0;modeRace=0;target(0);BeginFrame();R_FlushState();assert(!depthReversed && gpu.depthFunc==GL_LEQUAL && gpu.clearDepth==1);release();
 BeginFrame();R_FlushState();assert(!memcmp(nativeProjection,gpu.projection,sizeof nativeProjection));
 /* Shutdown restores the context even if a reverse eye target is abandoned. */
 modeRace=1;target(1);BeginFrame();draw(4);VR_Shutdown();
 assert(!depthReversed && !projectionPushed && gpu.clipDepth==GL_NEGATIVE_ONE_TO_ONE && gpu.clearDepth==1 && gpu.depthFunc==GL_LEQUAL);
 assert(!memcmp(nativeProjection,gpu.projection,sizeof nativeProjection));
 puts("world depth lifecycle: D24 identity/menu, same-size32F reallocation, zero-to-one projection, GEQUAL/GREATER/ALWAYS, clears, layers, release and abandoned-target shutdown restoration passed");
}
int main(void){
 uint64_t immediateHash=0;
 for(fixtureArrays=0;fixtureArrays<2;fixtureArrays++){
  streamHash=1469598103934665603ULL;oldLeaks=newLeaks=samples=nearCases=xywChecks=0;
  terrainTest();shadowTest();gridNearTest();rangeAndClipTest();lifecycleTest();
  if(!fixtureArrays)immediateHash=streamHash;else assert(streamHash==immediateHash);
  printf("world depth backend client_arrays=%d passed\n",fixtureArrays);
 }
 return 0;
}
