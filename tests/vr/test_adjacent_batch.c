/* Production GL renderer, frame/resource boundaries and VR lifecycle. The GL
 * recorder rejects illegal operations inside Begin and normalizes fans/triangles
 * to the same ordered triangle stream with effective state at every vertex. */
#include "vr/vr.c"
#include "r_gl_backend.c"
#include "render_gl.c"
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include "legacy_draw.h"
static int legacyRenderer;
void *g_tpagePixelBuf[52];
int g_tpageWidth[52],g_tpageHeight[52];
PrecipParticle g_precipParticles[128];
GroundParticle g_particleSlots[16];
typedef struct {
 unsigned enabled,texture,framebuffer,depth,depthFunc,src,dst,env,rgb,source0,source1,operand0,operand1;
 float alpha,scale;double clearDepth;unsigned clipDepth,depthFormat;int scissor[4],viewport[4];double projection[16],modelview[16];
} GLState;
typedef struct {int min,mag,wrapS,wrapT;unsigned generation;} Texture;
typedef struct {float xyzw[4],color[4],uv[2];GLState state;Texture texture;} Output;
typedef struct {int type;Output vertex;} Event;
static double projectionStack[16];static int projectionPushed;
static unsigned boundRenderbuffer,renderbufferFormats[16],storageCalls;
static GLState gpu;static Texture textures[256];static GLenum primitive,matrixMode;
static float color[4],uv[2];static Output fan[128];static unsigned fanCount,triangleIndex;
static Event events[32768],reference[32768];static unsigned eventCount,referenceCount,begins,logicalFans;
static unsigned nextTexture=1,reads,swaps,releaseCount,beginSlots[3],colorCalls;
static int raceProtocol,raceOwned[2],raceDone[2],raceCaptures;
static unsigned pairFinishes,attachedTexture,captureMask,raceEyeFlush[2],raceRenders[3];
static void outside(void){assert(!primitive);}
static unsigned enableBit(GLenum e){switch(e){case GL_TEXTURE_2D:return 1;case GL_DEPTH_TEST:return 2;case GL_BLEND:return 4;case GL_CULL_FACE:return 8;case GL_ALPHA_TEST:return 16;case GL_SCISSOR_TEST:return 32;case GL_FRAMEBUFFER_SRGB:return 64;default:assert(0);return 0;}}
static void record(int type,const Output *v){assert(eventCount<32768);Event *e=events+eventCount++;memset(e,0,sizeof *e);e->type=type;if(v)e->vertex=*v;else {e->vertex.state=gpu;e->vertex.texture=textures[gpu.texture];}}
void glBegin(GLenum p){outside();assert(p==GL_TRIANGLES || p==GL_TRIANGLE_FAN || p==GL_QUADS);primitive=p;fanCount=triangleIndex=0;begins++;beginSlots[eye<0?0:eye+1]++;}
void glEnd(void){assert(primitive);if(primitive==GL_TRIANGLES)assert(triangleIndex%3==0);primitive=0;}
void glVertex4f(GLfloat x,GLfloat y,GLfloat z,GLfloat w){
 assert(primitive);if(raceProtocol && eye>=0)assert(raceOwned[eye] && !raceDone[eye]);Output o={0};o.xyzw[0]=x;o.xyzw[1]=y;o.xyzw[2]=z;o.xyzw[3]=w;
 memcpy(o.color,color,sizeof color);memcpy(o.uv,uv,sizeof uv);o.state=gpu;o.texture=textures[gpu.texture];
 if(primitive==GL_TRIANGLES){record(1,&o);triangleIndex++;}
 else {assert(fanCount<128);fan[fanCount++]=o;if(fanCount>=3){record(1,fan);record(1,fan+fanCount-2);record(1,fan+fanCount-1);}}
}
void glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a){colorCalls++;color[0]=r;color[1]=g;color[2]=b;color[3]=a;}
void glTexCoord2f(GLfloat u,GLfloat v){uv[0]=u;uv[1]=v;}
void glTexCoord2fv(const GLfloat *p){glTexCoord2f(p[0],p[1]);}
void glEnable(GLenum e){outside();gpu.enabled|=enableBit(e);}
void glDisable(GLenum e){outside();gpu.enabled&=~enableBit(e);}
GLboolean glIsEnabled(GLenum e){outside();return (gpu.enabled&enableBit(e))!=0;}
void glBindTexture(GLenum t,GLuint id){outside();assert(t==GL_TEXTURE_2D && id<256);gpu.texture=id;}
void glTexParameteri(GLenum t,GLenum p,GLint v){outside();assert(t==GL_TEXTURE_2D);Texture *a=textures+gpu.texture;switch(p){case GL_TEXTURE_MIN_FILTER:a->min=v;break;case GL_TEXTURE_MAG_FILTER:a->mag=v;break;case GL_TEXTURE_WRAP_S:a->wrapS=v;break;case GL_TEXTURE_WRAP_T:a->wrapT=v;break;default:assert(0);}}
void glTexEnvi(GLenum t,GLenum p,GLint v){outside();assert(t==GL_TEXTURE_ENV);switch(p){case GL_TEXTURE_ENV_MODE:gpu.env=v;break;case GL_COMBINE_RGB:gpu.rgb=v;break;case GL_SOURCE0_RGB:gpu.source0=v;break;case GL_SOURCE1_RGB:gpu.source1=v;break;case GL_OPERAND0_RGB:gpu.operand0=v;break;case GL_OPERAND1_RGB:gpu.operand1=v;break;default:assert(0);}}
void glTexEnvf(GLenum t,GLenum p,GLfloat v){outside();assert(t==GL_TEXTURE_ENV && p==GL_RGB_SCALE);gpu.scale=v;}
static void testClipControl(GLenum origin,GLenum depthMode){outside();assert(origin==GL_LOWER_LEFT);assert(depthMode==GL_ZERO_TO_ONE || depthMode==GL_NEGATIVE_ONE_TO_ONE);gpu.clipDepth=depthMode;}
void *SDL_GL_GetProcAddress(const char *name){outside();assert(!strcmp(name,"glClipControl"));return (void *)testClipControl;}
SDL_bool SDL_GL_ExtensionSupported(const char *name){outside();assert(!strcmp(name,"GL_ARB_clip_control"));return SDL_TRUE;}
void glClearDepth(GLdouble value){outside();gpu.clearDepth=value;}
void glDepthMask(GLboolean b){outside();gpu.depth=b;}
void glDepthFunc(GLenum f){outside();gpu.depthFunc=f;}
void glBlendFunc(GLenum s,GLenum d){outside();gpu.src=s;gpu.dst=d;}
void glAlphaFunc(GLenum f,GLfloat a){outside();assert(f==GL_GREATER);gpu.alpha=a;}
void glScissor(GLint x,GLint y,GLsizei w,GLsizei h){outside();int a[]={x,y,w,h};memcpy(gpu.scissor,a,sizeof a);}
void glViewport(GLint x,GLint y,GLsizei w,GLsizei h){outside();int a[]={x,y,w,h};memcpy(gpu.viewport,a,sizeof a);}
void glClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){outside();(void)r;(void)g;(void)b;(void)a;}
void glClear(GLbitfield b){outside();record((int)b,NULL);}
void glFlush(void){outside();if(raceProtocol && eye>=0)raceEyeFlush[eye]++;}
void glFinish(void){outside();}
void glGenTextures(GLsizei n,GLuint *ids){outside();for(int i=0;i<n;i++)ids[i]=nextTexture++;assert(nextTexture<256);}
void glGenFramebuffers(GLsizei n,GLuint *ids){outside();for(int i=0;i<n;i++)ids[i]=5+i;}
void glGenRenderbuffers(GLsizei n,GLuint *ids){outside();for(int i=0;i<n;i++)ids[i]=8+i;}
void glDeleteTextures(GLsizei n,const GLuint *ids){outside();(void)n;(void)ids;}
void glTexImage2D(GLenum t,GLint l,GLint f,GLsizei w,GLsizei h,GLint b,GLenum ef,GLenum et,const GLvoid *p){outside();(void)t;(void)l;(void)f;(void)w;(void)h;(void)b;(void)ef;(void)et;(void)p;textures[gpu.texture].generation++;record(3,NULL);}
void glTexSubImage2D(GLenum t,GLint l,GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum ty,const GLvoid *p){outside();(void)t;(void)l;(void)x;(void)y;(void)w;(void)h;(void)f;(void)ty;(void)p;textures[gpu.texture].generation++;record(3,NULL);}
void glCopyTexImage2D(GLenum t,GLint l,GLenum f,GLint x,GLint y,GLsizei w,GLsizei h,GLint b){outside();(void)t;(void)l;(void)f;(void)x;(void)y;(void)w;(void)h;(void)b;}
void glBindFramebuffer(GLenum t,GLuint f){outside();(void)t;gpu.framebuffer=f;}
void glFramebufferTexture2D(GLenum t,GLenum a,GLenum tt,GLuint tex,GLint l){outside();(void)t;(void)a;(void)tt;attachedTexture=tex;(void)l;}
void glBindRenderbuffer(GLenum t,GLuint r){outside();(void)t;assert(r<16);boundRenderbuffer=r;}
void glRenderbufferStorage(GLenum t,GLenum f,GLsizei w,GLsizei h){outside();(void)t;(void)w;(void)h;renderbufferFormats[boundRenderbuffer]=f;storageCalls++;}
void glFramebufferRenderbuffer(GLenum t,GLenum a,GLenum rt,GLuint r){outside();(void)t;(void)a;(void)rt;assert(r<16);gpu.depthFormat=renderbufferFormats[r];}
void glDrawBuffer(GLenum b){outside();(void)b;}
void glReadBuffer(GLenum b){outside();(void)b;}
GLenum glCheckFramebufferStatus(GLenum t){outside();(void)t;return GL_FRAMEBUFFER_COMPLETE;}
void glDeleteFramebuffers(GLsizei n,const GLuint *f){outside();(void)n;(void)f;}
void glDeleteRenderbuffers(GLsizei n,const GLuint *r){outside();(void)n;(void)r;}
void glPixelStorei(GLenum p,GLint v){outside();(void)p;(void)v;}
void glReadPixels(GLint x,GLint y,GLsizei w,GLsizei h,GLenum f,GLenum t,GLvoid *p){outside();(void)x;(void)y;(void)f;(void)t;if(raceProtocol && pairedComplete){assert(pairFinishes==1 && !releaseCount && raceOwned[0] && raceOwned[1]);assert(attachedTexture==101 || attachedTexture==102);captureMask|=1u<<(attachedTexture-101);}
 memset(p,0,(size_t)w*h*3);reads++;}
