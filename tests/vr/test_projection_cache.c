/* Actual emitter/backend and target/camera lifecycle; only GL output is recorded.
 * The uncached emitter is extracted verbatim from the baseline commit by runner. */
#include "vr/vr.c"
#include "r_gl_backend.c"
#include <assert.h>
#include "legacy_emitter.h"
static float emitted[4];static unsigned comparisons;
void glVertex4f(GLfloat x,GLfloat y,GLfloat z,GLfloat w){emitted[0]=x;emitted[1]=y;emitted[2]=z;emitted[3]=w;}
void glEnd(void){assert(0 && "projection fixture never opens a batch");}
void glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a){(void)r;(void)g;(void)b;(void)a;}
void glTexCoord2f(GLfloat u,GLfloat v){(void)u;(void)v;}
void glBindFramebuffer(GLenum t,GLuint f){(void)t;(void)f;}
void glFramebufferTexture2D(GLenum t,GLenum a,GLenum tt,GLuint tex,GLint l){(void)t;(void)a;(void)tt;(void)tex;(void)l;}
void glBindRenderbuffer(GLenum t,GLuint r){(void)t;(void)r;}
void glRenderbufferStorage(GLenum t,GLenum f,GLsizei w,GLsizei h){(void)t;(void)f;(void)w;(void)h;}
void glFramebufferRenderbuffer(GLenum t,GLenum a,GLenum rt,GLuint r){(void)t;(void)a;(void)rt;(void)r;}
void glDrawBuffer(GLenum b){(void)b;}
void glReadBuffer(GLenum b){(void)b;}
GLenum glCheckFramebufferStatus(GLenum t){(void)t;return GL_FRAMEBUFFER_COMPLETE;}
void glDisable(GLenum c){(void)c;}
void glViewport(GLint x,GLint y,GLsizei w,GLsizei h){(void)x;(void)y;(void)w;(void)h;}
void glMatrixMode(GLenum m){(void)m;assert(0);}
void glPushMatrix(void){assert(0);}
void glPopMatrix(void){assert(0);}
void glClearDepth(GLdouble d){(void)d;assert(0);}
void glDepthFunc(GLenum f){(void)f;assert(0);}
void glGetRenderbufferParameteriv(GLenum t,GLenum p,GLint *v){(void)t;(void)p;(void)v;assert(0);}
void glGetFramebufferAttachmentParameteriv(GLenum t,GLenum a,GLenum p,GLint *v){(void)t;(void)a;(void)p;(void)v;assert(0);}
void glDepthMask(GLboolean b){(void)b;}
void glClearColor(GLfloat r,GLfloat g,GLfloat b,GLfloat a){(void)r;(void)g;(void)b;(void)a;}
void glClear(GLbitfield b){(void)b;}
unsigned vr_xr_acquire(int e){(void)e;return 1;}
static void compare(RenderVertex v){
 float expected[4];Legacy_EmitVertex(&v);memcpy(expected,emitted,sizeof expected);
 VR_EmitVertex(&v);
 if(memcmp(expected,emitted,sizeof expected)){
  fprintf(stderr,"emitter mismatch eye=%d layer=%d identity=%d roll=%.9g\n",eye,layer,identity,roll);
  for(int i=0;i<4;i++)fprintf(stderr," component %d old=%a cached=%a\n",i,expected[i],emitted[i]);
  assert(0);
 }
 /* Exercise the actual backend dispatch as well, not just its VR function. */
 R_EmitVertex(&v);assert(!memcmp(expected,emitted,sizeof expected));comparisons++;
}
int main(void){
 diagnostic=0;haveCamera=1;anchorRotation=(VRQuat){0,0,0,1};
 const int order[]={0,1,1,0,0,1};
 for(int f=0;f<17;f++)for(int frustum=0;frustum<3;frustum++){
  float yaw=(f-8)*0.3f,pitch=(f-8)*0.08f,headRoll=(f-8)*0.13f;
  VRQuat q=vr_mul(vr_mul((VRQuat){0,sinf(yaw/2),0,cosf(yaw/2)},(VRQuat){sinf(pitch/2),0,0,cosf(pitch/2)}),(VRQuat){0,0,sinf(headRoll/2),cosf(headRoll/2)});
  for(int e=0;e<2;e++){
   eyes[e].pose=(VRPose){q,vr_add((VRVec){f*.002f,.1f,-.15f},vr_rotate(q,(VRVec){e?.035f:-.029f,0,0}))};
   /* Include canted eyes and asymmetric per-eye frusta. */
   eyes[e].pose.q=vr_mul(q,(VRQuat){0,sinf((e?1:-1)*.015f),0,cosf(.015f)});
   eyes[e].fov=frustum==0?(VRFov){-.78f,.78f,.64f,-.64f}:frustum==1?(VRFov){e?-.67f:-.88f,e?.88f:.67f,.70f,-.57f}:(VRFov){-.42f,.91f,.82f,-.39f};
   eyes[e].width=2160;eyes[e].height=2160;
  }
  for(int camera=0;camera<3;camera++){
   RenderCamera a={0},b={0};a.camFloatX=-2310+f;a.camFloatY=226;a.camFloatZ=-2961;
   b=a;b.camFloatX+=30;b.camFloatZ+=50;a.yawFloat=camera*.13f;b.yawFloat=a.yawFloat+.08f;
   a.pitchFloat=camera*.4f;b.pitchFloat=a.pitchFloat-.1f;
   memcpy(previous,&a,sizeof previous);memcpy(current,&b,sizeof current);clockState.alpha=(camera+1)*.25f;g_mirrorMode=camera==2;
   for(unsigned pass=0;pass<sizeof order/sizeof *order;pass++){
    identity=0;target(order[pass]);int *cam=current;VR_ApplyCamera(&cam);assert(cam==eyeCamera);
    for(layer=0;layer<4;layer++)for(int v=0;v<15;v++){
     RenderVertex vertex={0};vertex.sx=-170+v*71.37f;vertex.sy=-80+v*43.19f;
     vertex.sz=(v-2)*.1f;vertex.rhw=v==0?0:v==1?-1:1.0f/(v*127.31f);compare(vertex);
    }
    /* UI projection after target with no game camera call (menu route). */
    target(order[pass]);layer=1;compare((RenderVertex){123.25f,321.125f,.3f,.007f,0,0,0,0});
    /* Identity bypass does not consume cached projection or pose fields. */
    identity=1;for(layer=0;layer<4;layer++)compare((RenderVertex){17.25f,231.125f,.71f,.003f,0,0,0,0});
   }
  }
 }
 /* Canonical/native dispatch retains the original homogeneous coordinates. */
 identity=0;eye=-1;
 for(int i=0;i<15;i++){
  RenderVertex v={i*19.3f,i*7.1f,.37f,i?1.0f/(i*31.7f):0,0,0,0,0};
  float w=v.rhw>0?1/v.rhw:1,expected[4]={v.sx*w,v.sy*w,v.sz*w,w};
  R_EmitVertex(&v);assert(!memcmp(expected,emitted,sizeof expected));comparisons++;
 }
 printf("production projection cache: %u exact XYZW/backend comparisons; changing asymmetric FOV, canted/head pose, roll, camera/mirror, 4 layers, reversed eyes/repeats, menu, identity and native bypass\n",comparisons);
}
