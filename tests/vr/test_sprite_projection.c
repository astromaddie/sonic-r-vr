/* Production billboard and panorama draw-stream regression. The draw boundary
 * records vertices; camera, visibility, animation and projection remain real. */
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

extern void SubmitCharacterSprite(Player *,int,int);
void VR_WaterSurface(void) {}
int VR_ReflectionFade(void) { return 0; }
/* The full comfort fixture owns the new eye-only fade branch. */
void R_SetDepthTest(int d) { (void)d;assert(0); }
void R_SetAlphaTest(int d) { (void)d;assert(0); }
extern void RenderItemSprite(Player *,int,int);
extern void RenderPlayerItemEffect(Player *,int,int,int);
extern void RenderHiddenSubEntry(int x,int y,int z);
int g_farClipTimes8;
int g_itemEffectAnimPhase;
float g_uvLUT256[256];
static int active=1, count;
static RenderVertex draws[2048][4];
int VR_EyeActive(void) { return active; }
/* This historical regression retains the shared 2D water/legacy-sky branch.
 * Full rotation-only sky production coverage lives in run-pose-render.sh. */
int VR_SkyRotation(void) { return 0; }
int VR_DrawPanorama(int t,float p,float h) {(void)t;(void)p;(void)h;assert(0);return 0;}
void R_SetTexEnv(R_TexEnvMode m) {(void)m;}
void R_SetTexture(int t) {(void)t;}
void R_SetFilter(R_FilterMode f) {(void)f;}
void R_SetBlendMode(R_BlendMode m) {(void)m;}
void R_SetDepthWrite(int d) {(void)d;}
void R_PushState(void) {}
void R_PopState(void) {}
void R_DrawQuad(const RenderVertex v[4]) {
    assert(count<2048);memcpy(draws[count++],v,sizeof(draws[0]));
}

