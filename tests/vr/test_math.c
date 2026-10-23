#include "vr/vr_math.h"
#include <assert.h>
#include <stdio.h>
#define NEAR(a,b) assert(fabsf((a)-(b))<0.0001f)
int main(void) {
    const float base[9]={1,0,0,0,1,0,0,0,1};float m[9];
    vr_compose_basis(base,(VRQuat){0,0,0,1},m);
    for(int i=0;i<9;i++)NEAR(m[i],base[i]);
    VRQuat yaw={0,sqrtf(0.5f),0,sqrtf(0.5f)};
    vr_compose_basis(base,yaw,m);NEAR(m[2],-1);NEAR(m[8],0);
    VRVec p=vr_game_offset(base,(VRVec){0.032f,0,-0.2f},80);NEAR(p.x,2.56f);NEAR(p.z,16);
    VRVec left=vr_rotate(yaw,(VRVec){-0.032f,0,0}),right=vr_rotate(yaw,(VRVec){0.032f,0,0});
    NEAR(left.z-right.z,0.064f);NEAR(left.x,right.x);
    VRFov f={-atanf(0.8f),atanf(1.2f),atanf(1.1f),-atanf(0.9f)};
    float x,y;vr_project((VRVec){-0.8f,1.1f,-1},f,&x,&y);NEAR(x,-1);NEAR(y,1);
    vr_project((VRVec){1.2f,-0.9f,-1},f,&x,&y);NEAR(x,1);NEAR(y,-1);
    float xl,xr;vr_project((VRVec){0.032f,0,-1},f,&xl,&y);vr_project((VRVec){-0.032f,0,-1},f,&xr,&y);assert(xl>xr);
    float nearDisparity=xl-xr;vr_project((VRVec){0.032f,0,-10},f,&xl,&y);vr_project((VRVec){-0.032f,0,-10},f,&xr,&y);NEAR(nearDisparity,(xl-xr)*10);
    NEAR(vr_angle(3.1f,-3.1f,0.5f),3.14159265f);
    VRQuat yawRoll=vr_mul(yaw,(VRQuat){0,0,sinf(0.4f),cosf(0.4f)});
    VRQuat level=vr_without_roll(yawRoll);
    VRVec up=vr_rotate(level,(VRVec){0,1,0});NEAR(up.x,0);NEAR(up.y,1);NEAR(up.z,0);
    VRVec gaze=vr_rotate(level,(VRVec){0,0,-1});NEAR(gaze.x,-1);NEAR(gaze.y,0);
    int64_t since=0;assert(!vr_anchor_ready(0,1000000000LL,&since));
    assert(!vr_anchor_ready(1,1100000000LL,&since));
    assert(!vr_anchor_ready(1,1499999999LL,&since));
    assert(vr_anchor_ready(1,1500000000LL,&since));
    assert(!vr_anchor_ready(0,1600000000LL,&since));assert(since==0);
    const int rates[]={72,90,120};
    for(int rateIndex=0;rateIndex<3;rateIndex++) {
        VRClock cadence={0};int ticks=0,rate=rates[rateIndex];
        for(int i=0;i<rate*10;i++) {
            ticks+=vr_clock_step(&cadence,1000000000LL+(int64_t)i*1000000000LL/rate,1);
            assert(cadence.alpha>=0 && cadence.alpha<=1);
        }
        assert(ticks==300);
    }
    VRClock c={0};
    assert(!vr_clock_step(&c,12000000000LL,0));assert(c.cut);
    assert(vr_clock_step(&c,30000000000LL,1));assert(c.cut);
    assert(!vr_clock_step(&c,30000000001LL,1));
    assert(vr_clock_step(&c,40000000000LL,1));assert(c.cut);
    puts("VR math: identity, handedness, yaw, runtime IPD, off-axis frustum, depth disparity, angle wrap, 30 Hz simulation at 72/90/120 Hz, roll-free HUD, focus settle, standby and stall checks passed");
}
