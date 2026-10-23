/* Exercise the production water material through a recording GL boundary. */
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "vr/vr.h"
#include "vr/vr_water.h"

typedef struct {GLuint texture;GLenum env,src,dst,depth;GLboolean write;unsigned enables;float color[4],uv[2];} State;
static State state,saved;
static int pushed,generated,deleted,uploads,flushes,vertices,levelMask;
static GLint minFilter,magFilter,wrapS,wrapT;
static uint64_t textureHash=1469598103934665603ULL;
static RenderVertex polys[100][5];static int counts[100],polyCount;
void R_FlushState(void){flushes++;}
void glPushAttrib(GLbitfield mask){assert(!pushed);assert(mask&GL_TEXTURE_BIT);saved=state;pushed=1;}
void glPopAttrib(void){assert(pushed);state=saved;pushed=0;}
void glGenTextures(GLsizei n,GLuint *t){assert(n==1);*t=++generated;}
void glDeleteTextures(GLsizei n,const GLuint *t){assert(n==1 && *t);deleted++;}
void glBindTexture(GLenum target,GLuint t){assert(target==GL_TEXTURE_2D);state.texture=t;}
void glTexParameteri(GLenum target,GLenum name,GLint value){
 assert(target==GL_TEXTURE_2D);
 if(name==GL_TEXTURE_MIN_FILTER)minFilter=value;
 else if(name==GL_TEXTURE_MAG_FILTER)magFilter=value;
 else if(name==GL_TEXTURE_WRAP_S)wrapS=value;
 else if(name==GL_TEXTURE_WRAP_T)wrapT=value;
 else assert(0);
}
void glTexImage2D(GLenum target,GLint level,GLint internal,GLsizei w,GLsizei h,GLint border,GLenum format,GLenum type,const GLvoid *data){
 assert(target==GL_TEXTURE_2D && internal==GL_RGBA && format==GL_RGBA && type==GL_UNSIGNED_BYTE && border==0);
 assert(w==(64>>level) && h==w);levelMask|=1<<level;uploads++;
 const unsigned char *p=data;
 int lo=255,hi=0;
 for(int i=0;i<w*h;i++){
  assert(p[i*4+3]==255);
  if(p[i*4]<lo)lo=p[i*4];if(p[i*4]>hi)hi=p[i*4];
  for(int c=0;c<4;c++)textureHash=(textureHash^p[i*4+c])*1099511628211ULL;
 }
 if(level==0)assert(lo>=31 && hi<=111 && hi-lo>60);
 if(level==6)assert(lo==hi);
}
void glTexEnvi(GLenum target,GLenum name,GLint value){assert(target==GL_TEXTURE_ENV && name==GL_TEXTURE_ENV_MODE);state.env=value;}
static unsigned cap(GLenum c){switch(c){case GL_TEXTURE_2D:return 1;case GL_BLEND:return 2;case GL_DEPTH_TEST:return 4;case GL_ALPHA_TEST:return 8;case GL_CULL_FACE:return 16;case GL_LIGHTING:return 32;case GL_FOG:return 64;default:assert(0);return 0;}}
void glEnable(GLenum c){state.enables|=cap(c);}
void glDisable(GLenum c){state.enables&=~cap(c);}
void glBlendFunc(GLenum s,GLenum d){state.src=s;state.dst=d;}
void glDepthFunc(GLenum f){state.depth=f;}
void glDepthMask(GLboolean b){state.write=b;}
void glColor4f(GLfloat r,GLfloat g,GLfloat b,GLfloat a){state.color[0]=r;state.color[1]=g;state.color[2]=b;state.color[3]=a;}
void glTexCoord2f(GLfloat u,GLfloat v){state.uv[0]=u;state.uv[1]=v;}
void glBegin(GLenum mode){assert(mode==GL_TRIANGLE_FAN);}
void glEnd(void){}
void glVertex4f(GLfloat x,GLfloat y,GLfloat z,GLfloat w){
 assert(isfinite(x) && isfinite(y) && isfinite(z) && w>0);
 assert(state.texture==1 && state.env==GL_MODULATE && state.enables==7);
 assert(state.src==GL_SRC_ALPHA && state.dst==GL_ONE_MINUS_SRC_ALPHA && state.write==GL_FALSE && state.depth==GL_LEQUAL);
 assert(state.color[3]>=0 && state.color[3]<=0.34f);vertices++;
}
void VR_TraceDraw(const RenderVertex *v,int n,const void *s,size_t size){
 assert(polyCount<100 && n>=3 && n<=5 && !s && !size);
 memcpy(polys[polyCount],v,n*sizeof(*v));counts[polyCount++]=n;
}
static uint64_t draw(const float m[16],VRVec camera,VRFov f,unsigned tick,int frozen){
 State incoming={39,GL_ADD,GL_ONE,GL_ZERO,GL_GREATER,GL_TRUE,127,{.1f,.2f,.3f,.4f},{.5f,.6f}};
 state=incoming;polyCount=0;vertices=0;int oldUploads=uploads;
 vr_water_draw(m,camera,f,tick,frozen,88064);
 assert(!memcmp(&state,&incoming,sizeof state) && !pushed && oldUploads==uploads);
 uint64_t h=1469598103934665603ULL;
 for(int i=0;i<polyCount;i++)for(int j=0;j<counts[i];j++){
  RenderVertex v=polys[i][j];
  /* Undo the actual FOV projection and native rigid view transform. Every
   * emitted corner must still lie on Y=0 with UVs tied to its world XZ. */
  float l=tanf(f.left),r=tanf(f.right),u=tanf(f.up),d=tanf(f.down),z=1/v.rhw;
  float cx=((v.sx/320-1)*(r-l)+r+l)*z*.5f;
  float cy=((1-v.sy/240)*(u-d)+u+d)*z*.5f;
  float wx=camera.x+m[0]*cx+m[1]*cy+m[2]*z;
  float wy=camera.y+m[4]*cx+m[5]*cy+m[6]*z;
  float wz=camera.z+m[8]*cx+m[9]*cy+m[10]*z;
  float phase=frozen?0:(float)(tick&2047);
  assert(fabsf(wy)<0.02f);
  assert(fabsf(v.u-(wx/512+phase/2048))<0.0001f);
  assert(fabsf(v.v-(wz/512-phase/1024))<0.0001f);
  const unsigned char *p=(void*)&v;for(size_t k=0;k<sizeof v;k++)h=(h^p[k])*1099511628211ULL;
 }
 return h;
}
static void sample(float x,float y,float *u,float *v){
 for(int p=0;p<polyCount;p++)for(int i=1;i<counts[p]-1;i++){
  RenderVertex a=polys[p][0],b=polys[p][i],c=polys[p][i+1];
  float den=(b.sy-c.sy)*(a.sx-c.sx)+(c.sx-b.sx)*(a.sy-c.sy);
  if(fabsf(den)<1e-6f)continue;
  float wa=((b.sy-c.sy)*(x-c.sx)+(c.sx-b.sx)*(y-c.sy))/den;
  float wb=((c.sy-a.sy)*(x-c.sx)+(a.sx-c.sx)*(y-c.sy))/den,wc=1-wa-wb;
  if(wa<0 || wb<0 || wc<0)continue;
  wa*=a.rhw;wb*=b.rhw;wc*=c.rhw;
  *u=(wa*a.u+wb*b.u+wc*c.u)/(wa+wb+wc);*v=(wa*a.v+wb*b.v+wc*c.v)/(wa+wb+wc);return;
 }
 assert(!"world sample must fall inside the emitted plane");
}
int main(void){
 vr_water_init();vr_water_init();assert(generated==1 && uploads==7 && levelMask==127);
 assert(minFilter==GL_LINEAR_MIPMAP_LINEAR && magFilter==GL_LINEAR && wrapS==GL_REPEAT && wrapT==GL_REPEAT);
 float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
 VRFov f[2]={{atanf(-1.2f),atanf(.8f),atanf(.85f),atanf(-.65f)},
              {atanf(-.8f),atanf(1.2f),atanf(.85f),atanf(-.65f)}};
 VRVec c[2]={{-2302.56f,226,-2962},{-2297.44f,226,-2962}};
 uint64_t previous=0;int changes=0;
 for(unsigned tick=80;tick<=110;tick++){
  uint64_t left=draw(m,c[0],f[0],tick,0),right=draw(m,c[1],f[1],tick,0);
  assert(left!=right && vertices>0);
  assert(draw(m,c[1],f[1],tick,0)==right);assert(draw(m,c[0],f[0],tick,0)==left);
  if(previous && previous!=left)changes++;previous=left;
  float uv[2][2];
  for(int eye=0;eye<2;eye++){
   draw(m,c[eye],f[eye],tick,0);
   float nx,ny;vr_project((VRVec){-2300-c[eye].x,-226,-1500},f[eye],&nx,&ny);
   sample((nx+1)*320,(1-ny)*240,&uv[eye][0],&uv[eye][1]);
  }
  assert(fabsf(uv[0][0]-uv[1][0])<0.00001f && fabsf(uv[0][1]-uv[1][1])<0.00001f);
 }
 assert(changes==30);
 assert(draw(m,c[0],f[0],80,1)==draw(m,c[0],f[0],110,1));
 assert(draw(m,c[0],f[0],0,0)==draw(m,c[0],f[0],2048,0));
 /* Translate the camera around one fixed patch: texture phase stays in
  * world space instead of following the screen or a camera-snapped grid. */
 for(int step=-4;step<=4;step++){
  VRVec moving={c[0].x+step*40,c[0].y+step*10,c[0].z+step*60};
  draw(m,moving,f[0],90,0);
  float nx,ny,u,v;
  /* OpenXR forward is negative Z; world point is1500 units ahead initially. */
  vr_project((VRVec){-2300-moving.x,-moving.y,moving.z+1462},f[0],&nx,&ny);
  sample((nx+1)*320,(1-ny)*240,&u,&v);
  assert(fabsf(u-(-2300.0f/512+90.0f/2048))<0.00001f);
  assert(fabsf(v-(-1462.0f/512-90.0f/1024))<0.00001f);
 }
 /* Head yaw/pitch/roll and near-plane crossings keep the same world UV map. */
 const float base[9]={1,0,0,0,1,0,0,0,1};
 for(int i=-8;i<=8;i++){
  VRQuat q=vr_mul((VRQuat){0,sinf(i*.07f),0,cosf(i*.07f)},(VRQuat){sinf(.15f),0,0,cosf(.15f)});
  q=vr_mul(q,(VRQuat){0,0,sinf(i*.09f),cosf(i*.09f)});
  float out[9];vr_compose_basis(base,q,out);
  for(int r=0;r<3;r++)for(int col=0;col<3;col++)m[r*4+col]=out[r*3+col];
  draw(m,c[0],f[0],90,0);
 }
 draw(m,(VRVec){0,-1,0},f[0],90,0);assert(polyCount==0);
 vr_water_shutdown();vr_water_shutdown();assert(deleted==1);
 printf("water texture: 64x64, 7 mip levels, upload hash=%016llx, one lifetime\n",(unsigned long long)textureHash);
 puts("production water: 31 ticks, stereo repeat exact, 30 animation changes; world UVs agree across asymmetric eyes; pose/near clip, freeze, wrap and GL state pass");
}
