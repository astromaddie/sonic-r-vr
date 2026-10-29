/* Full production track transform, projection, backend and pose composition.
 * The reference TU is the installed baseline, not a duplicate formula. */
#define WORLD_DEPTH_LIBRARY 1
#define VR_REAL_CAMERA 1
#include "test_world_depth.c"
#include "vr/vr_projection.h"
extern int g_footShadowVerts[];
int g_qtEdgeIdx,g_qtSurfIdx;
int QueryTerrainHeight(int x,int z,int y){(void)x;(void)z;(void)y;return 0;}
#include "legacy_pose.h"
void Legacy_RenderTrackD3D(void);
void RenderHiddenSubEntry(int x,int z,int y){(void)x;(void)y;(void)z;assert(0);}
float g_uvLUT256[256];
int g_saveBlock[554];
void PlaySoundEffect(int a,int b,int c){(void)a;(void)b;(void)c;}
unsigned short vr_input_read(const short *bindings){(void)bindings;return 0;}
int platform_poll_events(unsigned char *keys,int n){memset(keys,0,n);return 0;}
int platform_poll_gamepads(unsigned short *pads,int n){memset(pads,0,n*sizeof *pads);pads[0]=VR_PadBits();return 1;}
void StopAmbientSounds(void){}
static SrcVertex trackVertices[32768];
static int object[17],polygon[12];
static void initObject(int mode){
 for(int i=0;i<256;i++)g_uvLUT256[i]=(i+.5f)/256;
 memset(object,0,sizeof object);memset(polygon,0,sizeof polygon);
 g_totalObjects=1;g_objectStructArray=object;g_vertexArrayBase=trackVertices;g_polygonArrayBase=polygon;
 *(short *)((char *)object+0x2c)=30000;*(short *)((char *)object+0x2e)=mode;
 *(unsigned short *)((char *)object+0x32)=1;*(unsigned short *)((char *)object+0x3a)=4;
 for(int i=0;i<4;i++){*(unsigned short *)((char *)polygon+0x20+2*i)=i;polygon[i*2]=(i&1)*32<<16;polygon[i*2+1]=(i>>1)*32<<16;}
 *((unsigned char *)polygon+0x2e)=5;g_tpageStateArray[0]=4;g_trackId=1;
 g_sinTable[0]=0;g_cosTable[0]=16384;
 const int x[]={-2000,2000,2000,-2000},z[]={6000,6000,1000,1000};
 for(int i=0;i<4;i++){trackVertices[i]=(SrcVertex){0};trackVertices[i].posX=x[i]*depthScale;trackVertices[i].posZ=z[i]*depthScale;trackVertices[i].colorR=trackVertices[i].colorG=trackVertices[i].colorB=255<<13;}
}
static void poseCamera(int frame,int e){
 initDepth(1,e);haveCamera=1;anchorRotation=(VRQuat){0,0,0,1};anchor=(VRVec){0};
 float yaw=(frame-10)*.06f,pitch=.18f*sinf(frame*.2f),bank=.45f*sinf(frame*.3f);
 VRQuat q=vr_mul(vr_mul((VRQuat){0,sinf(yaw/2),0,cosf(yaw/2)},(VRQuat){sinf(pitch/2),0,0,cosf(pitch/2)}),(VRQuat){0,0,sinf(bank/2),cosf(bank/2)});
 for(int i=0;i<2;i++)eyes[i].pose=(VRPose){q,vr_rotate(q,(VRVec){i?.032f:-.032f,0,0})};
 RenderCamera cam={0};cam.camFloatY=500;cam.yawFloat=.12f;
 memcpy(current,&cam,sizeof cam);memcpy(previous,&cam,sizeof cam);clockState.alpha=.5f;
 cacheEyeProjection();int *vp=current;VR_ApplyCamera(&vp);SetViewportClipRect(vp);
 g_clipLeft=g_clipTop=0;g_clipRight=639;g_clipBottom=479;eventCount=0;
}
static void fullTrackTest(void){
 unsigned corrected=0,oldWrong=0,streams=0,oldLeaks=0,fixedLeaks=0,depthSamples=0;
 for(depthScale=1;depthScale<=11;depthScale+=10)
 for(int tilt=0;tilt<3;tilt++)for(int frame=0;frame<21;frame++)for(int e=0;e<2;e++)for(int mode=0;mode<3;mode++){
  poseCamera(frame,e);initObject(mode);for(int i=0;i<4;i++)trackVertices[i].posY=tilt==1?trackVertices[i].posX/8:tilt==2?trackVertices[i].posZ/8:0;Legacy_RenderTrackD3D();R_EndBatch();assert(eventCount>0);
  int oldX[4];for(int i=0;i<4;i++)oldX[i]=trackVertices[i].camX;
  eventCount=0;RenderTrackD3D();R_EndBatch();assert(eventCount>0);streams++;
  for(int i=0;i<4;i++){
   SrcVertex *v=trackVertices+i;int dx=v->posX-g_camIntX,dy=v->posY-g_camIntY,dz=v->posZ-g_camIntZ;
   int full=(g_viewMtx00*dx+g_viewMtx10*dy+g_viewMtx20*dz)>>12;
   const float *m=(const float *)g_floatMtxDest;
   float fx=(v->posX-g_camFloatX)*m[0]+((v->posZ-g_camFloatZ)*m[8]+(v->posY-g_camFloatY)*m[4]);
   assert(v->camX==(int)fx);corrected++;oldWrong+=oldX[i]!=full;
  }
  for(int legacy=0;legacy<2;legacy++){
   eventCount=0;if(legacy)Legacy_RenderTrackD3D();else RenderTrackD3D();R_EndBatch();unsigned end=eventCount;
   const float *m=(const float *)g_floatMtxDest;
   GridProjection gp={g_farClipFloat+64,(float)g_projScaleXCurrent,(float)g_projScaleY,(float)g_screenCenterX,(float)g_screenCenterY,0xffffff};
   for(int z=1400*depthScale;z<5600*depthScale;z+=200*depthScale){
    GridClipVert tile[4];const int tx[]={-100,100,100,-100},tz[]={100,100,-100,-100};
    for(int i=0;i<4;i++){
     float x=tx[i]-g_camFloatX,y=(tilt==1?tx[i]/8.0f:tilt==2?(z+tz[i])/8.0f:0)-g_camFloatY,zz=z+tz[i]-g_camFloatZ;
     tile[i]=(GridClipVert){x*m[0]+(zz*m[8]+y*m[4]),x*m[1]+(zz*m[9]+y*m[5]),x*m[2]+(zz*m[10]+y*m[6]),i&1,i>>1};
    }
    unsigned first=eventCount;assert(GridClipAndEmitQuad(tile,0,&gp));R_EndBatch();
    float x=0,y=0;for(unsigned j=first;j<first+3;j++){x+=events[j].vertex.xyzw[0]/events[j].vertex.xyzw[3]/3;y+=events[j].vertex.xyzw[1]/events[j].vertex.xyzw[3]/3;}
    double road,grid;if(sampleDepth(0,end,x,y,&road)){
     assert(sampleDepth(first,eventCount,x,y,&grid));if(grid>=road){if(legacy)oldLeaks++;else fixedLeaks++;}if(!legacy)depthSamples++;
    }
   }
  }
 }
 assert(oldWrong>0);printf("full RenderTrackD3D modes0/1/2: %u actual streams; %u correct full-basis vertices; baseline wrong=%u\n",streams,corrected,oldWrong);
 printf("posed full track vs float grid: samples=%u baseline_leaks=%u corrected_leaks=%u (ties fail)\n",depthSamples,oldLeaks,fixedLeaks);assert(depthSamples>15000 && oldLeaks>0 && !fixedLeaks);depthScale=1;
}
static void nativeTrackTest(void){
 unsigned checks=0;
 for(int frame=0;frame<9;frame++)for(int mode=0;mode<3;mode++)for(int ident=0;ident<3;ident++){
  poseCamera(frame,0);initObject(mode);setDepthMode(ident==2);identity=ident==1;eye=ident?0:-1;trackFloat=ident!=2;
  eventCount=0;Legacy_RenderTrackD3D();R_EndBatch();referenceCount=eventCount;memcpy(reference,events,eventCount*sizeof *events);
  eventCount=0;RenderTrackD3D();R_EndBatch();assert(eventCount==referenceCount && !memcmp(reference,events,eventCount*sizeof *events));checks++;
 }
 trackFloat=1;printf("full native/identity/legacy-control track: %u exact baseline streams (all object modes, nonzero camera bank)\n",checks);
}
static void trackEdgesTest(void){
 unsigned clipped=0,rotated=0,repeated=0;
 for(int count=3;count<=4;count++)for(unsigned mask=0;mask<(1u<<count);mask++)for(int mirror=0;mirror<2;mirror++){
  poseCamera(0,0);initObject(0);g_mirrorMode=mirror;
  float *m=(float *)g_floatMtxDest;memset(m,0,16*sizeof *m);m[0]=m[5]=m[10]=1;
  memset(g_viewMtx,0,16*sizeof(int));g_viewMtx[0]=g_viewMtx[5]=g_viewMtx[10]=4096;
  g_camIntX=g_camIntY=g_camIntZ=0;g_camFloatX=.125f;g_camFloatY=.25f;g_camFloatZ=0;
  g_screenCenterX=320;g_screenCenterY=240;g_projScaleXCurrent=g_projScaleY=200;
  *(unsigned char *)((char *)polygon+0x2e)=count==4?5:4;
  for(int i=0;i<count;i++){trackVertices[i].posX=(i==0 || i==3)?-2:2;trackVertices[i].posY=i<2?1:-1;trackVertices[i].posZ=(mask>>i&1)?-2:4;trackVertices[i].colorR=(20+i*30)<<13;}
  eventCount=0;Legacy_RenderTrackD3D();R_EndBatch();referenceCount=eventCount;memcpy(reference,events,eventCount*sizeof *events);
  eventCount=0;RenderTrackD3D();R_EndBatch();assert(eventCount==referenceCount);
  for(unsigned i=0;i<eventCount;i++){
   Output *v=&events[i].vertex;assert(v->xyzw[3]>=1 && isfinite(v->xyzw[0]) && isfinite(v->xyzw[1]));
   assert(!memcmp(v->uv,reference[i].vertex.uv,sizeof v->uv));assert(!memcmp(v->color,reference[i].vertex.color,sizeof v->color));
   assert(projectedZ(v)>=0 && projectedZ(v)<=1);
  }
  clipped+=eventCount>0 && mask!=0;
 }
 g_mirrorMode=0;
 for(int i=0;i<4096;i++){g_sinTable[i]=(int)(16384*sin(i*6.283185307179586/4096));g_cosTable[i]=(int)(16384*cos(i*6.283185307179586/4096));}
 for(int f=0;f<9;f++){
  int world[4][3];poseCamera(f,0);initObject(2);
  *(short *)((char *)object+0x18)=20+f*3;*(short *)((char *)object+0x1a)=13+f*2;*(short *)((char *)object+0x1c)=17+f*2;
  object[8]=17;object[9]=31;object[10]=103;
  memset(g_viewMtx,0,16*sizeof(int));g_viewMtx[0]=g_viewMtx[5]=g_viewMtx[10]=4096;g_camIntX=g_camIntY=g_camIntZ=0;
  Legacy_RenderTrackD3D();R_EndBatch();for(int i=0;i<4;i++){world[i][0]=trackVertices[i].camX;world[i][1]=trackVertices[i].camY;world[i][2]=trackVertices[i].depth;}
  uint64_t hashes[2];const int order[]={0,1,1,0};
  for(int pass=0;pass<4;pass++){
   poseCamera(f,order[pass]);g_tpageStateArray[0]=4;eventCount=0;RenderTrackD3D();R_EndBatch();assert(eventCount>0);
   const float *m=(float *)g_floatMtxDest;
   for(int i=0;i<4;i++)for(int j=0;j<3;j++){
    float x=world[i][0]-g_camFloatX,y=world[i][1]-g_camFloatY,z=world[i][2]-g_camFloatZ;
    assert(vrTrackXYZ[i][j]==x*m[j]+(z*m[8+j]+y*m[4+j]));rotated++;
   }
   uint64_t h=hashBytes(0,events,eventCount*sizeof *events);if(pass<2)hashes[order[pass]]=h;else{assert(h==hashes[order[pass]]);repeated++;}
  }
 }
 /* A sphere touching each sloped view plane must not disappear even when
  * its centre lies outside. Behind/far spheres are rejected. */
 poseCamera(0,0);float *m=(float *)g_floatMtxDest;memset(m,0,16*sizeof *m);m[0]=m[5]=m[10]=1;
 g_camFloatX=g_camFloatY=g_camFloatZ=0;g_projScaleXCurrent=g_projScaleY=320;g_screenCenterX=320;g_screenCenterY=240;
 assert(VRTrackVisible(1100,0,1000,100,8000));assert(VRTrackVisible(-1100,0,1000,100,8000));
 assert(VRTrackVisible(0,800,1000,100,8000));assert(VRTrackVisible(0,-800,1000,100,8000));
 assert(!VRTrackVisible(0,0,-200,100,8000));assert(!VRTrackVisible(0,0,8200,100,8000));
 printf("actual track near clipping: %u nonempty clipped cases, exact native UV/color/fan order; local rotation %u float coordinates, %u reversed-eye repeats; conservative sphere planes passed\n",clipped,rotated,repeated);
}
static void pauseTest(void){
 unsigned checks=0;
 for(int frame=0;frame<21;frame++)for(int e=0;e<2;e++){
  poseCamera(frame,e);g_tpageObjects=0;g_tpageStateArray[0]=4;g_demoMode=DEMO_NONE;g_isPaused=1;
  g_projScaleXCurrent=g_projScaleY=512;
  R_SetDepthTest(0);R_SetDepthWrite(0);R_SetDepthFunc(R_DEPTH_LESS);R_SetBlendMode(R_BLEND_ALPHA);
  R_StateSnapshot saved=s_desired;eventCount=0;
  VR_HudBegin();DrawPauseOverlay();VR_HudEnd();R_EndBatch();
  assert(!memcmp(&saved,&s_desired,sizeof saved));assert(eventCount==31 && events[0].type==GL_DEPTH_BUFFER_BIT);
  double background=projectedZ(&events[1].vertex),letter=projectedZ(&events[7].vertex);
  assert(letter>background && background>.25 && letter<.75);
  /* The actual installed emitter erases this native priority and places both
   * depths exactly on the clipping boundary, independent of the pose. */
  layer=1;float oldA[4],oldB[4];RenderVertex v={224,192,5/g_farClipFloat,.2f,0xffffffff,0,0,0};
  LegacyPoseTransform(&v,oldA);v.sz=4/g_farClipFloat;v.rhw=.25f;LegacyPoseTransform(&v,oldB);
  assert(oldA[2]==oldA[3] && oldB[2]==oldB[3]);layer=0;
  for(unsigned i=1;i<eventCount;i++){assert(events[i].type==1);GLState *s=&events[i].vertex.state;assert((s->enabled&2) && s->depth && s->depthFunc==GL_GEQUAL);}
  /* A second HUD scope retains the existing HUD depth instead of clearing it. */
  eventCount=0;VR_HudBegin();DrawPauseOverlay();VR_HudEnd();R_EndBatch();assert(eventCount==30);checks++;
 }
 g_isPaused=0;printf("actual pause overlay: %u posed stereo passes; depth4 text strictly ahead of depth5 background, interior clip depth, one clear, inherited state restored\n",checks);
}
/* Clip the actual homogeneous eye triangles, including their w=0 crossings.
 * This exercises sky coverage instead of judging only a few emitted points. */
