#include "vr.h"
#if SONICR_VR
#define GL_GLEXT_PROTOTYPES
#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#include "vr_water.h"
#include "r_state.h"
#include "r_draw.h"
#include "sonicr_globals.h"
#include <math.h>

static GLuint waterTexture;

void vr_water_init(void)
{
    if(waterTexture)return;

    /* Texture creation changes server texture state. Close the native eye
     * batch and let the renderer cache catch up before scoping raw GL calls. */
    R_EndBatch();
    R_FlushState();
    unsigned char pixels[64*64*4];
    for(int y=0;y<64;y++)for(int x=0;x<64;x++) {
        float u=(x+0.5f)/64,v=(y+0.5f)/64;
        float wave=0.5f+0.5f*sinf(6.28318530718f*(6*v+0.35f*sinf(6.28318530718f*u)
                                                         +0.12f*sinf(12.56637061436f*u)));
        float crest=wave*wave*wave;
        unsigned char *p=pixels+4*(y*64+x);
        p[0]=(unsigned char)(31+80*crest);
        p[1]=(unsigned char)(94+90*crest);
        p[2]=(unsigned char)(125+90*crest);
        p[3]=255;
    }

    glPushAttrib(GL_TEXTURE_BIT);
    glGenTextures(1,&waterTexture);
    glBindTexture(GL_TEXTURE_2D,waterTexture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    /* Supply the full mip chain without a driver-generated mip pass. */
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
    if(!waterTexture)return;
    /* The object may still be in the adjacent eye batch/cache. Restore the
     * caller's tracked texture first, then delete only after that barrier. */
    R_EndBatch();
    R_FlushState();
    glDeleteTextures(1,&waterTexture);
    waterTexture=0;
}

typedef struct {float x,y,z,u,v,alpha;} WaterVertex;

static WaterVertex lerp(WaterVertex a,WaterVertex b,float t)
{
    return (WaterVertex){a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,a.z+(b.z-a.z)*t,
        a.u+(b.u-a.u)*t,a.v+(b.v-a.v)*t,a.alpha+(b.alpha-a.alpha)*t};
}

static void emit(const WaterVertex in[4],float farClip)
{
    /* Native camera depth is positive in this renderer. Clip against its
     * one-unit near plane, then submit legacy-canvas vertices so the shared
     * VR transform applies the cached asymmetric FOV and reverse depth once. */
    WaterVertex clipped[5];
    int count=0;
    for(int i=0;i<4;i++) {
        WaterVertex a=in[i],b=in[(i+1)%4];
        int aIn=a.z>=1.0f,bIn=b.z>=1.0f;
        if(aIn)clipped[count++]=a;
        if(aIn!=bIn) {
            WaterVertex intersection=lerp(a,b,(1.0f-a.z)/(b.z-a.z));
            intersection.z=1.0f;
            clipped[count++]=intersection;
        }
    }
    if(count<3)return;

    RenderVertex vertices[5];
    float alphaSum=0;
    for(int i=0;i<count;i++) {
        WaterVertex v=clipped[i];
        unsigned alpha=(unsigned)(255*fmaxf(0,fminf(0.34f,v.alpha)));
        vertices[i]=(RenderVertex){
            g_screenCenterX+v.x*g_projScaleXCurrent/v.z,
            g_screenCenterY-v.y*g_projScaleY/v.z,
            v.z/farClip,1/v.z,(alpha<<24)|0x00ffffffu,0,v.u,v.v
        };
        alphaSum+=alpha;
    }
    if(alphaSum==0)return;
    R_DrawTriFan(vertices,count);
}

void vr_water_draw(const float view[16],VRVec camera,
                   unsigned tick,int frozen,float farClip)
{
    if(!waterTexture || camera.y<=0 || farClip<=1)return;

    /* Native land tiles and the water-ring holes draw afterward and mask this
     * plane. It never writes depth, so it leaves their ordering untouched. */
    float extent=fminf(8192,farClip*0.9f);
    static const float offsets[11]={-1,-0.5f,-0.25f,-0.125f,-0.0625f,0,
                                    0.0625f,0.125f,0.25f,0.5f,1};
    float phase=frozen?0:(float)(tick&2047);
    WaterVertex grid[11][11];
    for(int z=0;z<11;z++)for(int x=0;x<11;x++) {
        float dx=offsets[x]*extent,dz=offsets[z]*extent,dy=-camera.y;
        float fade=fmaxf(0,1-sqrtf(dx*dx+dz*dz)/(extent*0.85f));
        grid[z][x]=(WaterVertex){
            dx*view[0]+dy*view[4]+dz*view[8],
            dx*view[1]+dy*view[5]+dz*view[9],
            dx*view[2]+dy*view[6]+dz*view[10],
            (camera.x+dx)/512+phase/2048,
            (camera.z+dz)/512-phase/1024,
            0.34f*fade
        };
    }

    R_PushState();
    R_SetTextureObject(waterTexture);
    R_SetTexEnv(R_TEXENV_MODULATE);
    R_SetBlendMode(R_BLEND_ALPHA);
    R_SetDepthTest(1);
    R_SetDepthFunc(R_DEPTH_LEQUAL);
    R_SetDepthWrite(0);
    R_SetAlphaTest(0);
    R_SetCullMode(R_CULL_NONE);
    for(int z=0;z<10;z++)for(int x=0;x<10;x++) {
        WaterVertex q[4]={grid[z][x],grid[z][x+1],grid[z+1][x+1],grid[z+1][x]};
        emit(q,farClip);
    }
    R_EndBatch();
    R_PopState();
    R_FlushState();
}
#endif
