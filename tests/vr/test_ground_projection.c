/* Real ground/track emitters, with only the final draw boundary recorded. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "r_state.h"
#include "r_draw.h"
/* Exercise the production near-clipping/track emission without exporting
 * otherwise-private functions just for the fixture. */
#include "render_track_sdl.c"
int g_farClipTimes8;
float g_uvLUT256[256];
extern int g_footShadowVerts[],g_renderStateBlock[];
int g_particleOwner[16],g_particleAge[16],g_qtEdgeIdx,g_qtSurfIdx;
GroundParticle g_particleSlots[16];
extern void SetupCharacterShadowQuad(Player *);
static int active=1,repeat=1,count;
static RenderVertex draws[4096][5];static int sizes[4096];
typedef struct {int tex,env,blend,depth,filter;} State;
static State state,stack[4],states[4096];static int stackCount;
int VR_EyeActive(void){return active;}
int VR_Repeat(void){return repeat;}
int QueryTerrainHeight(int x,int z,int y){(void)x;(void)z;(void)y;return 0;}
void R_SetTexEnv(R_TexEnvMode m){state.env=m;}
void R_SetTexture(int t){state.tex=t;}
void R_SetDepthWrite(int d){state.depth=d;}
void R_SetFilter(R_FilterMode f){state.filter=f;}
void R_SetBlendMode(R_BlendMode b){state.blend=b;}
void R_PushState(void){assert(stackCount<4);stack[stackCount++]=state;}
void R_PopState(void){assert(stackCount);state=stack[--stackCount];}
void R_DrawTriFan(const RenderVertex *v,int n){assert(count<4096 && n<=5);memcpy(draws[count],v,n*sizeof(*v));sizes[count]=n;states[count++]=state;}
void R_DrawQuad(const RenderVertex *v){R_DrawTriFan(v,4);}
void R_DrawTri(const RenderVertex *v){R_DrawTriFan(v,3);}
static void camera(int x,int z,int center){
 g_camIntX=x;g_camIntY=200;g_camIntZ=z;
 g_camOrientX=x*4096;g_camOrientY=200*4096;g_camOrientZ=z*4096;
 memset(g_viewMtx,0,sizeof(int)*16);g_viewMtx[0]=g_viewMtx[5]=g_viewMtx[10]=4096;
 g_screenCenterX=center;g_screenCenterY=240;g_projScaleXCurrent=g_projScaleY=320;
 g_clipLeft=g_clipTop=0;g_clipRight=639;g_clipBottom=479;count=0;
}
static float area(const RenderVertex *v){return (v[1].sx-v[0].sx)*(v[2].sy-v[0].sy)-(v[1].sy-v[0].sy)*(v[2].sx-v[0].sx);}
static uint64_t hash(void){
 uint64_t h=1469598103934665603ULL;
 for(int i=0;i<count;i++)for(int j=0;j<sizes[i];j++){
  RenderVertex v=draws[i][j];v.specular=0;const unsigned char *p=(void*)&v;
  for(size_t k=0;k<sizeof v;k++)h=(h^p[k])*1099511628211ULL;
 }
 return h;
}
static int collapsed[5];
static uint64_t scene(int x,int z,int center,int record){
 camera(x,z,center);
 /* Correct camera-facing winding. Token half-size 30 is native metadata. */
 int v[12]={-30,0,5030, 30,0,5030, 30,0,4970, -30,0,4970};
 SubmitGroundShadow((intptr_t)v);
 int ends[5];ends[0]=count;
 memset(g_footShadowCtrl,0,80*4*sizeof(int));g_footShadowCtrl[3]=1;
 for(int i=0;i<4;i++)for(int j=0;j<3;j++)g_footShadowVerts[i*16+5+j]=v[i*3+j];
 DrawFootShadows();ends[1]=count;
 Player player={0};player.posZ=5000*4096;player.surfNormY=-4096;
 SetupCharacterShadowQuad(&player);ends[2]=count;
 DrawMovingPickupShadow(0,0,5000*4096,30);ends[3]=count;
 memset(g_renderStateBlock,0,16*sizeof(int));g_renderStateBlock[0]=1;
 for(int i=0;i<4;i++){g_particleSlots[0].corners[i].x=v[i*3];g_particleSlots[0].corners[i].z=v[i*3+2];}
 DrawGroundParticlesD3D(0);ends[4]=count;
 for(int i=0,prev=0;i<5;i++){
  if(record && (ends[i]!=prev+1 || fabsf(area(draws[prev]))<0.00001f))collapsed[i]++;
  if(ends[i]==prev+1)for(int j=0;j<4;j++)assert((draws[prev][j].color>>24)>0);
  prev=ends[i];
 }
 return hash();
}
/* Sample the actual emitted triangle fan using the GL depth interpolation
 * rule. This catches a decal whose projection puts its depth behind the track
 * beneath it, without substituting a duplicated shadow/track implementation. */