typedef struct { double x,y,w; } ClipPoint;
static double edgeDistance(ClipPoint p,int edge){return edge==0?p.x:edge==1?640*p.w-p.x:edge==2?p.y:480*p.w-p.y;}
static int skyContains(float x,float y){
 for(unsigned i=0;i+2<eventCount;i+=3){
  ClipPoint p[10],q[10];int n=3;
  for(int j=0;j<3;j++){float *v=events[i+j].vertex.xyzw;p[j]=(ClipPoint){v[0],v[1],v[3]};}
  for(int edge=0;edge<4 && n;edge++){
   int out=0;for(int j=0;j<n;j++){ClipPoint a=p[(j+n-1)%n],b=p[j];double da=edgeDistance(a,edge),db=edgeDistance(b,edge);
    if((da>=0)!=(db>=0)){double t=da/(da-db);q[out++]=(ClipPoint){a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.w+(b.w-a.w)*t};}
    if(db>=0)q[out++]=b;
   }n=out;memcpy(p,q,n*sizeof *p);
  }
  for(int j=1;j+1<n;j++){
   ClipPoint a=p[0],b=p[j],c=p[j+1];if(a.w<=0 || b.w<=0 || c.w<=0)continue;
   double ax=a.x/a.w,ay=a.y/a.w,bx=b.x/b.w,by=b.y/b.w,cx=c.x/c.w,cy=c.y/c.w;
   double d=(by-cy)*(ax-cx)+(cx-bx)*(ay-cy);if(fabs(d)<1e-10)continue;
   double u=((by-cy)*(x-cx)+(cx-bx)*(y-cy))/d,v=((cy-ay)*(x-cx)+(ax-cx)*(y-cy))/d;
   if(u>=-1e-6 && v>=-1e-6 && u+v<=1+1e-6)return 1;
  }
 }
 return 0;
}
static void skyTest(void){
 unsigned expected=0,oldMissing=0,newMissing=0,repeatChecks=0;
 for(int track=1;track<=5;track++)for(int mirror=0;mirror<2;mirror++)for(int frame=0;frame<9;frame++)for(int e=0;e<2;e++){
  poseCamera(frame*2,e);g_trackId=track;g_mirrorMode=mirror;g_tpageCount=0;g_tpageStateArray[0]=4;g_parallaxWidth=1664;g_parallaxExtraX=128;g_viewportArray[6]=512;
  /* Combined view rotation; turn far enough to expose side/zenith coverage. */
  float yaw=(frame-4)*.3f,pitch=(frame-4)*.32f,bank=(frame-4)*.24f;
  VRQuat q=vr_mul(vr_mul((VRQuat){0,sinf(yaw/2),0,cosf(yaw/2)},(VRQuat){sinf(pitch/2),0,0,cosf(pitch/2)}),(VRQuat){0,0,sinf(bank/2),cosf(bank/2)});
  for(int k=0;k<2;k++){eyes[k].fov=(VRFov){k?-.91f:-1.01f,k?1.01f:.91f,.96f,-.96f};eyes[k].pose.q=vr_mul(q,(VRQuat){0,sinf((k?1:-1)*.015f),0,cosf(.015f)});eyes[k].pose.p=(VRVec){k?.03f:-.03f,.1f,-.1f};}
  cacheEyeProjection();int *cam=current;VR_ApplyCamera(&cam);SetViewportClipRect(cam);ComputeParallaxAndPlayfieldState(0,cam);
  for(int legacy=1;legacy>=0;legacy--){
   eventCount=0;VR_SkyBegin();if(legacy)LegacyPanorama(0);else RenderParallaxStripsD3D(0);R_EndBatch();
   if(!legacy){assert(eventCount==52*9);for(unsigned cap=6;cap<eventCount;cap+=9)for(unsigned j=0;j<3;j++)assert(events[cap+j].vertex.uv[0]==0 && events[cap+j].vertex.uv[1]==0);}
   for(int y=8;y<480;y+=58)for(int x=8;x<640;x+=78){
    float cx=eyeProjection.left+(eyeProjection.right-eyeProjection.left)*x/640;
    float cy=eyeProjection.up+(eyeProjection.down-eyeProjection.up)*y/480;
    float worldY=skyBasis[3]*cx+skyBasis[4]*cy+skyBasis[5];
    /* Strict upper hemisphere avoids the Emerald's authored lowered rim. */
    if(worldY>.02f){if(!skyContains(x,y)){if(legacy)oldMissing++;else newMissing++;}if(!legacy)expected++;}
   }
   if(!legacy){
    referenceCount=eventCount;memcpy(reference,events,eventCount*sizeof *events);
    /* Translating either eye/head cannot move this rotation-only sky. */
    eyes[e].pose.p=(VRVec){90,-80,70};eventCount=0;RenderParallaxStripsD3D(0);R_EndBatch();assert(eventCount==referenceCount && !memcmp(reference,events,eventCount*sizeof *events));repeatChecks++;
   }
  }
  VR_SkyEnd();g_mirrorMode=0;
 }
 printf("actual panorama tracks1..5/mirror/canted eyes/combined poses: coverage=%u baseline_missing=%u corrected_missing=%u exact_translation_repeats=%u\n",expected,oldMissing,newMissing,repeatChecks);
 assert(expected && oldMissing && !newMissing);
}
static int uvAt(float x,float y,float out[2]){
 int found=0;
 for(unsigned i=0;i+2<eventCount;i+=3){
  Output *a=&events[i].vertex,*b=&events[i+1].vertex,*c=&events[i+2].vertex;
  float aw=a->xyzw[3],bw=b->xyzw[3],cw=c->xyzw[3];if(aw<=0 || bw<=0 || cw<=0)continue;
  double ax=a->xyzw[0]/aw,ay=a->xyzw[1]/aw,bx=b->xyzw[0]/bw,by=b->xyzw[1]/bw,cx=c->xyzw[0]/cw,cy=c->xyzw[1]/cw;
  double d=(by-cy)*(ax-cx)+(cx-bx)*(ay-cy);if(fabs(d)<1e-10)continue;
  double u=((by-cy)*(x-cx)+(cx-bx)*(y-cy))/d,v=((cy-ay)*(x-cx)+(ax-cx)*(y-cy))/d,w=1-u-v;
  if(u>=-1e-6 && v>=-1e-6 && w>=-1e-6){for(int j=0;j<2;j++)out[j]=(u*a->uv[j]/aw+v*b->uv[j]/bw+w*c->uv[j]/cw)/(u/aw+v/bw+w/cw);found=1;}
 }
 return found;
}
static void skyAtlasTest(void){
 skyFilter=0; /* Exact old atlas coordinates remain available for causal A/B. */
 float largest=0;
 for(int track=1;track<=5;track++)for(int mirror=0;mirror<2;mirror++){
  poseCamera(0,0);g_trackId=track;g_mirrorMode=mirror;g_tpageCount=0;g_tpageStateArray[0]=4;g_parallaxWidth=1664;g_parallaxExtraX=128;g_viewportArray[6]=512;
  RenderCamera cam={0};memcpy(current,&cam,sizeof cam);memcpy(previous,&cam,sizeof cam);anchorRotation=(VRQuat){0,0,0,1};
  for(int k=0;k<2;k++){eyes[k].pose=(VRPose){{0,0,0,1},{0}};eyes[k].fov=(VRFov){-atanf(.625f),atanf(.625f),atanf(.46875f),-atanf(.46875f)};}
  cacheEyeProjection();int *camera=current;VR_ApplyCamera(&camera);SetViewportClipRect(camera);ComputeParallaxAndPlayfieldState(0,camera);
  float horizon=track==5?-.125f:0,rayY=horizon+.25f;
  float x=(-eyeProjection.left)/(eyeProjection.right-eyeProjection.left)*640,y=(eyeProjection.up-rayY)/(eyeProjection.up-eyeProjection.down)*480;
  float old[2],now[2];eventCount=0;VR_SkyBegin();LegacyPanorama(0);R_EndBatch();assert(uvAt(x,y,old));
  eventCount=0;RenderParallaxStripsD3D(0);R_EndBatch();assert(uvAt(x,y,now));VR_SkyEnd();
  float delta=fmaxf(fabsf(old[0]-now[0]),fabsf(old[1]-now[1]))*256;if(delta>largest)largest=delta;
  /* At the native optical axis the 32/1664-turn chord sag is <0.057 texel. */
  assert(delta<.065f);
 }
 g_mirrorMode=0;skyFilter=1;printf("native-FOV neutral atlas tracks1..5/mirror, including Emerald rim: maximum chord texel delta %.6f\n",largest);
}
static void statusTest(void){
 poseCamera(8,0);g_projScaleXCurrent=g_projScaleY=512;g_tpageParallax1=g_tpageCharBase=g_tpageObjects=0;g_tpageStateArray[0]=4;
 g_numPlayers=5;g_numHumans=1;g_demoMode=DEMO_NONE;g_postRaceCameraMode=0;g_minimapToggle=1;g_raceType=RACE_GP;g_screenWidthFull=640;
 for(int i=0;i<5;i++){g_playerBase[i]=(Player){0};g_playerBase[i].charId=i;g_playerBase[i].posX=-0x2d37*4096;g_playerBase[i].posZ=0x28eb*4096;}
 eventCount=0;VR_HudBegin();DrawMinimapWidget((char *)g_playerBase);VR_HudEnd();R_EndBatch();assert(eventCount==37);
 double background=projectedZ(&events[1].vertex),own=projectedZ(&events[31].vertex);
 for(unsigned i=7;i<31;i+=6)assert(projectedZ(&events[i].vertex)>background && projectedZ(&events[i].vertex)<own);
 for(int results=0;results<2;results++){
  g_postRaceCameraMode=results;eventCount=0;VR_HudBegin();DrawTimerAndStatus(0);VR_HudEnd();R_EndBatch();assert(eventCount>0);
  for(unsigned i=0;i<eventCount;i++)if(events[i].type==1)assert(projectedZ(&events[i].vertex)>.25 && projectedZ(&events[i].vertex)<.75);
 }
 puts("actual minimap/background/all five icons, race status and results HUD: interior native depth ordering passed");
}
static void fixtureTest(const char *directory){
 /* Start enters the actual native pause state machine before the later
  * diagnostic/replay hook. Release remains paused while head motion continues. */
 poseCamera(0,0);fixturePauseTick=90;g_fadeState=FADE_VISIBLE;g_demoMode=DEMO_NONE;
 g_introCountdown=g_postRaceCameraMode=g_pauseLatch=g_isPaused=g_exitRaceFlag=0;g_numViewports=1;g_ghostMaxFrames=0;g_diDeviceReady=1;
 for(int tick=89;tick<=92;tick++){
  ticks=tick;ReadInput();UpdatePerPlayerInput();VR_DiagnosticInput();
  assert(g_isPaused==(tick>=90));assert(VR_PadBits()==(tick==90?PAD_START:0));
 }
 fixturePauseTick=0;g_isPaused=g_pauseLatch=0;
 diagnostic=1;fixturePose=1;anchored=wasFocused=1;modeRace=1;ticks=180;frames=11;begin();VREye saved[2];memcpy(saved,eyes,sizeof saved);
 frames=900;begin();assert(!memcmp(saved,eyes,sizeof saved)); /* Tick match ignores display rate. */
 ticks=181;begin();assert(memcmp(&saved[0].pose,&eyes[0].pose,sizeof(VRPose)));
 assert(memcmp(&eyes[0].pose.q,&eyes[1].pose.q,sizeof(VRQuat)));fixturePose=0;
 /* Dump calls only CPU math; JSON is parsed independently by the runner. */
 char path[1024];snprintf(path,sizeof path,"%s/stream.jsonl",directory);streamFile=fopen(path,"w");assert(streamFile);
 poseCamera(8,0);g_isPaused=1;streamTick=90;VR_TraceStage(2);RenderVertex v={320,240,.1f,.01f,0xffffffff,0,.2f,.3f};
 unsigned before=reads;VR_TraceDraw(&v,1,&s_desired,sizeof s_desired);assert(reads==before);assert(!fclose(streamFile));streamFile=NULL;g_isPaused=0;
 for(int bad=0;bad<5;bad++){
  fflush(NULL);pid_t p=fork();assert(p>=0);if(!p){
   setup(1,0);unsetenv("SONICR_VR_OFFHEAD_RUNTIME_FOV");unsetenv("SONICR_VR_FIXTURE_REPLAY");unsetenv("SONICR_VR_DUMP_STREAM");
   setenv("SONICR_VR_DIAGNOSTIC","1",1);setenv("SONICR_VR_FIXTURE_POSE","1",1);setenv("SONICR_VR_FIXTURE_PAUSE_TICK","90",1);
   if(bad==4){unsetenv("SONICR_VR_DIAGNOSTIC");setenv("SONICR_VR_AUTORACE","1",1);setenv("SONICR_VR_OFFHEAD_RENDER","1",1);setenv("SONICR_VR_OFFHEAD_NEUTRAL","1",1);setenv("SONICR_VR_RUN_SECONDS","20",1);}
   if(bad==1){unsetenv("SONICR_VR_DIAGNOSTIC");unsetenv("SONICR_VR_OFFHEAD_RENDER");}
   if(bad==2)unsetenv("SONICR_VR_FIXTURE_POSE");
   if(bad==3){unsetenv("SONICR_VR_FIXTURE_POSE");unsetenv("SONICR_VR_FIXTURE_PAUSE_TICK");unsetenv("SONICR_VR_DIAGNOSTIC");setenv("SONICR_VR_DUMP_STREAM","bad",1);}
   assert(VR_Init());assert(fixturePose && fixturePauseTick==90 && trackFloat && hudDepth && skyRotation && skyFilter && reflectionFade);VR_Shutdown();
   setenv("SONICR_VR_SKY_FILTER","0",1);setenv("SONICR_VR_REFLECTION_FADE","0",1);
   setenv("SONICR_VR_TRACK_FLOAT","0",1);setenv("SONICR_VR_HUD_DEPTH","0",1);setenv("SONICR_VR_SKY_ROTATION","0",1);assert(VR_Init());assert(!trackFloat && !hudDepth && !skyRotation && !skyFilter && !reflectionFade);
   setenv("SONICR_VR_SKY_FILTER","1",1);setenv("SONICR_VR_REFLECTION_FADE","1",1);
   assert(!skyFilter && !reflectionFade);VR_Shutdown();_exit(0);
  }
  int status;assert(waitpid(p,&status,0)==p && WIFEXITED(status));assert((WEXITSTATUS(status)!=0)==(bad>0 && bad<4));
 }
 puts("pose fixture: actual native input/pause transition; tick-locked combined canted poses; default/legacy controls; unsafe gates rejected; CPU-only stream dump passed");
}
/* The same full track transform and the three actual overlay producers must
 * agree at covered pixel centers, not just at projected quad vertices. */