GLenum glGetError(void){outside();return GL_NO_ERROR;}
void glGetIntegerv(GLenum p,GLint *v){outside();(void)p;*v=8;}
void glGetRenderbufferParameteriv(GLenum t,GLenum p,GLint *v){outside();assert(t==GL_RENDERBUFFER && p==GL_RENDERBUFFER_DEPTH_SIZE);*v=renderbufferFormats[boundRenderbuffer]==GL_DEPTH_COMPONENT32F?32:24;}
void glGetFramebufferAttachmentParameteriv(GLenum t,GLenum a,GLenum p,GLint *v){outside();(void)t;(void)p;*v=a==GL_DEPTH_ATTACHMENT?(gpu.depthFormat==GL_DEPTH_COMPONENT32F?GL_FLOAT:GL_UNSIGNED_NORMALIZED):8;}
static double *matrix(void){return matrixMode==GL_PROJECTION?gpu.projection:gpu.modelview;}
void glMatrixMode(GLenum m){outside();assert(m==GL_PROJECTION || m==GL_MODELVIEW);matrixMode=m;}
void glPushMatrix(void){outside();assert(matrixMode==GL_PROJECTION && !projectionPushed);memcpy(projectionStack,gpu.projection,sizeof projectionStack);projectionPushed=1;}
void glPopMatrix(void){outside();assert(matrixMode==GL_PROJECTION && projectionPushed);memcpy(gpu.projection,projectionStack,sizeof projectionStack);projectionPushed=0;}
void glLoadIdentity(void){outside();double *m=matrix();memset(m,0,16*sizeof *m);m[0]=m[5]=m[10]=m[15]=1;}
void glOrtho(GLdouble l,GLdouble r,GLdouble b,GLdouble t,GLdouble n,GLdouble f){outside();double *m=matrix();m[0]=2/(r-l);m[5]=2/(t-b);m[10]=-2/(f-n);m[12]=-(r+l)/(r-l);m[13]=-(t+b)/(t-b);m[14]=-(f+n)/(f-n);}
void glTranslatef(GLfloat x,GLfloat y,GLfloat z){outside();double *m=matrix();m[12]+=x;m[13]+=y;m[14]+=z;}
void glScalef(GLfloat x,GLfloat y,GLfloat z){outside();double *m=matrix();m[0]*=x;m[5]*=y;m[10]*=z;}
void platform_get_drawable_size(int *w,int *h){*w=640;*h=480;}
void platform_gl_swap(void){outside();swaps++;}
unsigned vr_xr_acquire(int e){outside();if(raceProtocol){assert(!raceOwned[e]);raceOwned[e]=1;raceDone[e]=0;}return 101+e;}
int vr_xr_complete_pair(void){outside();assert(raceProtocol && pairedComplete && raceOwned[0] && raceOwned[1] && !releaseCount && !reads && raceEyeFlush[0] && raceEyeFlush[1]);raceDone[0]=raceDone[1]=1;pairFinishes++;return 1;}
void vr_xr_release(int e){outside();if(raceProtocol){assert(raceOwned[e]);if(pairedComplete)assert(raceDone[e]);raceOwned[e]=0;}releaseCount++;}
void vr_xr_shutdown(void){outside();}
int vr_xr_init(void){outside();return 1;}
int vr_xr_runtime_fov(int enabled){return !enabled;}
void vr_xr_deadline(unsigned seconds,int offhead,int neutral){(void)seconds;(void)offhead;(void)neutral;}
int SDL_GL_SetSwapInterval(int interval){outside();(void)interval;return 0;}
void vr_xr_end(int r){outside();if(raceProtocol){assert(raceRenders[0]==1 && raceRenders[1]==1 && raceRenders[2]==1);assert(r==1 && !raceOwned[0] && !raceOwned[1] && releaseCount==2);assert(reads==(raceCaptures?2u:0u));if(pairedComplete)assert(pairFinishes==1 && captureMask==(raceCaptures?3u:0u));}}
int vr_xr_begin(VREye e[2],int64_t *t){(void)e;*t+=11111111;return 1;}
int vr_xr_focused(void){return 1;}
int vr_xr_offhead(void){return 0;}
void vr_input_tick(void){}
Uint64 SDL_GetPerformanceCounter(void){return 100;}
Uint64 SDL_GetPerformanceFrequency(void){return 1000;}
static void draw(int n){
 RenderVertex v[7];assert(n<=7);
 for(int i=0;i<n;i++)v[i]=(RenderVertex){120+i*17.125f,90+i*i*3.375f,.1f+i*.03f,1.0f/(10+i*13.125f),0x7fa1b2c3u+i*0x01020304u,0,i*.125f,i*.0625f};
 if(legacyRenderer)Legacy_DrawTriFan(v,n);else R_DrawTriFan(v,n);logicalFans++;
}
void RenderHUD(void){if(raceProtocol)raceRenders[eye<0?0:eye+1]++;BeginFrame();draw(4);EndFrame();}
void SetViewportClipRect(int *cam){assert(cam);}
static void setup(int enabled,int mode){
 if(depthReversed)setDepthMode(0);
 R_EndBatch();memset(&gpu,0,sizeof gpu);gpu.clipDepth=GL_NEGATIVE_ONE_TO_ONE;gpu.clearDepth=1;
 perspectiveDepth=0;s_vrDepthReversed=0;clipControl=testClipControl;memset(textures,0,sizeof textures);memset(color,0,sizeof color);memset(uv,0,sizeof uv);
 memset(s_glTextureFilter,0,sizeof s_glTextureFilter);memset(s_tpageFilter,0,sizeof s_tpageFilter);memset(s_glTextureDirty,0,sizeof s_glTextureDirty);
 memset(g_tpageStateArray,0,52*sizeof *g_tpageStateArray);memset(s_tpageNoColorKey,0,sizeof s_tpageNoColorKey);memset(s_tpageGreen6,0,sizeof s_tpageGreen6);
 s_glTexturesInited=1;nextTexture=53;for(int i=0;i<52;i++){s_glTextures[i]=i+1;g_tpageWidth[i]=g_tpageHeight[i]=2;}
 static unsigned short pixels[4]={0x1234,0x3456,0x5678,0x789a};g_tpagePixelBuf[0]=pixels;g_tpagePixelBuf[1]=pixels;
 eventCount=begins=logicalFans=reads=swaps=releaseCount=colorCalls=0;memset(beginSlots,0,sizeof beginSlots);
 perfEnabled=1;modeRace=1;memset(perfSubmits,0,sizeof perfSubmits);memset(perfVertices,0,sizeof perfVertices);
 traceStart();R_SetVRBatching(enabled);R_ResetState();g_screenWidth=640;g_screenHeight=480;g_screenHeightFull=480;
 g_screenCenterX=320;g_screenCenterY=240;g_projScaleXCurrent=g_projScaleY=320;g_farClipFloat=32768;
 initialized=1;diagnostic=0;identity=mode==2;eye=mode?0:-1;framebuffer=5;
 depth[0]=8;depth[1]=9;memset(depthFormat,0,sizeof depthFormat);
 for(int e=0;e<2;e++){eyes[e]=(VREye){.pose={{0,0,0,1},{e?.032f:-.032f,0,0}},.fov={-.8f,.7f,.6f,-.65f},.width=8,.height=6};}
 if(eye>=0){cacheEyeProjection();}layer=0;roll=0;rollCos=1;rollSin=0;
}
static void raceProtocolTest(int paired,int captures,int reverse){
 fflush(NULL);pid_t child=fork();assert(child>=0);
 if(!child){
  setup(1,1);perspectiveDepth=reverse;raceProtocol=1;pairedComplete=paired;raceCaptures=captures;pairFinishes=captureMask=0;
  memset(raceOwned,0,sizeof raceOwned);memset(raceDone,0,sizeof raceDone);
  memset(raceEyeFlush,0,sizeof raceEyeFlush);memset(raceRenders,0,sizeof raceRenders);
  eye=-1;runtimeDump=captures;ticks=1;dumpTick=2;stereoCheck=0;haveCamera=1;
  g_viewportArray[0]=0;VR_RenderRace(1);outside();assert(!captures);
  assert(releaseCount==2 && !reads && pairFinishes==(unsigned)paired);
  assert(!depthReversed && !projectionPushed && gpu.clipDepth==GL_NEGATIVE_ONE_TO_ONE && gpu.clearDepth==1);VR_Shutdown();_exit(0);
 }
 int status;assert(waitpid(child,&status,0)==child);assert(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
static void scenario(int enabled,int mode){
 setup(enabled,mode);BeginFrame();R_SetTexture(0);R_SetTexEnv(R_TEXENV_MODULATE);
 for(int i=0;i<30;i++)draw(3+i%5);
 R_PushState();R_SetBlendMode(R_BLEND_ADDITIVE);draw(4);draw(3);R_PopState();draw(5);
 R_SetDepthWrite(0);draw(4);R_SetDepthTest(0);draw(3);R_SetDepthFunc(R_DEPTH_ALWAYS);draw(4);
 R_SetAlphaTest(0);draw(3);R_SetAlphaRef(.3f);draw(4);R_SetCullMode(R_CULL_FRONT);draw(3);
 R_SetTexEnv(R_TEXENV_ADD_SIGNED);draw(4);R_SetScissor(1,2,30,40);draw(4);R_DisableScissor();draw(3);
 R_SetFilter(R_FILTER_LINEAR);draw(4);R_SetTpageFilter(0,R_FILTER_NEAREST);draw(4);R_ClearTpageFilter(0);draw(3);
 R_SetTexture(1);draw(4);R_SetTexture(-1);draw(4);R_SetTexture(0);draw(4);
 R_MarkTextureDirty(0);draw(4);draw(4);GL_UploadTpage(0);draw(3);
 unsigned char rgba[16]={0};GL_UploadTpageRGBA(0,rgba,2,2);draw(4);GL_UploadTpageSubRect(0,0,0,1,1);draw(3);
 draw(3);R_SetVRBatching(0);outside();draw(4);R_SetVRBatching(enabled);draw(5);
 R_FlushState();outside();draw(4);R_ClearDepth();draw(3);ProcessTpageStates();draw(4);RenderBackground();draw(4);
 R_ResetState();draw(3);BeginFrame();draw(4);SetViewportFromConfig(NULL);outside();draw(3);
 if(mode){
  VR_Layer(1);outside();draw(4);VR_SkyBegin();outside();draw(4);VR_SkyEnd();outside();draw(4);
  int *cam=current;VR_ApplyCamera(&cam);outside();draw(4);
  attachment("batch-fixture",1);outside();draw(4);imageFingerprint(0);outside();draw(3);
  capture("batch-fixture",2,2);outside();draw(4);release();outside();target(1);draw(3);
  target(0);outside();draw(4);
 }
 EndFrame();outside();draw(3);presented=mirrorDirty=1;FlipD3D();outside();draw(4);
 /* Explicit resource initialization and shutdown barriers, even if invoked
  * after geometry rather than their usual startup/end-of-frame positions. */
 s_glTexturesInited=0;GL_InitTextures();outside();R_ResetState();draw(4);VR_Shutdown();outside();
 assert(reads==(mode?2u:0u));assert(swaps==1);assert(releaseCount==(mode?1u:0u));
 traceDraw=0;
}
/* Compare the extracted pre-optimization emitter, not the candidate emitter
 * shared by both fan paths. Exercise each color byte, uniform faces, gradients,
 * zero/negative RHW and external GL color changes between batch boundaries. */
static void colorScenario(int enabled,int mode){
 setup(enabled,mode);BeginFrame();R_SetTexture(0);
 for(unsigned c=0;c<256;c++){
  RenderVertex v[4];
  for(int i=0;i<4;i++)v[i]=(RenderVertex){100+i*23.125f,80+i*i*6.375f,.2f+i*.03f,
    i==0?0:i==1?-1:1.0f/(31+i*7.125f),(c<<24)|((255-c)<<16)|(((c*17)&255)<<8)|((c*7)&255),0,i*.125f,i*.25f};
  /* The shared packed color must include alpha, including fully zero ARGB. */
  if(!c)for(int i=0;i<4;i++)v[i].color=0;
  for(int repeat=0;repeat<3;repeat++){
   if(legacyRenderer)Legacy_DrawTriFan(v,4);else R_DrawTriFan(v,4);
  }
  for(int i=0;i<4;i++)v[i].color^=(uint32_t)i*0x01010101u;
  if(legacyRenderer)Legacy_DrawTriFan(v,4);else R_DrawTriFan(v,4);
  R_EndBatch();assert(!s_batchColorValid);
  glColor4f(.125f,.25f,.5f,.75f); /* raw menu/external color is not cacheable */
  if(legacyRenderer)Legacy_DrawTriFan(v,4);else R_DrawTriFan(v,4);
  R_EndBatch();assert(!s_batchColorValid);
 }
 EndFrame();outside();traceDraw=0;
}
static void colorTest(void){
 for(int mode=0;mode<3;mode++){
  legacyRenderer=1;colorScenario(0,mode);referenceCount=eventCount;
  memcpy(reference,events,eventCount*sizeof *events);unsigned oldColors=colorCalls;
  legacyRenderer=0;colorScenario(0,mode);
  assert(colorCalls==oldColors && eventCount==referenceCount && !memcmp(reference,events,eventCount*sizeof *events));
  colorScenario(1,mode);
  assert(eventCount==referenceCount && !memcmp(reference,events,eventCount*sizeof *events));
  if(mode==1)assert(colorCalls<oldColors);else assert(colorCalls==oldColors);
  printf("batch color production mode=%s legacy_calls=%u candidate_calls=%u exact_xyzw_color_uv_state=1 external_color_barriers=1\n",mode==0?"native":mode==1?"eye":"identity",oldColors,colorCalls);
 }
}
int main(int argc,char **argv){
 assert(argc==2);dumpDir=argv[1];
 colorTest();
 for(int mode=0;mode<3;mode++){
  legacyRenderer=1;scenario(0,mode);referenceCount=eventCount;memcpy(reference,events,eventCount*sizeof *events);
  unsigned oldBegins=begins,oldFans=logicalFans;uint64_t oldHash=drawHash;unsigned oldCount=drawCount;
  legacyRenderer=0;scenario(0,mode);
  assert(begins==oldBegins && eventCount==referenceCount && !memcmp(reference,events,eventCount*sizeof *events));
  scenario(1,mode);
  assert(eventCount==referenceCount && oldFans==logicalFans && drawHash==oldHash && drawCount==oldCount);
  if(memcmp(reference,events,eventCount*sizeof *events)){
   for(unsigned i=0;i<eventCount;i++)if(memcmp(reference+i,events+i,sizeof *events)){fprintf(stderr,"event mismatch mode=%d event=%u type=%d/%d\n",mode,i,reference[i].type,events[i].type);break;}
   assert(0);
  }
  if(mode==1)assert(begins<oldBegins);else assert(begins==oldBegins);
  unsigned submits=perfSubmits[0]+perfSubmits[1]+perfSubmits[2];assert(submits==begins);
  assert(!memcmp(perfSubmits,beginSlots,sizeof beginSlots));
  printf("batch production legacy+optout mode=%s fans=%u events=%u old_submits=%u new_submits=%u exact_triangles_state=1 legal_gl=1\n",mode==0?"native":mode==1?"eye":"identity",logicalFans,eventCount,oldBegins,begins);
 }
 /* Pending race geometry must close before the menu copy and its direct
  * GL_QUADS block. This path intentionally has no backend fan counter. */
 setup(1,1);pairedComplete=1;BeginFrame();draw(4);presented=0;anchored=wasFocused=1;clockState=(VRClock){0};
 assert(VR_Present()==1);outside();assert(releaseCount==2);VR_Shutdown();
 puts("batch production menu transition: pending eye geometry closed before copy and direct quad draws");
 /* Test the actual environment route, not only its backend setter. */
 setenv("SONICR_VR_DIAGNOSTIC","1",1);unsetenv("SONICR_VR_OFFHEAD_RENDER");unsetenv("SONICR_VR_OFFHEAD_NEUTRAL");
 unsetenv("SONICR_VR_PAIRED_COMPLETE");unsetenv("SONICR_VR_DEPTH");unsetenv("SONICR_VR_OFFHEAD_RUNTIME_FOV");
 setenv("SONICR_VR_BATCH","0",1);assert(VR_Init());assert(!batching && !s_vrBatching && pairedComplete && perspectiveDepth);VR_Shutdown();
 unsetenv("SONICR_VR_BATCH");assert(VR_Init());assert(batching && s_vrBatching && perspectiveDepth);VR_Shutdown();
 puts("batch production environment: explicit 0 disables batching; unset batching and depth default on");
 setenv("SONICR_VR_PAIRED_COMPLETE","1",1);assert(VR_Init());assert(pairedComplete);VR_Shutdown();
 setenv("SONICR_VR_PAIRED_COMPLETE","0",1);assert(VR_Init());assert(!pairedComplete);VR_Shutdown();
 setenv("SONICR_VR_DEPTH","1",1);assert(VR_Init());assert(perspectiveDepth);VR_Shutdown();
 setenv("SONICR_VR_DEPTH","0",1);assert(VR_Init());assert(!perspectiveDepth);VR_Shutdown();
 pid_t bad=fork();assert(bad>=0);
 if(!bad){setenv("SONICR_VR_OFFHEAD_RUNTIME_FOV","1",1);VR_Init();_exit(0);}
 int status;assert(waitpid(bad,&status,0)==bad);assert(WIFEXITED(status) && WEXITSTATUS(status)!=0);
 puts("runtime FOV production startup: invalid diagnostic/neutral flag combination rejected");
 dumpDir=argv[1];
 for(int paired=0;paired<2;paired++)for(int captures=0;captures<2;captures++)for(int reverse=0;reverse<2;reverse++)raceProtocolTest(paired,captures,reverse);
 puts("race completion: actual render loop default/opt-out, no normal readbacks, paired captures after shared finish before either release passed");
 return 0;
}
