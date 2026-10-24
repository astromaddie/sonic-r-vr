/* Real weather initialization and draw paths, recorded at the draw boundary. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "sonicr_types.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "r_state.h"
#include "r_draw.h"
extern void InitWeather(void);
extern void DrawSnowParticlesGate(int);
extern OtherParticle g_particleArray[64];
float g_uvLUT256[256];
static int eye=1,repeat=1,count,collapsed,depth=1,blend,env,texture;
static int oldDepth,oldBlend,oldEnv,oldTexture;
static RenderVertex draws[256][4];
int VR_EyeActive(void){return eye;}
int VR_Repeat(void){return repeat;}
void VR_PerfWeather(int start){(void)start;}
int Random(void){return 8192;}
void DebugLog(char *fmt,...){(void)fmt;}
void R_SetTexture(int t){texture=t;}
void R_SetDepthWrite(int d){depth=d;}
void R_SetTexEnv(R_TexEnvMode e){env=e;}
void R_SetBlendMode(R_BlendMode b){blend=b;}
void R_PushState(void){oldDepth=depth;oldBlend=blend;oldEnv=env;oldTexture=texture;}
void R_PopState(void){depth=oldDepth;blend=oldBlend;env=oldEnv;texture=oldTexture;}
void R_DrawQuad(const RenderVertex *v){
 assert(count<256);assert((v[0].color>>24)>0);
 memcpy(draws[count++],v,4*sizeof(*v));
 if(v[0].sx==v[1].sx || v[0].sy==v[3].sy)collapsed++;
}
static uint64_t hash(void){
 uint64_t h=1469598103934665603ULL;
 for(int i=0;i<count;i++)for(int j=0;j<4;j++){
  RenderVertex v=draws[i][j];v.specular=0;unsigned char *b=(void*)&v;
  for(unsigned k=0;k<sizeof v;k++)h=(h^b[k])*1099511628211ULL;
 }
 return h;
}
static uint64_t render(int x,int center,int ambient){
 g_camIntX=x;g_screenCenterX=center;count=0;
 if(ambient)DrawOtherParticles();else {
  int d=depth,b=blend,e=env,t=texture;
  DrawSnowParticlesGate(0);
  assert(depth==d && blend==b && env==e && texture==t);
 }
 assert(count==1);return hash();
}
int main(int argc,char **argv){
 int flat=argc>1 && !strcmp(argv[1],"--flat");if(flat)eye=0;
 g_trackId=TRACK_RESORT_ISLAND;g_raceType=RACE_TYPE_GP;g_raceSubMode=SUBMODE_NORMAL;
 g_weatherType=WEATHER_SNOW;g_numHumans=1;g_tpageCharBase=1;
 g_tpageStateArray[1]=g_tpageStateArray[g_tpageParticle1]=4;
 InitWeather();assert(g_precipVpRange[0]==0 && g_precipVpRange[1]==128);
 for(int i=0;i<128;i++)assert(g_precipParticles[i].state==1 && g_precipParticles[i].sizeW==8 && g_precipParticles[i].tpage==g_tpageParticle1);
 for(int i=1;i<128;i++){g_precipParticles[i].posY=-1;g_precipParticles[i].state=0;}
 for(int i=0;i<256;i++)g_uvLUT256[i]=i/256.0f;
 memset(g_viewMtx,0,16*sizeof(int));g_viewMtx[0]=g_viewMtx[5]=g_viewMtx[10]=4096;
 g_camIntY=100;g_camIntZ=0;g_screenCenterY=240;
 g_projScaleXCurrent=g_projScaleY=320;g_farClipDepth=4096;g_farClipFloat=32768;
 g_clipLeft=g_clipTop=0;g_clipRight=639;g_clipBottom=479;
 PrecipParticle *p=g_precipParticles;p->posY=100*256;p->posZ=8000*256;
 OtherParticle *o=g_particleArray;o->active=o->lifetime=1;o->width=o->height=8;
 o->posY=p->posY;o->posZ=p->posZ;o->tpage=g_tpageParticle1;o->uvStep=32;o->flags=0x10;
 uint64_t stream=0;int cases=0;
 for(int f=0;f<81;f++)for(int kind=0;kind<2;kind++){
  /* Moving camera and atlas phase; each same-eye replay sees one simulation state. */
  p->uvBaseX=(unsigned char)(192+(f%2)*32);o->uvX=p->uvBaseX;
  PrecipParticle before[128];memcpy(before,g_precipParticles,sizeof before);
  uint64_t a=render(f-40,300,kind),b=render(f-35,340,kind);
  assert(a==render(f-40,300,kind));assert(b==render(f-35,340,kind));
  assert(memcmp(before,g_precipParticles,sizeof before)==0);
  stream^=a+((uint64_t)f<<32)+b;cases+=4;
 }
 int failures=collapsed;
 /* Canonical visibility owns state; an offscreen repeated eye cannot delete it. */
 p->posZ=-256;count=0;DrawSnowParticlesGate(0);assert(count==0);
#if SONICR_VR
 assert(p->state==1);
#else
 assert(p->state==0);
#endif
 repeat=0;DrawSnowParticlesGate(0);assert(p->state==0);repeat=1;
 /* Native race gating, plus rain initialized through the same production entry. */
 g_raceType=RACE_TIMEATTACK;g_raceSubMode=SUBMODE_NORMAL;count=0;DrawSnowParticlesGate(0);assert(!count);
 g_raceType=RACE_TYPE_GP;g_weatherType=WEATHER_RAIN;InitWeather();assert(p->sizeW==6);
 for(int i=1;i<128;i++)g_precipParticles[i].posY=-1;
 p->posY=100*256;p->posZ=8000*256;
 uint64_t rain=render(0,300,0);assert(rain==render(0,300,0));
 printf("weather production passes=%d collapsed=%d stream=%016llx mode=%s native_state=owned init=snow+rain\n",cases,failures,(unsigned long long)stream,flat?"flat":"eye");fflush(stdout);
#if SONICR_VR
 if(!flat)assert(failures==0);
#endif
 return 0;
}
