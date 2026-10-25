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
static int legacyRenderer,fixtureArrays;
void *g_tpagePixelBuf[52];
int g_tpageWidth[52],g_tpageHeight[52];
int g_farClipTimes8;
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
static unsigned nextTexture=1,reads,swaps,releaseCount,beginSlots[3],colorCalls,flushCalls,finishCalls;
static unsigned arrayDraws;
static GLState textureAttribState;static int textureAttribPushed;
static GLuint waterTextureId;static unsigned waterUploadCount,waterUploadLevels,waterDeleteCount;
static uint64_t waterUploadHash=1469598103934665603ULL;
static int waterBaseMin=255,waterBaseMax;
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
/* Model client state separately from server state. A draw deliberately poisons
 * current color/UV, as the GL specification permits, to enforce restoration. */
typedef struct {GLint size;GLenum type;GLsizei stride;const void *pointer;GLuint buffer;} ArrayPointer;
typedef struct {unsigned enabled;GLenum unit;GLuint buffer;ArrayPointer vertex,color,tex[4];} ClientState;
static ClientState client,savedClient;
static int clientPushed;
static unsigned clientBit(GLenum cap){switch(cap){case GL_VERTEX_ARRAY:return 1;case GL_COLOR_ARRAY:return 2;case GL_TEXTURE_COORD_ARRAY:return 4u<<(client.unit-GL_TEXTURE0);default:assert(0);return 0;}}
void glPushClientAttrib(GLbitfield bits){outside();assert(bits==GL_CLIENT_VERTEX_ARRAY_BIT && !clientPushed);savedClient=client;clientPushed=1;}
void glPopClientAttrib(void){outside();assert(clientPushed);client=savedClient;clientPushed=0;}
void glBindBuffer(GLenum target,GLuint buffer){outside();assert(target==GL_ARRAY_BUFFER);client.buffer=buffer;}
void glClientActiveTexture(GLenum unit){outside();assert(unit>=GL_TEXTURE0 && unit<GL_TEXTURE0+4);client.unit=unit;}
void glEnableClientState(GLenum cap){outside();client.enabled|=clientBit(cap);}
void glDisableClientState(GLenum cap){outside();client.enabled&=~clientBit(cap);}
static void pointer(ArrayPointer *p,GLint size,GLenum type,GLsizei stride,const void *data){outside();*p=(ArrayPointer){size,type,stride,data,client.buffer};}
void glVertexPointer(GLint size,GLenum type,GLsizei stride,const void *data){pointer(&client.vertex,size,type,stride,data);}
void glColorPointer(GLint size,GLenum type,GLsizei stride,const void *data){pointer(&client.color,size,type,stride,data);}
void glTexCoordPointer(GLint size,GLenum type,GLsizei stride,const void *data){pointer(client.tex+client.unit-GL_TEXTURE0,size,type,stride,data);}
static const float *arrayElement(const ArrayPointer *p,unsigned i,int size){assert(p->size==size && p->type==GL_FLOAT && !p->buffer && p->stride>0 && p->pointer);return (const float *)((const char *)p->pointer+i*p->stride);}
void glDrawArrays(GLenum mode,GLint first,GLsizei count){
 outside();assert(clientPushed && mode==GL_TRIANGLES && first==0 && count>0 && count%3==0 && count<=R_ARRAY_VERTICES);
 assert(client.unit==GL_TEXTURE0 && (client.enabled&7)==7 && !client.buffer);
 arrayDraws++;beginSlots[eye<0?0:eye+1]++;primitive=GL_TRIANGLES;triangleIndex=0;
 for(int i=0;i<count;i++){
  const float *p=arrayElement(&client.vertex,i,4);memcpy(color,arrayElement(&client.color,i,4),sizeof color);memcpy(uv,arrayElement(client.tex,i,2),sizeof uv);
  glVertex4f(p[0],p[1],p[2],p[3]);
 }
 assert(triangleIndex==(unsigned)count);primitive=0;
 for(int i=0;i<4;i++)color[i]=NAN;for(int i=0;i<2;i++)uv[i]=NAN;
}
void glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a){colorCalls++;color[0]=r;color[1]=g;color[2]=b;color[3]=a;}
void glTexCoord2f(GLfloat u,GLfloat v){uv[0]=u;uv[1]=v;}
void glTexCoord2fv(const GLfloat *p){glTexCoord2f(p[0],p[1]);}
void glEnable(GLenum e){outside();gpu.enabled|=enableBit(e);}
void glDisable(GLenum e){outside();gpu.enabled&=~enableBit(e);}
GLboolean glIsEnabled(GLenum e){outside();return (gpu.enabled&enableBit(e))!=0;}
void glPushAttrib(GLbitfield bits){outside();assert(bits==GL_TEXTURE_BIT && !textureAttribPushed);textureAttribState=gpu;textureAttribPushed=1;}
void glPopAttrib(void){outside();assert(textureAttribPushed);gpu=textureAttribState;textureAttribPushed=0;}
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
void glFlush(void){outside();flushCalls++;if(raceProtocol && eye>=0)raceEyeFlush[eye]++;}
void glFinish(void){outside();finishCalls++;}
void glGenTextures(GLsizei n,GLuint *ids){outside();for(int i=0;i<n;i++)ids[i]=nextTexture++;assert(nextTexture<256);}
void glGenFramebuffers(GLsizei n,GLuint *ids){outside();for(int i=0;i<n;i++)ids[i]=5+i;}
void glGenRenderbuffers(GLsizei n,GLuint *ids){outside();for(int i=0;i<n;i++)ids[i]=8+i;}
void glDeleteTextures(GLsizei n,const GLuint *ids){outside();for(int i=0;i<n;i++)if(ids[i]==waterTextureId)waterDeleteCount++;}
void glTexImage2D(GLenum t,GLint l,GLint f,GLsizei w,GLsizei h,GLint b,GLenum ef,GLenum et,const GLvoid *p){
 outside();(void)t;(void)f;(void)b;(void)ef;(void)et;textures[gpu.texture].generation++;
 if(w==64 && h==64 && p && f==GL_RGBA){waterTextureId=gpu.texture;waterUploadCount=waterUploadLevels=0;waterUploadHash=1469598103934665603ULL;waterBaseMin=255;waterBaseMax=0;}
 if(gpu.texture==waterTextureId && p){
  assert(l>=0 && l<=6 && w==(64>>l) && h==w);waterUploadCount++;waterUploadLevels|=1u<<l;
  const unsigned char *bytes=p;
  for(int i=0;i<w*h;i++)for(int c=0;c<4;c++){
   unsigned char value=bytes[4*i+c];waterUploadHash=(waterUploadHash^value)*1099511628211ULL;
   if(l==0 && c==0){if(value<waterBaseMin)waterBaseMin=value;if(value>waterBaseMax)waterBaseMax=value;}
   if(c==3)assert(value==255);
  }
 }
 record(3,NULL);
}
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
void vr_xr_end(int r){outside();if(raceProtocol){assert(raceRenders[0]==1 && raceRenders[1]==1 && raceRenders[2]==1);assert(r==1 && !raceOwned[0] && !raceOwned[1] && releaseCount==2);assert(reads==(raceCaptures?2u:0u));assert(raceEyeFlush[0]==1u+(earlyFlushDraws?1u:0u) && raceEyeFlush[1]==1u+(earlyFlushDraws?1u:0u));if(pairedComplete)assert(pairFinishes==1 && captureMask==(raceCaptures?3u:0u));}}
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
#ifndef VR_REAL_CAMERA
void SetViewportClipRect(int *cam){assert(cam);}
#endif
static void setup(int enabled,int mode){
 if(depthReversed)setDepthMode(0);
 R_SetVREarlyFlush(0);earlyFlushDraws=0;memset(&gpu,0,sizeof gpu);gpu.clipDepth=GL_NEGATIVE_ONE_TO_ONE;gpu.clearDepth=1;
 perspectiveDepth=0;s_vrDepthReversed=0;clipControl=testClipControl;memset(textures,0,sizeof textures);memset(color,0,sizeof color);memset(uv,0,sizeof uv);
 memset(s_glTextureFilter,0,sizeof s_glTextureFilter);memset(s_tpageFilter,0,sizeof s_tpageFilter);memset(s_glTextureDirty,0,sizeof s_glTextureDirty);
 memset(g_tpageStateArray,0,52*sizeof *g_tpageStateArray);memset(s_tpageNoColorKey,0,sizeof s_tpageNoColorKey);memset(s_tpageGreen6,0,sizeof s_tpageGreen6);
 s_glTexturesInited=1;nextTexture=53;for(int i=0;i<52;i++){s_glTextures[i]=i+1;g_tpageWidth[i]=g_tpageHeight[i]=2;}
 static unsigned short pixels[4]={0x1234,0x3456,0x5678,0x789a};g_tpagePixelBuf[0]=pixels;g_tpagePixelBuf[1]=pixels;
 eventCount=begins=logicalFans=reads=swaps=releaseCount=colorCalls=flushCalls=finishCalls=0;memset(beginSlots,0,sizeof beginSlots);
 perfEnabled=1;modeRace=1;memset(perfArraySubmits,0,sizeof perfArraySubmits);memset(perfSubmits,0,sizeof perfSubmits);memset(perfVertices,0,sizeof perfVertices);
 R_SetVRClientArrays(fixtureArrays);arrayDraws=0;
 traceStart();R_SetVRBatching(enabled);R_ResetState();g_screenWidth=640;g_screenHeight=480;g_screenHeightFull=480;
 g_screenCenterX=320;g_screenCenterY=240;g_projScaleXCurrent=g_projScaleY=320;g_farClipFloat=32768;
 initialized=1;diagnostic=0;identity=mode==2;eye=mode?0:-1;framebuffer=5;
 depth[0]=8;depth[1]=9;memset(depthFormat,0,sizeof depthFormat);
 for(int e=0;e<2;e++){eyes[e]=(VREye){.pose={{0,0,0,1},{e?.032f:-.032f,0,0}},.fov={-.8f,.7f,.6f,-.65f},.width=8,.height=6};}
 if(eye>=0){cacheEyeProjection();}layer=0;roll=0;rollCos=1;rollSin=0;
}
static void raceProtocolTest(int paired,int captures,int reverse,int early){
 fflush(NULL);pid_t child=fork();assert(child>=0);
 if(!child){
  setup(1,1);earlyFlushDraws=early;perspectiveDepth=reverse;raceProtocol=1;pairedComplete=paired;raceCaptures=captures;pairFinishes=captureMask=0;
  memset(raceOwned,0,sizeof raceOwned);memset(raceDone,0,sizeof raceDone);
  memset(raceEyeFlush,0,sizeof raceEyeFlush);memset(raceRenders,0,sizeof raceRenders);
  eye=-1;runtimeDump=captures;ticks=1;dumpTick=2;stereoCheck=0;haveCamera=1;
  g_viewportArray[0]=0;VR_RenderRace(1);outside();assert(!captures);
  assert(releaseCount==2 && !reads && pairFinishes==(unsigned)paired);
  assert(raceEyeFlush[0]==1u+early && raceEyeFlush[1]==1u+early);
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
 R_SetVRClientArrays(!fixtureArrays);outside();draw(3);R_SetVRClientArrays(fixtureArrays);outside();draw(5);
 R_FlushState();outside();draw(4);R_ClearDepth();draw(3);ProcessTpageStates();draw(4);RenderBackground();draw(4);
 R_ResetState();draw(3);BeginFrame();draw(4);SetViewportFromConfig(NULL);outside();draw(3);
 if(mode){
  VR_Layer(1);outside();draw(4);VR_SkyBegin();outside();draw(4);VR_SkyEnd();outside();draw(4);VR_Layer(3);outside();draw(4);VR_Layer(0);
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
static void earlyFlushScenario(unsigned threshold,int batch){
 setup(batch,1);earlyFlushDraws=threshold;
 /* Canonical native, identity and menu targets remain disarmed. */
 eye=-1;BeginFrame();draw(4);EndFrame();assert(flushCalls==1);
 for(int mode=0;mode<2;mode++){
  modeRace=mode;identity=mode;target(0);BeginFrame();
  unsigned before=flushCalls;for(int i=0;i<10;i++)draw(4);
  R_EndBatch();assert(flushCalls==before && !s_earlyFlushRemaining);EndFrame();release();
 }
 identity=0;modeRace=1;
 const int order[]={0,1,1,0};
 for(unsigned pass=0;pass<4;pass++){
  target(order[pass]);BeginFrame();unsigned before=flushCalls;
  for(int i=0;i<10;i++){
   if(i==2)R_SetBlendMode(R_BLEND_ADDITIVE);
   if(i==5){VR_Layer(1);R_ResetState();}
   draw(3+i%3);
   assert(flushCalls==before+((threshold && (unsigned)i+1>=threshold)?1u:0u));
  }
  EndFrame();assert(flushCalls==before+1+((threshold && threshold<=10)?1u:0u));
  if(pass&1)unbindEye();else release();
  assert(!s_earlyFlushRemaining);
 }
 /* An abandoned eye cannot leave a countdown armed for later native draws. */
 target(0);BeginFrame();draw(4);VR_Shutdown();assert(!s_earlyFlushRemaining);
 assert(!reads && !finishCalls);outside();traceDraw=0;
}
static void earlyFlushTest(void){
 for(int batch=0;batch<2;batch++){
  earlyFlushScenario(0,batch);referenceCount=eventCount;
  memcpy(reference,events,eventCount*sizeof *events);unsigned baseFlushes=flushCalls;
  const unsigned thresholds[]={1,3,12};
  for(unsigned i=0;i<3;i++){
   unsigned threshold=thresholds[i];earlyFlushScenario(threshold,batch);
   assert(eventCount==referenceCount && !memcmp(reference,events,eventCount*sizeof *events));
   assert(flushCalls==baseFlushes+(threshold<=10?4:0)+(threshold==1?1:0));
  }
 }
 puts("early flush production: once per race eye, batch on/off, thresholds1/3/12, no native/identity/menu flush, repeated/reversed eyes, state/layer boundaries, release/abandon reset; exact vertices/state and no finish/readback");
}
static void arrayStorageTest(void){
 for(int mode=0;mode<3;mode++){
  fixtureArrays=0;setup(1,mode);BeginFrame();
  for(int i=0;i<700;i++)draw(7);
  R_EndBatch();referenceCount=eventCount;memcpy(reference,events,eventCount*sizeof *events);
  float lastColor[4],lastUV[2];memcpy(lastColor,color,sizeof color);memcpy(lastUV,uv,sizeof uv);
  fixtureArrays=1;setup(1,mode);BeginFrame();
  /* Foreign client pointers/buffer/unit are untouched after every submit. They
   * are never dereferenced; desktop production itself has no other array user. */
  memset(&client,0,sizeof client);client.unit=GL_TEXTURE0+3;client.buffer=77;client.enabled=1|4;
  client.vertex=(ArrayPointer){2,GL_SHORT,8,(void *)16,66};
  client.color=(ArrayPointer){3,GL_UNSIGNED_BYTE,4,(void *)32,55};
  client.tex[0]=(ArrayPointer){3,GL_SHORT,12,(void *)48,44};
  client.tex[3]=(ArrayPointer){4,GL_FLOAT,16,(void *)64,33};
  ClientState initial=client;
  for(int i=0;i<700;i++)draw(7);
  R_EndBatch();
  assert(!memcmp(&client,&initial,sizeof client) && !clientPushed);
  assert(eventCount==referenceCount && !memcmp(reference,events,eventCount*sizeof *events));
  assert(!memcmp(lastColor,color,sizeof color) && !memcmp(lastUV,uv,sizeof uv));
  assert(arrayDraws==(mode==1?4u:0u));
  /* A following immediate draw inherits the same last attributes. */
  glBegin(GL_TRIANGLES);for(int i=0;i<3;i++)glVertex4f(i,i,0,1);glEnd();
  for(unsigned i=eventCount-3;i<eventCount;i++)assert(!memcmp(events[i].vertex.color,lastColor,sizeof color) && !memcmp(events[i].vertex.uv,lastUV,sizeof uv));
  memset(&client,0,sizeof client);client.unit=GL_TEXTURE0;
 }
 fixtureArrays=0;puts("client arrays: 10500 vertices, exact intra-fan overflow splits at3072/6144/9216, inherited attributes restored, client pointers/enables/unit/buffers restored, native/identity bypass");
}
_Static_assert(sizeof(R_StateSnapshot)==60,"water must preserve the CPU draw-stream state schema");
static float waterCamera[3],waterView[16];
static VRFov waterFov;
static uint64_t hashWaterEvents(void){
 uint64_t h=1469598103934665603ULL;
 for(unsigned i=0;i<eventCount;i++)h=hashBytes(h,events+i,sizeof events[i]);
 return h;
}
static uint64_t hashWaterGeometry(void){
 uint64_t h=1469598103934665603ULL;
 for(unsigned i=0;i<eventCount;i++){
  Output *v=&events[i].vertex;
  h=hashBytes(h,v->xyzw,sizeof(float)*2);
  h=hashBytes(h,v->xyzw+3,sizeof(float));
  h=hashBytes(h,v->color,sizeof v->color);
  h=hashBytes(h,v->uv,sizeof v->uv);
 }
 return h;
}
static unsigned waterPass(int e,int reverse,unsigned tick,int snow,float cameraY,
                          float yaw,float pitch,float roll,float shiftX,float shiftZ,
                          int arrays,int batch){
 fixtureArrays=arrays;modeRace=1;identity=0;layer=0;perspectiveDepth=reverse;
 earlyFlushDraws=0;g_farClipFloat=88064;g_farClipTimes8=88064;g_farClipDepth=1000;
 eyes[0].fov=(VRFov){atanf(-1.2f),atanf(.8f),atanf(.85f),atanf(-.65f)};
 eyes[1].fov=(VRFov){atanf(-.8f),atanf(1.2f),atanf(.85f),atanf(-.65f)};
 R_SetVRBatching(batch);R_SetVRClientArrays(arrays);R_SetVREarlyFlush(0);
 target(e);BeginFrame();

 memset(eyeCamera,0,sizeof eyeCamera);
 RenderCamera *camera=(RenderCamera*)eyeCamera;
 camera->camFloatX=-2300.0f+(e?2.56f:-2.56f)+shiftX;
 camera->camFloatY=cameraY;
 camera->camFloatZ=-2962.0f+shiftZ;
 waterCamera[0]=camera->camFloatX;waterCamera[1]=cameraY;waterCamera[2]=camera->camFloatZ;
 float base[9]={1,0,0,0,1,0,0,0,1},basis[9];
 VRQuat qy={0,sinf(yaw*.5f),0,cosf(yaw*.5f)};
 VRQuat qp={sinf(pitch*.5f),0,0,cosf(pitch*.5f)};
 VRQuat qr={0,0,sinf(roll*.5f),cosf(roll*.5f)};
 VRQuat q=vr_mul(vr_mul(qy,qp),qr);
 vr_compose_basis(base,q,basis);
 float *matrix=(float*)(eyeCamera+0x22);
 for(int r=0;r<4;r++)for(int c=0;c<4;c++)matrix[r*4+c]=(r<3 && c<3)?basis[r*3+c]:(r==3 && c==3?1:0);
 memcpy(waterView,matrix,sizeof waterView);waterFov=eyes[e].fov;
 g_totalFrames=(int)tick;g_weatherType=snow?WEATHER_SNOW:WEATHER_CLEAR;

 /* Give the pass a non-default caller state and foreign client pointers. */
 R_SetTexture(1);R_SetBlendMode(R_BLEND_ADDITIVE);R_SetDepthTest(1);
 R_SetDepthFunc(R_DEPTH_LESS);R_SetDepthWrite(1);R_SetTexEnv(R_TEXENV_ADD_SIGNED);
 R_SetAlphaTest(1);R_SetCullMode(R_CULL_BACK);R_SetScissor(2,3,600,470);
 R_FlushState();GLState incoming=gpu;
 int beforeDepth=0,beforeWrite=0,beforeFunc=0,beforeBlend=0,beforeAlpha=0;float beforeRef=0;
 R_DebugGetState(&beforeDepth,&beforeWrite,&beforeFunc,&beforeBlend,&beforeAlpha,&beforeRef);
 ClientState initialClient;memset(&client,0,sizeof client);client.unit=GL_TEXTURE0+3;client.buffer=77;client.enabled=1|4;
 client.vertex=(ArrayPointer){2,GL_SHORT,8,(void*)16,66};
 client.color=(ArrayPointer){3,GL_UNSIGNED_BYTE,4,(void*)32,55};
 client.tex[0]=(ArrayPointer){3,GL_SHORT,12,(void*)48,44};
 client.tex[3]=(ArrayPointer){4,GL_FLOAT,16,(void*)64,33};initialClient=client;
 unsigned oldUploads=waterUploadCount,oldNextTexture=nextTexture,oldReads=reads,oldFinishes=finishCalls,oldFlushes=flushCalls;
 eventCount=0;
 /* Exercise the VR-facing entry sequence inside RenderHUD's legacy sky scope. */
 VR_SkyBegin();assert(layer==2);
 VR_WaterSurface();
 assert(layer==2); /* water temporarily used world cached projection/depth */
 VR_SkyEnd();assert(layer==0);
 assert(!textureAttribPushed && !clientPushed && !s_batchOpen);
 assert(!memcmp(&client,&initialClient,sizeof client));
 assert(s_desired.textureId==1 && !s_desiredTextureObject);
 assert(!memcmp(&gpu,&incoming,sizeof gpu));
 int depth=0,write=0,func=0,blend=0,alpha=0;float ref=0;
 R_DebugGetState(&depth,&write,&func,&blend,&alpha,&ref);
 assert(depth==beforeDepth && write==beforeWrite && func==beforeFunc && blend==beforeBlend && alpha==beforeAlpha && ref==beforeRef);
 if(cameraY<=0)assert(eventCount==0);else assert(eventCount>0);
 if(eventCount){
  assert(waterTextureId && waterTextureId<256);
  float phase=snow?0:(float)(tick&2047);
  float l=tanf(waterFov.left),r=tanf(waterFov.right),u=tanf(waterFov.up),d=tanf(waterFov.down);
  for(unsigned i=0;i<eventCount;i++){
   assert(events[i].type==1);Output *v=&events[i].vertex;
   assert(v->state.texture==waterTextureId && v->texture.min==GL_LINEAR_MIPMAP_LINEAR);
   assert(v->texture.mag==GL_LINEAR && v->texture.wrapS==GL_REPEAT && v->texture.wrapT==GL_REPEAT);
   assert(v->state.depth==GL_FALSE && v->state.depthFunc==(reverse?GL_GEQUAL:GL_LEQUAL));
   assert(v->state.clipDepth==(reverse?GL_ZERO_TO_ONE:GL_NEGATIVE_ONE_TO_ONE));
   assert(v->state.depthFormat==(reverse?GL_DEPTH_COMPONENT32F:GL_DEPTH_COMPONENT24));
   assert((v->state.enabled&7)==7 && v->state.src==GL_SRC_ALPHA && v->state.dst==GL_ONE_MINUS_SRC_ALPHA);
   assert(v->state.env==GL_MODULATE && v->color[3]>=0 && v->color[3]<=0.34f);
   float depthZ=v->xyzw[3];assert(isfinite(depthZ) && depthZ>=1.0f);
   float expectedDepth=reverse?vr_depth_map(depthZ/g_farClipFloat,vr_depth_near(g_farClipFloat)):depthZ/g_farClipFloat;
   assert(fabsf(v->xyzw[2]/depthZ-expectedDepth)<3e-6f);
   float nx=(v->xyzw[0]/depthZ)/320-1,ny=1-(v->xyzw[1]/depthZ)/240;
   float cx=depthZ*(nx*(r-l)+r+l)*.5f,cy=depthZ*(ny*(u-d)+u+d)*.5f;
   float wx=waterCamera[0]+cx*waterView[0]+cy*waterView[1]+depthZ*waterView[2];
   float wy=waterCamera[1]+cx*waterView[4]+cy*waterView[5]+depthZ*waterView[6];
   float wz=waterCamera[2]+cx*waterView[8]+cy*waterView[9]+depthZ*waterView[10];
   assert(fabsf(wy)<0.08f);
   assert(fabsf(v->uv[0]-(wx/512+phase/2048))<0.0002f);
   assert(fabsf(v->uv[1]-(wz/512-phase/1024))<0.0002f);
  }
  assert(!memcmp(color,events[eventCount-1].vertex.color,sizeof color));
  assert(!memcmp(uv,events[eventCount-1].vertex.uv,sizeof uv));
 }
 R_FlushState();assert(!memcmp(&gpu,&incoming,sizeof gpu));
 assert(oldUploads==waterUploadCount && oldNextTexture==nextTexture && oldReads==reads && oldFinishes==finishCalls && oldFlushes==flushCalls);
 return eventCount;
}
static int sampleWaterWorld(float worldX,float worldZ,float *u,float *v){
 float dx=worldX-waterCamera[0],dy=-waterCamera[1],dz=worldZ-waterCamera[2];
 float cx=dx*waterView[0]+dy*waterView[4]+dz*waterView[8];
 float cy=dx*waterView[1]+dy*waterView[5]+dz*waterView[9];
 float cz=dx*waterView[2]+dy*waterView[6]+dz*waterView[10];
 if(cz<1)return 0;
 float nx,ny;vr_project((VRVec){cx,cy,-cz},waterFov,&nx,&ny);
 float x=(nx+1)*320,y=(1-ny)*240;
 for(unsigned i=0;i+2<eventCount;i+=3){
  Output *a=&events[i].vertex,*b=&events[i+1].vertex,*c=&events[i+2].vertex;
  float ax=a->xyzw[0]/a->xyzw[3],ay=a->xyzw[1]/a->xyzw[3];
  float bx=b->xyzw[0]/b->xyzw[3],by=b->xyzw[1]/b->xyzw[3];
  float cxp=c->xyzw[0]/c->xyzw[3],cyp=c->xyzw[1]/c->xyzw[3];
  float den=(by-cyp)*(ax-cxp)+(cxp-bx)*(ay-cyp);if(fabsf(den)<1e-6f)continue;
  float wa=((by-cyp)*(x-cxp)+(cxp-bx)*(y-cyp))/den;
  float wb=((cyp-ay)*(x-cxp)+(ax-cxp)*(y-cyp))/den,wc=1-wa-wb;
  if(wa<0 || wb<0 || wc<0)continue;
  wa/=a->xyzw[3];wb/=b->xyzw[3];wc/=c->xyzw[3];float sum=wa+wb+wc;
  *u=(wa*a->uv[0]+wb*b->uv[0]+wc*c->uv[0])/sum;
  *v=(wa*a->uv[1]+wb*b->uv[1]+wc*c->uv[1])/sum;return 1;
 }
 return 0;
}
static void waterSurfaceRegression(void){
 setup(1,1);BeginFrame();R_FlushState();
 /* Texture allocation must close a pending native eye batch before raw GL
  * binding/upload calls, and restore its tracked binding afterwards. */
 R_SetTexture(1);draw(4);assert(s_batchOpen);GLState initial=gpu;
 unsigned pendingBefore=eventCount;
 unsigned startNext=nextTexture,startDeletes=waterDeleteCount;
 vr_water_init();GLuint textureId=waterTextureId;unsigned uploads=waterUploadCount,levels=waterUploadLevels;
 uint64_t textureHash=waterUploadHash;
 assert(!s_batchOpen && eventCount>pendingBefore);
 assert(textureId && nextTexture==startNext+1 && uploads==7 && levels==127);
 assert(textureHash==0x300a251ce8690b12ULL); /* Exact 64x64+mip bytes from approved f2491a8 material. */
 assert(waterBaseMin>=31 && waterBaseMax<=111 && waterBaseMax-waterBaseMin>60);
 assert(textures[textureId].min==GL_LINEAR_MIPMAP_LINEAR && textures[textureId].mag==GL_LINEAR);
 assert(textures[textureId].wrapS==GL_REPEAT && textures[textureId].wrapT==GL_REPEAT);
 assert(!memcmp(&gpu,&initial,sizeof gpu) && !textureAttribPushed);
 unsigned afterCreate=nextTexture;vr_water_init();
 assert(waterTextureId==textureId && waterUploadCount==uploads && nextTexture==afterCreate);

 unsigned nearClipped=0;uint64_t animated[2][2]={{0}},geometryBase=0;
 for(int arrays=0;arrays<2;arrays++)for(int batch=0;batch<2;batch++)for(int reverse=0;reverse<2;reverse++){
  unsigned count=waterPass(0,reverse,80,0,226,0,0,0,0,0,arrays,batch);assert(count>0);
  uint64_t geometry=hashWaterGeometry();animated[arrays][reverse]=hashWaterEvents();
  for(unsigned i=0;i<eventCount;i++)if(fabsf(events[i].vertex.xyzw[3]-1.0f)<0.0001f)nearClipped++;
  unsigned repeated=waterPass(0,reverse,80,0,226,0,0,0,0,0,arrays,batch);
  assert(repeated==count && hashWaterEvents()==animated[arrays][reverse]);
  assert(hashWaterGeometry()==geometry);
  if(!arrays && !batch && !reverse)geometryBase=geometry;
  if(!arrays && !batch && reverse)assert(geometry==geometryBase);
 }
 assert(nearClipped>0);

 uint64_t moving0[2],moving1[2];
 unsigned count=waterPass(0,1,90,0,226,0,0,0,0,0,1,1);assert(count>0);
 float leftUV[2];assert(sampleWaterWorld(-2300,-1462,leftUV,leftUV+1));moving0[0]=hashWaterEvents();
 waterPass(1,1,90,0,226,0,0,0,0,0,1,1);float rightUV[2];
 assert(sampleWaterWorld(-2300,-1462,rightUV,rightUV+1));moving0[1]=hashWaterEvents();
 assert(fabsf(leftUV[0]-rightUV[0])<0.0001f && fabsf(leftUV[1]-rightUV[1])<0.0001f);
 waterPass(0,1,90,0,226,0,0,0,0,0,1,1);assert(hashWaterEvents()==moving0[0]);
 waterPass(1,1,90,0,226,0,0,0,0,0,1,1);assert(hashWaterEvents()==moving0[1]);
 waterPass(0,1,90,0,226,0,0,0,40,60,1,1);float movedUV[2];
 assert(sampleWaterWorld(-2300,-1462,movedUV,movedUV+1));
 assert(fabsf(leftUV[0]-movedUV[0])<0.0001f && fabsf(leftUV[1]-movedUV[1])<0.0001f);
 uint64_t flowing=hashWaterEvents();waterPass(0,1,91,0,226,0,0,0,0,0,1,1);
 assert(hashWaterEvents()!=flowing);
 uint64_t snow=hashWaterEvents();waterPass(0,1,90,1,226,0,0,0,0,0,1,1);
 snow=hashWaterEvents();waterPass(0,1,91,1,226,0,0,0,0,0,1,1);
 assert(hashWaterEvents()==snow);
 assert(waterPass(0,1,90,0,0,0,0,0,0,0,1,1)==0);
 assert(waterPass(0,1,90,0,-1,0,0,0,0,0,1,1)==0);
 for(int i=-8;i<=8;i++)assert(waterPass(0,1,90,0,226,i*.07f,.11f,i*.05f,0,0,1,1)>0);
 /* Shutdown closes any final native batch before deleting its owned texture. */
 eventCount=0;R_SetTexture(1);draw(4);assert(s_batchOpen);
 vr_water_shutdown();assert(!s_batchOpen && eventCount==6 && gpu.texture==s_glTextures[1] && gpu.texture!=textureId);
 vr_water_shutdown();assert(waterDeleteCount==startDeletes+1);
 initialized=0;

 /* The opt-out is sampled by VR_Init once; changing its environment value
  * after startup cannot change the current session or allocate the texture. */
 setenv("SONICR_VR_DIAGNOSTIC","1",1);unsetenv("SONICR_VR_OFFHEAD_RENDER");unsetenv("SONICR_VR_OFFHEAD_NEUTRAL");
 unsetenv("SONICR_VR_CLIENT_ARRAYS");unsetenv("SONICR_VR_EARLY_FLUSH_DRAWS");
 unsetenv("SONICR_VR_PAIRED_COMPLETE");unsetenv("SONICR_VR_DEPTH");unsetenv("SONICR_VR_OFFHEAD_RUNTIME_FOV");
 setenv("SONICR_VR_WATER_SURFACE","0",1);unsigned uploadsBeforeOptout=waterUploadCount;
 assert(VR_Init() && !waterSurfaceEnabled && waterUploadCount==uploadsBeforeOptout);
 setenv("SONICR_VR_WATER_SURFACE","1",1);eventCount=0;VR_WaterSurface();assert(eventCount==0);
 VR_Shutdown();
 unsetenv("SONICR_VR_WATER_SURFACE");unsigned previousId=waterTextureId;
 assert(VR_Init() && waterSurfaceEnabled && waterTextureId!=previousId && waterUploadCount==7);
 setenv("SONICR_VR_WATER_SURFACE","0",1);
 assert(waterPass(0,1,90,0,226,0,0,0,0,0,1,1)>0);
 VR_Shutdown();
 printf("production water: 64x64 mip texture hash=%016llx; immediate/client-array and batching modes; reverse/legacy depth; animation/world anchor/stereo repeat/pose/near clip/underwater/snow/state/resource/optout pass\n",(unsigned long long)textureHash);
}
int main(int argc,char **argv){
 assert(argc==2);dumpDir=argv[1];
 if(getenv("SONICR_TEST_WATER_ONLY")){waterSurfaceRegression();return 0;}
 arrayStorageTest();
 for(fixtureArrays=0;fixtureArrays<2;fixtureArrays++){
 colorTest();earlyFlushTest();
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
  if(mode==1)assert(begins+arrayDraws<oldBegins);else assert(begins==oldBegins && !arrayDraws);
  unsigned submits=perfSubmits[0]+perfSubmits[1]+perfSubmits[2];assert(submits==begins+arrayDraws);
  assert(perfArraySubmits[0]+perfArraySubmits[1]+perfArraySubmits[2]==arrayDraws);
  assert(!memcmp(perfSubmits,beginSlots,sizeof beginSlots));
  printf("batch production legacy+optout mode=%s fans=%u events=%u old_submits=%u new_submits=%u array_submits=%u exact_triangles_state=1 legal_gl=1\n",mode==0?"native":mode==1?"eye":"identity",logicalFans,eventCount,oldBegins,submits,arrayDraws);
 }
 /* Pending race geometry must close before the menu copy and its direct
  * GL_QUADS block. This path intentionally has no backend fan counter. */
 setup(1,1);pairedComplete=1;BeginFrame();draw(4);presented=0;anchored=wasFocused=1;clockState=(VRClock){0};
 assert(VR_Present()==1);outside();assert(releaseCount==2);VR_Shutdown();
 puts("batch production menu transition: pending eye geometry closed before copy and direct quad draws");
 }
 fixtureArrays=0;
 /* Test the actual environment route, not only its backend setter. */
 setenv("SONICR_VR_DIAGNOSTIC","1",1);unsetenv("SONICR_VR_OFFHEAD_RENDER");unsetenv("SONICR_VR_OFFHEAD_NEUTRAL");
 unsetenv("SONICR_VR_CLIENT_ARRAYS");
 unsetenv("SONICR_VR_EARLY_FLUSH_DRAWS");unsetenv("SONICR_VR_PAIRED_COMPLETE");unsetenv("SONICR_VR_DEPTH");unsetenv("SONICR_VR_OFFHEAD_RUNTIME_FOV");
 setenv("SONICR_VR_BATCH","0",1);assert(VR_Init());assert(!batching && !s_vrBatching && pairedComplete && perspectiveDepth);VR_Shutdown();
 unsetenv("SONICR_VR_BATCH");assert(VR_Init());assert(batching && s_vrBatching && perspectiveDepth && earlyFlushDraws==2048 && !s_earlyFlushRemaining && clientArrays && s_vrClientArrays);VR_Shutdown();
 setenv("SONICR_VR_EARLY_FLUSH_DRAWS","0",1);assert(VR_Init());assert(!earlyFlushDraws && !s_earlyFlushRemaining);VR_Shutdown();
 setenv("SONICR_VR_EARLY_FLUSH_DRAWS","3",1);assert(VR_Init());assert(earlyFlushDraws==3 && !s_earlyFlushRemaining);VR_Shutdown();
 unsetenv("SONICR_VR_EARLY_FLUSH_DRAWS");
 setenv("SONICR_VR_CLIENT_ARRAYS","1",1);assert(VR_Init());assert(clientArrays && s_vrClientArrays);VR_Shutdown();
 setenv("SONICR_VR_CLIENT_ARRAYS","0",1);assert(VR_Init());assert(!clientArrays && !s_vrClientArrays);VR_Shutdown();
 unsetenv("SONICR_VR_CLIENT_ARRAYS");
 puts("client arrays startup: default on, explicit1 enabled, explicit0 disabled");
 puts("early flush startup: unset defaults2048; explicit0 disables; explicit3 selects threshold; startup never arms an eye");
 puts("batch production environment: explicit 0 disables batching; unset batching and depth default on");
 setenv("SONICR_VR_PAIRED_COMPLETE","1",1);assert(VR_Init());assert(pairedComplete);VR_Shutdown();
 setenv("SONICR_VR_PAIRED_COMPLETE","0",1);assert(VR_Init());assert(!pairedComplete);VR_Shutdown();
 setenv("SONICR_VR_DEPTH","1",1);assert(VR_Init());assert(perspectiveDepth);VR_Shutdown();
 setenv("SONICR_VR_DEPTH","0",1);assert(VR_Init());assert(!perspectiveDepth);VR_Shutdown();
 fflush(NULL);pid_t bad=fork();assert(bad>=0);
 if(!bad){setenv("SONICR_VR_OFFHEAD_RUNTIME_FOV","1",1);VR_Init();_exit(0);}
 int status;assert(waitpid(bad,&status,0)==bad);assert(WIFEXITED(status) && WEXITSTATUS(status)!=0);
 puts("runtime FOV production startup: invalid diagnostic/neutral flag combination rejected");
 dumpDir=argv[1];
 for(fixtureArrays=0;fixtureArrays<2;fixtureArrays++)for(int paired=0;paired<2;paired++)for(int captures=0;captures<2;captures++)for(int reverse=0;reverse<2;reverse++)for(int early=0;early<2;early++)raceProtocolTest(paired,captures,reverse,early);
 puts("race completion: actual render loop default/opt-out, no normal readbacks, paired captures after shared finish before either release passed");
 waterSurfaceRegression();
 return 0;
}