static void decalDraw(int producer,int legacy,int z){
 int v[12]={-32,0,z+32,32,0,z+32,32,0,z-32,-32,0,z-32};
 if(producer==0){if(legacy)Legacy_SubmitGroundShadow((intptr_t)v);else SubmitGroundShadow((intptr_t)v);}
 if(producer==1){
  memset(g_footShadowCtrl,0,80*4*sizeof(int));g_footShadowCtrl[3]=1;
  for(int i=0;i<4;i++)for(int j=0;j<3;j++)g_footShadowVerts[i*16+5+j]=v[i*3+j];
  if(legacy)Legacy_DrawFootShadows();else DrawFootShadows();
 }
 if(producer==2){if(legacy)Legacy_DrawMovingPickupShadow(0,0,z*4096,32);else DrawMovingPickupShadow(0,0,z*4096,32);}
 R_EndBatch();
}
static int strictlyCovered(unsigned first,unsigned end,float x,float y){
 for(unsigned i=first;i+2<end;i+=3){
  double xx[3],yy[3];for(int j=0;j<3;j++){const Output *v=&events[i+j].vertex;xx[j]=(double)v->xyzw[0]/v->xyzw[3];yy[j]=(double)v->xyzw[1]/v->xyzw[3];}
  double a=(xx[1]-xx[0])*(y-yy[0])-(yy[1]-yy[0])*(x-xx[0]);
  double b=(xx[2]-xx[1])*(y-yy[1])-(yy[2]-yy[1])*(x-xx[1]);
  double c=(xx[0]-xx[2])*(y-yy[2])-(yy[0]-yy[2])*(x-xx[2]);
  if((a>0 && b>0 && c>0)||(a<0 && b<0 && c<0))return 1;
 }
 return 0;
}
static void decalTrackTest(void){
 unsigned samples[2][3]={{0}},fails[2][3]={{0}},culled[2][3]={{0}},nearSamples=0,controls=0,repeats=0;
 for(int i=0;i<4096;i++){g_sinTable[i]=(int)(16384*sin(i*6.283185307179586/4096));g_cosTable[i]=(int)(16384*cos(i*6.283185307179586/4096));}
 for(int near=0;near<2;near++)for(int scale=1;scale<=11;scale+=5)
 for(int frame=0;frame<21;frame++)for(int e=0;e<2;e++)for(int zi=0;zi<(near?3:21);zi++)for(int legacy=0;legacy<2;legacy++){
  depthScale=scale;poseCamera(frame,e);initObject(0);trackFloat=1;
  if(near){
   /* Actual full road crosses the camera plane; overlays stay in front. */
   RenderCamera cam={0};cam.camFloatY=20;cam.yawFloat=.12f;
   memcpy(current,&cam,sizeof cam);memcpy(previous,&cam,sizeof cam);int *vp=current;VR_ApplyCamera(&vp);SetViewportClipRect(vp);
   trackVertices[2].posZ=trackVertices[3].posZ=-32;
  }
  RenderTrackD3D();R_EndBatch();unsigned roadEnd=eventCount;assert(roadEnd>0);
  int z=near?100+zi*60:(1400+zi*200)*scale;
  for(int producer=0;producer<3;producer++){
   eventCount=roadEnd;decalDraw(producer,legacy,z);unsigned end=eventCount;
   if(end==roadEnd){culled[legacy][producer]++;continue;}
   for(unsigned i=roadEnd;i<end;i+=3){
    float x=0,y=0;for(int j=0;j<3;j++){const Output *v=&events[i+j].vertex;assert(v->color[3]>0 && isfinite(v->xyzw[0]) && isfinite(v->xyzw[1]));x+=v->xyzw[0]/v->xyzw[3]/3;y+=v->xyzw[1]/v->xyzw[3]/3;}
    int px=(int)floorf(x*2160/640),py=(int)floorf(y*2160/480);
    for(int oy=-1;oy<=1;oy++)for(int ox=-1;ox<=1;ox++){
     float qx=(px+ox+.5f)*640/2160,qy=(py+oy+.5f)*480/2160;double road,decal;
     if(qx<0 || qx>=640 || qy<0 || qy>=480 || !strictlyCovered(roadEnd,end,qx,qy) || !strictlyCovered(0,roadEnd,qx,qy))continue;
     assert(sampleDepth(0,roadEnd,qx,qy,&road) && sampleDepth(roadEnd,end,qx,qy,&decal));
     samples[legacy][producer]++;fails[legacy][producer]+=decal<=road;nearSamples+=near && !legacy;
    }
   }
  }
 }
 for(int p=0;p<3;p++){
  printf("full track vs %s: covered2160pixel samples=%u old_fail=%u/%u new_fail=%u old_culled=%u new_culled=%u (strict float32 depth; ties fail)\n",p==0?"pickup":p==1?"foot":"moving",samples[0][p],fails[1][p],samples[1][p],fails[0][p],culled[1][p],culled[0][p]);
  assert(samples[0][p]>15000 && fails[1][p]>0 && !fails[0][p]);
 }
 assert(nearSamples>0);
 depthScale=1;
 for(int frame=0;frame<21;frame++)for(int producer=0;producer<3;producer++)for(int control=0;control<3;control++){
  for(int legacy=1;legacy>=0;legacy--){
   poseCamera(frame,0);initObject(0);identity=control==1;eye=control?0:-1;trackFloat=control!=2;
   if(legacy)Legacy_RenderTrackD3D();else RenderTrackD3D();R_EndBatch();decalDraw(producer,legacy,5000);
   if(legacy){referenceCount=eventCount;memcpy(reference,events,eventCount*sizeof *events);}else assert(eventCount==referenceCount && !memcmp(reference,events,eventCount*sizeof *events));
  }controls++;
 }
 trackFloat=1;
 for(int frame=0;frame<21;frame++)for(int producer=0;producer<3;producer++){
  uint64_t saved[2];for(int pass=0;pass<4;pass++){
   int e=pass==0 || pass==3?0:1;poseCamera(frame,e);initObject(0);RenderTrackD3D();R_EndBatch();decalDraw(producer,0,5000);
   uint64_t h=geometryHash(1);if(pass<2)saved[e]=h;else{assert(h==saved[e]);repeats++;}
  }
 }
 printf("decal/full track: near-plane road covered samples=%u native/identity/paired-old exact streams=%u reversed/repeated eyes=%u\n",nearSamples,controls,repeats);
}

#ifndef COMFORT_LIBRARY
int main(int argc,char **argv){assert(argc==2);fullTrackTest();decalTrackTest();nativeTrackTest();trackEdgesTest();pauseTest();statusTest();skyTest();skyAtlasTest();fixtureTest(argv[1]);return 0;}

#endif
