#ifndef SONICR_VR_MATH_H
#define SONICR_VR_MATH_H
/* Standalone, allocation-free math; all tracking lengths are metres. */
#include <math.h>
#include <stdint.h>
typedef struct { float x,y,z; } VRVec;
typedef struct { float x,y,z,w; } VRQuat;
typedef struct { VRQuat q; VRVec p; } VRPose;
typedef struct { float left,right,up,down; } VRFov;
typedef struct { int64_t next; float alpha; int cut; } VRClock;
static inline VRVec vr_add(VRVec a,VRVec b) { return (VRVec){a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline VRVec vr_sub(VRVec a,VRVec b) { return (VRVec){a.x-b.x,a.y-b.y,a.z-b.z}; }
static inline VRVec vr_scale(VRVec a,float s) { return (VRVec){a.x*s,a.y*s,a.z*s}; }
static inline VRVec vr_cross(VRVec a,VRVec b) { return (VRVec){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
static inline VRQuat vr_inverse(VRQuat q) { return (VRQuat){-q.x,-q.y,-q.z,q.w}; }
static inline VRVec vr_rotate(VRQuat q,VRVec v) {
    VRVec u={q.x,q.y,q.z},t=vr_scale(vr_cross(u,v),2);
    return vr_add(v,vr_add(vr_scale(t,q.w),vr_cross(u,t)));
}
static inline float vr_angle(float a,float b,float t) {
    return a+remainderf(b-a,6.28318530718f)*t;
}
/* A HUD basis follows gaze yaw/pitch, while its up axis stays roll-free. */
static inline VRQuat vr_mul(VRQuat a,VRQuat b) {
    return (VRQuat){a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,
        a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
        a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,
        a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
}
static inline VRQuat vr_without_roll(VRQuat q) {
    VRVec forward=vr_rotate(q,(VRVec){0,0,-1});
    float yaw=atan2f(-forward.x,-forward.z);
    float pitch=asinf(fmaxf(-1,fminf(1,forward.y)));
    return vr_mul((VRQuat){0,sinf(yaw/2),0,cosf(yaw/2)},
                  (VRQuat){sinf(pitch/2),0,0,cosf(pitch/2)});
}
/* Anchor only after focus settles; losing focus restarts the settle window. */
static inline int vr_anchor_ready(int focused,int64_t time,int64_t *since) {
    if(!focused){*since=0;return 0;}
    if(!*since)*since=time;
    return time-*since>=400000000LL;
}
/* Native matrix is stored [world axis][camera axis], camera +Z forward.
 * OpenXR is +X right,+Y up,-Z forward. Reflect Z on both sides of q. */
static inline void vr_compose_basis(const float base[9],VRQuat q,float out[9]) {
    const VRVec axis[3]={{1,0,0},{0,1,0},{0,0,-1}};
    for(int c=0;c<3;c++) {
        VRVec v=vr_rotate(q,axis[c]); float a[3]={v.x,v.y,-v.z};
        for(int r=0;r<3;r++) out[r*3+c]=base[r*3]*a[0]+base[r*3+1]*a[1]+base[r*3+2]*a[2];
    }
}
static inline VRVec vr_game_offset(const float base[9],VRVec p,float units) {
    float a[3]={p.x,p.y,-p.z},o[3]={0};
    for(int r=0;r<3;r++) for(int c=0;c<3;c++) o[r]+=base[r*3+c]*a[c]*units;
    return (VRVec){o[0],o[1],o[2]};
}
static inline void vr_project(VRVec p,VRFov f,float *x,float *y) {
    float l=tanf(f.left),r=tanf(f.right),u=tanf(f.up),d=tanf(f.down);
    *x=2*p.x/(-p.z*(r-l))-(r+l)/(r-l);
    *y=2*p.y/(-p.z*(u-d))-(u+d)/(u-d);
}
/* Same projection arithmetic, with the eye's invariant trig evaluated once.
 * Keep vr_project above as the independent uncached reference. */
typedef struct { float left,right,up,down; } VRProjection;
static inline VRProjection vr_projection(VRFov f) {
    return (VRProjection){tanf(f.left),tanf(f.right),tanf(f.up),tanf(f.down)};
}
static inline void vr_project_cached(VRVec p,VRProjection f,float *x,float *y) {
    float l=f.left,r=f.right,u=f.up,d=f.down;
    *x=2*p.x/(-p.z*(r-l))-(r+l)/(r-l);
    *y=2*p.y/(-p.z*(u-d))-(u+d)/(u-d);
}
/* Native s contains each producer's depth bias and far-divisor adjustment.
 * A shared reciprocal map makes unbiased planes affine in screen space while
 * retaining the ordering of those biases. Reversed depth is computed directly,
 * never by subtracting a rounded forward depth from one. s=1 maps to zero.
 * The largest active world divisor is F+64; its one-unit minimum sets n.
 * Below n, use a finite clipped continuation (normal producers clip first). */
static inline float vr_depth_near(float far) { return 1.0f/((far>0?far:88064.0f)+64.0f); }
static inline float vr_depth_map(float s,float n) {
    if(s<=0)return 2;
    if(s<n)return 2-s/n;
    return n*((1-s)/s)/(1-n);
}
static inline float vr_depth_unmap(float d,float n) {
    return d>1?n*(2-d):n/(d*(1-n)+n);
}
/* One tick maximum per display frame. After a stall, re-anchor rather than
 * accelerating gameplay. Tick N is presented between N-1 and N (one tick lag). */
static inline int vr_clock_step(VRClock *c,int64_t display,int visible) {
    const int64_t tick=1000000000LL/30;
    c->cut=0;
    if(!visible) { c->next=0; c->alpha=1; c->cut=1; return 0; }
    if(!c->next || display-c->next>2*tick || display<c->next-tick) {
        c->next=display; c->cut=1;
    }
    int due=display>=c->next;
    if(due) c->next+=tick;
    c->alpha=1-(float)(c->next-display)/(float)tick;
    if(c->alpha<0)c->alpha=0;
    if(c->alpha>1)c->alpha=1;
    return due;
}
#endif
