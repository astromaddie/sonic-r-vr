#include "vr.h"
#if SONICR_VR
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "vr_water.h"
#include "r_state.h"
#include <math.h>

static GLuint waterTexture;

void vr_water_init(void)
{
    if(waterTexture)return;
    /* Seamless, broad curved wave crests. Colour is deliberately muted: the
     * native reflection remains visible through the surface's vertex alpha. */
    unsigned char pixels[64*64*4];
    for(int y=0;y<64;y++)for(int x=0;x<64;x++) {
        float u=(x+0.5f)/64,v=(y+0.5f)/64;
        float wave=0.5f+0.5f*sinf(6.28318530718f*(6*v+0.35f*sinf(6.28318530718f*u)
                                                         +0.12f*sinf(12.56637061436f*u)));
        float crest=wave*wave*wave;
        unsigned char *p=pixels+4*(y*64+x);
        p[0]=(unsigned char)(31+80*crest);
        p[1]=(unsigned char)(94+90*crest);
        p[2]=(unsigned char)(125+90*crest);p[3]=255;
    }
    glPushAttrib(GL_TEXTURE_BIT);
    glGenTextures(1,&waterTexture);glBindTexture(GL_TEXTURE_2D,waterTexture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    /* Supply the complete mip chain without a driver-generated mip pass. */
    for(int size=64,level=0;;size/=2,level++) {
        glTexImage2D(GL_TEXTURE_2D,level,GL_RGBA,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels);
        if(size==1)break;
        int next=size/2;
        for(int y=0;y<next;y++)for(int x=0;x<next;x++)for(int c=0;c<4;c++) {
            int i=4*(2*y*size+2*x)+c;
            pixels[4*(y*next+x)+c]=(pixels[i]+pixels[i+4]+pixels[i+4*size]+pixels[i+4*size+4]+2)/4;
        }
    }
    glPopAttrib();
}
void vr_water_shutdown(void)
{
    if(waterTexture)glDeleteTextures(1,&waterTexture);
    waterTexture=0;
}

typedef struct {float x,y,z,u,v,alpha;} WaterVertex;
static WaterVertex lerp(WaterVertex a,WaterVertex b,float t)
{
    return (WaterVertex){a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,
        a.u+(b.u-a.u)*t,a.v+(b.v-a.v)*t,a.alpha+(b.alpha-a.alpha)*t};
}
static void emit(const WaterVertex in[4],VRFov fov,float farClip)
{
    /* Clip in camera space, keeping perspective texture coordinates intact. */
    WaterVertex clipped[5];int count=0;
    for(int i=0;i<4;i++) {
        WaterVertex a=in[i],b=in[(i+1)%4];
        int aIn=a.z>=1,bIn=b.z>=1;
        if(aIn)clipped[count++]=a;
        if(aIn!=bIn)clipped[count++]=lerp(a,b,(1-a.z)/(b.z-a.z));
    }
    if(count<3)return;
    RenderVertex vertices[5];float alphaSum=0;
    for(int i=0;i<count;i++) {
        WaterVertex v=clipped[i];float nx,ny;
        vr_project((VRVec){v.x,v.y,-v.z},fov,&nx,&ny);
        unsigned alpha=(unsigned)(255*fmaxf(0,fminf(0.34f,v.alpha)));
        vertices[i]=(RenderVertex){(nx+1)*320,(1-ny)*240,v.z/farClip,1/v.z,
                                    (alpha<<24)|0x00ffffff,0,v.u,v.v};
        alphaSum+=alpha;
    }
    if(alphaSum==0)return;
    VR_TraceDraw(vertices,count,NULL,0);
    glBegin(GL_TRIANGLE_FAN);
    for(int i=0;i<count;i++) {
        RenderVertex v=vertices[i];float w=1/v.rhw;
        glColor4f(1,1,1,(v.color>>24)/255.0f);glTexCoord2f(v.u,v.v);
        /* Already projected through the exact eye FOV. Do not use the sky
         * layer's angular reprojection or apply its roll a second time. */
        glVertex4f(v.sx*w,v.sy*w,v.sz*w,w);
    }
    glEnd();
}
void vr_water_draw(const float view[16],VRVec camera,VRFov fov,
                   unsigned tick,int frozen,float farClip)
{
    if(!waterTexture || camera.y<=0 || farClip<=1)return;
    /* Native land tiles/track mask this pass by drawing after it. It writes
     * no depth, just like the reflection underneath, and cannot cover land
     * already rendered. Y=0 is the native playfield/water-ring plane. */
    float extent=fminf(8192,farClip*0.9f);
    static const float offsets[11]={-1,-0.5f,-0.25f,-0.125f,-0.0625f,0,
                                    0.0625f,0.125f,0.25f,0.5f,1};
    float phase=frozen?0:(float)(tick&2047);
    WaterVertex grid[11][11];
    for(int z=0;z<11;z++)for(int x=0;x<11;x++) {
        float dx=offsets[x]*extent,dz=offsets[z]*extent,dy=-camera.y;
        float fade=fmaxf(0,1-sqrtf(dx*dx+dz*dz)/(extent*0.85f));
        grid[z][x]=(WaterVertex){dx*view[0]+dy*view[4]+dz*view[8],
            dx*view[1]+dy*view[5]+dz*view[9],dx*view[2]+dy*view[6]+dz*view[10],
            (camera.x+dx)/512+phase/2048,(camera.z+dz)/512-phase/1024,0.34f*fade};
    }
    /* Flush the native cache before saving actual GL state; restore exactly
     * afterward so its texture/state cache continues to describe the driver. */
    R_FlushState();
    glPushAttrib(GL_ENABLE_BIT|GL_TEXTURE_BIT|GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT|GL_CURRENT_BIT);
    glEnable(GL_TEXTURE_2D);glBindTexture(GL_TEXTURE_2D,waterTexture);
    glTexEnvi(GL_TEXTURE_ENV,GL_TEXTURE_ENV_MODE,GL_MODULATE);
    glEnable(GL_BLEND);glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);glDepthFunc(GL_LEQUAL);glDepthMask(GL_FALSE);
    glDisable(GL_ALPHA_TEST);glDisable(GL_CULL_FACE);glDisable(GL_LIGHTING);glDisable(GL_FOG);
    for(int z=0;z<10;z++)for(int x=0;x<10;x++) {
        WaterVertex q[4]={grid[z][x],grid[z][x+1],grid[z+1][x+1],grid[z+1][x]};
        emit(q,fov,farClip);
    }
    glPopAttrib();
}
#endif