static void camera(int x,int cx) {
    g_camIntX=x;g_camIntY=g_camIntZ=0;
    g_camOrientX=x*4096;g_camOrientY=g_camOrientZ=0;
    memset(g_viewMtx,0,sizeof(int)*16);
    g_viewMtx[0]=g_viewMtx[5]=g_viewMtx[10]=4096;
    g_screenCenterX=cx;g_screenCenterY=240;
    g_projScaleXCurrent=g_projScaleX=320;g_projScaleY=320;
    g_clipLeft=g_clipTop=0;g_clipRight=639;g_clipBottom=479;
    count=0;
}
static uint64_t stream_hash(void) {
    uint64_t h=1469598103934665603ULL;
    const unsigned char *p=(const unsigned char*)draws;
    for(size_t i=0;i<(size_t)count*sizeof(draws[0]);i++)h=(h^p[i])*1099511628211ULL;
    return h;
}
static uint64_t scene(int x,int center,int phase) {
    camera(x,center);g_ringAnimFrame=phase%16;g_totalFrames=phase;
    RenderHiddenSubEntry(0,0,16000);
    CollectEffect effects[COLLECT_EFFECT_COUNT]={0};
    effects[0].lifetime=1;effects[0].billboardSize=12;
    effects[0].posZ=16000*256;effects[0].tpage=1;effects[0].uvSpan=16;
    effects[0].timer=phase%16;effects[0].animDiv=1;
    DrawCollectEffectsD3D(effects);
    Player player={0};player.posZ=16000*4096;
    SubmitCharacterSprite(&player,0,0);
    RenderItemSprite(&player,0,0);
    RenderPlayerItemEffect(&player,0,-1,0);
    assert(count==5);
    static float uv[5][4][2];
    for(int i=0;i<5;i++)for(int j=0;j<4;j++) {
        if(center==280){uv[i][j][0]=draws[i][j].u;uv[i][j][1]=draws[i][j].v;}
        else assert(uv[i][j][0]==draws[i][j].u && uv[i][j][1]==draws[i][j].v);
    }
    g_parallaxState[0]=240;g_parallaxState[1]=512+phase%256;
    RenderParallaxStripsD3D(0);
    RenderWaterReflectionD3D(0);
    return stream_hash();
}
int main(int argc,char **argv) {
    active=argc==1 || strcmp(argv[1],"--flat")!=0;
#if !SONICR_VR
    active=0;
#endif
    g_farClipFloat=88064;g_farClipTimes8=88064;
    g_tpageCharBase=1;g_tpageCount=2;
    g_tpageStateArray[1]=g_tpageStateArray[2]=4;
    for(int i=0;i<256;i++)g_uvLUT256[i]=(float)i/256;
    for(int i=0;i<4096;i++)g_sinTable[i]=(int)(sinf(i*6.28318530718f/4096)*4096);
    g_viewportHeight=g_screenHeight=480;g_screenWidthFull=640;
    g_d3dSurfaceMode=4;g_parallaxWidth=1664;
    uint64_t temporal=1469598103934665603ULL;
    for(int frame=0;frame<81;frame++) {
        uint64_t left=scene(frame-43,280,frame),right=scene(frame-37,360,frame);
        assert(scene(frame-37,360,frame)==right);
        assert(scene(frame-43,280,frame)==left);
        assert(scene(frame-43,280,frame)==left);
        assert(scene(frame-37,360,frame)==right);
        temporal=(temporal^left)*1099511628211ULL;
        temporal=(temporal^right)*1099511628211ULL;
    }
    printf("temporal draw stream: frames=81 passes=486 order=exact flat=%d hash=%016llx\n",!active,(unsigned long long)temporal);
    if(!active)return 0;
    int collapsed=0, reversed=0;
    for(int x=-40;x<=40;x++) {
        camera(x,320);RenderHiddenSubEntry(0,0,16000);
        assert(count==1);
        float width=draws[0][1].sx-draws[0][0].sx;
        if(width<=0)collapsed++;
        if(fabsf(width-24.0f*320/16000)>0.0001f)reversed++;
    }
    printf("ring sweep: collapsed=%d incorrect_width=%d /81\n",collapsed,reversed);
    int effectCollapsed=0;
    CollectEffect effects[COLLECT_EFFECT_COUNT]={0};
    effects[0].lifetime=1;effects[0].billboardSize=12;
    effects[0].posZ=16000*256;effects[0].tpage=1;effects[0].uvSpan=16;
    for(int x=-40;x<=40;x++) {
        camera(x,320);DrawCollectEffectsD3D(effects);
        assert(count==1);
        if(draws[0][1].sx<=draws[0][0].sx)effectCollapsed++;
    }
    printf("effect sweep: collapsed=%d /81\n",effectCollapsed);
    /* The same yaw/pitch at infinity has the same ray for a strip boundary
     * in both eyes. Asymmetric FOV shifts the principal point, not the sky. */
    RenderVertex sky[2][4];
    for(int e=0;e<2;e++) {
        camera(0,e?360:280);
        g_parallaxState[0]=240;g_parallaxState[1]=512+32;
        RenderParallaxStripsD3D(0);assert(count>1);
        memcpy(sky[e],draws[1],sizeof(sky[e]));
    }
    float ray0=(sky[0][0].sx-280)/320;
    float ray1=(sky[1][0].sx-360)/320;
    printf("sky same-boundary ray mismatch=%.6f\n",fabsf(ray0-ray1));
    assert(collapsed==0 && reversed==0 && effectCollapsed==0);
    assert(fabsf(ray0-ray1)<0.0001f);
    static RenderVertex water[2][2048][4];int waterCount[2];
    for(int e=0;e<2;e++) {
        camera(0,e?360:280);RenderWaterReflectionD3D(0);
        waterCount[e]=count;memcpy(water[e],draws,count*sizeof(draws[0]));
    }
    int matched=0;
    for(int i=0;i<waterCount[0];i++) {
        const RenderVertex *a=water[0][i];
        if(a[0].sy!=240 || a[0].sx<160 || a[0].sx>400)continue;
        for(int j=0;j<waterCount[1];j++) {
            const RenderVertex *b=water[1][j];
            if(a[0].u!=b[0].u || a[0].v!=b[0].v || a[0].sy!=b[0].sy)continue;
            assert(fabsf((a[0].sx-280)/320-(b[0].sx-360)/320)<0.0001f);
            matched++;break;
        }
    }
    assert(matched>0);
    printf("water same-boundary rays: matched=%d\n",matched);
    puts("production sprite projection passed");
}