static int depth_at(const RenderVertex *v,int n,float x,float y,float *depth){
 for(int i=1;i<n-1;i++){
  const RenderVertex *a=v,*b=v+i,*c=v+i+1;
  float d=(b->sy-c->sy)*(a->sx-c->sx)+(c->sx-b->sx)*(a->sy-c->sy);
  if(fabsf(d)<1e-8f)continue;
  float wa=((b->sy-c->sy)*(x-c->sx)+(c->sx-b->sx)*(y-c->sy))/d;
  float wb=((c->sy-a->sy)*(x-c->sx)+(a->sx-c->sx)*(y-c->sy))/d;
  float wc=1-wa-wb;
  if(wa>=-1e-5f && wb>=-1e-5f && wc>=-1e-5f){*depth=wa*a->sz+wb*b->sz+wc*c->sz;return 1;}
 }
 return 0;
}
static void track_depth_test(void){
 int samples=0,occluded=0,missing=0;
 for(int f=0;f<81;f++)for(int nearClip=0;nearClip<2;nearClip++){
  camera(f-40,0,320);
  if(nearClip)g_camIntY=20;
  int z=nearClip?150:5000, extent=nearClip?40:1000;
  int ground[12]={-30,0,z+30,30,0,z+30,30,0,z-30,-30,0,z-30};
  NearClipVert track[4]={0};
  int xyz[4][2]={{-1000,z+extent},{1000,z+extent},{1000,z-extent},{-1000,z-extent}};
  if(nearClip)xyz[2][1]=xyz[3][1]=-10;
  for(int i=0;i<4;i++){
   track[i].camX=xyz[i][0]-g_camIntX;track[i].camY=-g_camIntY;track[i].depth=xyz[i][1];
   track[i].colorR=track[i].colorG=track[i].colorB=255<<13;
  }
  /* Two-sided flag avoids using the irrelevant pre-clip screen scratch. */
  assert(TrackClipAndEmitQuad(track,4,g_farClipFloat,4)>0);
  assert(count==1);RenderVertex terrain[5];int n=sizes[0];memcpy(terrain,draws[0],n*sizeof(*terrain));
  SubmitGroundShadow((intptr_t)ground);
  if(count!=2 || fabsf(area(draws[1]))<1e-5f){missing++;continue;}
  for(int i=1;i<3;i++){
   float x=(draws[1][0].sx+draws[1][i].sx+draws[1][i+1].sx)/3;
   float y=(draws[1][0].sy+draws[1][i].sy+draws[1][i+1].sy)/3;
   float surface,decal;
   assert(depth_at(terrain,n,x,y,&surface));assert(depth_at(draws[1],4,x,y,&decal));
   if(decal>=surface)occluded++;
   samples++;
  }
 }
 printf("production track/decal depth: samples=%d depth_rejects=%d missing_or_degenerate=%d (grazing + clipped track)\n",samples,occluded,missing);fflush(stdout);
 if(active)assert(!occluded && !missing);
}
static void water_state_test(void){
 camera(0,0,320);g_viewportHeight=g_screenHeight=480;g_screenWidthFull=640;
 g_d3dSurfaceMode=4;g_parallaxWidth=1664;g_parallaxState[0]=240;g_parallaxState[1]=512;
 uint64_t first=0;
 for(int pass=0;pass<3;pass++){
  count=0;g_totalFrames=pass==2?17:0;
  State incoming={-1,pass?R_TEXENV_ADD_SIGNED:R_TEXENV_MODULATE,pass?R_BLEND_ADDITIVE:R_BLEND_ALPHA,1,R_FILTER_NEAREST};
  state=incoming;RenderWaterReflectionD3D(0);assert(count>10);
  assert(!memcmp(&state,&incoming,sizeof state) && stackCount==0);
  int textured=0,tinted=0;
  for(int i=0;i<count;i++){
   if(states[i].tex==g_tpageCount){
    textured++;assert(states[i].env==R_TEXENV_MODULATE && states[i].blend==R_BLEND_NONE && !states[i].depth);
   }else if(states[i].tex==-1){tinted++;assert(states[i].blend==R_BLEND_ADDITIVE && !states[i].depth);}
  }
  assert(textured>0 && tinted>0);
  if(pass==0)first=hash();else if(pass==1)assert(first==hash());else assert(first!=hash());
 }
 puts("water: textured reflection + additive tint; incoming-state independent; animation changes geometry; state restored");
}
static void native_cull_test(void){
 camera(0,0,320);
 /* Products8191*8193 and8192*8192 fit int32 and differ by-1,
  * but single-precision multiplication rounds both to the same value. */
 int v[12]={8192,200-8191,320, 0,200,320,
            8193,200-8192,320, 16385,200-16383,320};
 SubmitGroundShadow((intptr_t)v);assert(count==0);
 puts("native near-collinear negative-area cull passed");
}
static void particle_state_test(void){
 /* Eye A culls behind its near plane; eye B still sees the same live slot. */
 int x[4]={-30,30,30,-30},z[4]={130,130,70,70};
 for(int i=0;i<4;i++){g_particleSlots[0].corners[i].x=x[i];g_particleSlots[0].corners[i].z=z[i];}
 camera(0,200,280);g_renderStateBlock[0]=1;g_particleOwner[0]=0;repeat=1;
 DrawGroundParticlesD3D(0);assert(count==0 && g_renderStateBlock[0]==1);
 camera(0,0,360);g_camIntY=20;DrawGroundParticlesD3D(0);assert(count==1 && area(draws[0])!=0);
 camera(0,200,280);repeat=0;DrawGroundParticlesD3D(0);assert(g_renderStateBlock[0]==0);repeat=1;
 puts("ground particle canonical/repeat ownership passed");
}
int main(int argc,char **argv){
 active=argc==1 || strcmp(argv[1],"--flat");
#if !SONICR_VR
 active=0;
#endif
 g_farClipFloat=88064;g_farClipDepth=11008;g_farClipTimes8=88064;
 g_tpageCharBase=1;g_tpageCount=2;g_tpageParallax1=3;
 g_tpageStateArray[1]=g_tpageStateArray[2]=g_tpageStateArray[3]=4;
 g_modelMeta[0].lightGroup=30;
 for(int i=0;i<5120;i++)g_sinTable[i]=(int)(sinf(i*6.28318530718f/4096)*4096);
 /* g_cosTable aliases the sine table with a quarter-turn offset. */
 for(int i=0;i<256;i++)g_uvLUT256[i]=(float)i/256;
 if(argc>1 && !strcmp(argv[1],"--water")){water_state_test();return 0;}
 if(argc>1 && !strcmp(argv[1],"--particle")){particle_state_test();return 0;}
 uint64_t temporal=0;
 for(int f=0;f<81;f++){
  uint64_t l=scene(f-43,(f-40)*50,280,1),r=scene(f-37,(f-40)*50,360,1);
  assert(scene(f-37,(f-40)*50,360,0)==r);assert(scene(f-43,(f-40)*50,280,0)==l);
  temporal=(temporal^l)*1099511628211ULL;temporal=(temporal^r)*1099511628211ULL;
 }
 printf("ground temporal: 81 frames 324 passes flat=%d hash=%016llx collapse(pickup,foot,character,moving,water)=%d,%d,%d,%d,%d /162\n",!active,(unsigned long long)temporal,collapsed[0],collapsed[1],collapsed[2],collapsed[3],collapsed[4]);fflush(stdout);
 native_cull_test();
 track_depth_test();
 if(!active)return 0;
 if(active)for(int i=0;i<5;i++)assert(!collapsed[i]);
 particle_state_test();
 water_state_test();
 return 0;
}
