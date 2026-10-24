#ifndef SONICR_VR_TRACK_H
#define SONICR_VR_TRACK_H
#include <string.h>
/* Eye-only track scratch. Polygon indices are uint16, so this covers the full
 * source index domain without changing SrcVertex's native binary layout. */
typedef struct { float x,y,z,u,v; int r,g,b; } VRTrackVertex;
static float vrTrackXYZ[65536][3];
static void VRTrackTransform(SrcVertex *v,float wx,float wy,float wz) {
    const float *m=(const float *)g_floatMtxDest;
    float x=wx-g_camFloatX,y=wy-g_camFloatY,z=wz-g_camFloatZ;
    float *p=vrTrackXYZ[(unsigned)(v-g_vertexArrayBase)];
    /* Same origin, matrix and grouped additions as the float grid producer. */
    p[0]=x*m[0]+(z*m[8]+y*m[4]);
    p[1]=x*m[1]+(z*m[9]+y*m[5]);
    p[2]=x*m[2]+(z*m[10]+y*m[6]);
    v->camX=(int)p[0];v->camY=(int)p[1];v->depth=(int)p[2];
    if(p[2]>=1){v->screenX=(int)(g_screenCenterX+g_projScaleXCurrent*p[0]/p[2]);v->screenY=(int)(g_screenCenterY-g_projScaleY*p[1]/p[2]);}
}
static int VRTrackVisible(float wx,float wy,float wz,float radius,float far) {
    const float *m=(const float *)g_floatMtxDest;
    float x=wx-g_camFloatX,y=wy-g_camFloatY,z=wz-g_camFloatZ;
    float cx=x*m[0]+(z*m[8]+y*m[4]),cy=x*m[1]+(z*m[9]+y*m[5]),cz=x*m[2]+(z*m[10]+y*m[6]);
    if(cz+radius<1 || cz-radius>far)return 0;
    float l=(g_clipLeft-g_screenCenterX)/(float)g_projScaleXCurrent;
    float r=(g_clipRight-g_screenCenterX)/(float)g_projScaleXCurrent;
    float u=(g_screenCenterY-g_clipTop)/(float)g_projScaleY;
    float d=(g_screenCenterY-g_clipBottom)/(float)g_projScaleY;
    return cx-l*cz>=-radius*sqrtf(1+l*l) && cx-r*cz<=radius*sqrtf(1+r*r) &&
           cy-d*cz>=-radius*sqrtf(1+d*d) && cy-u*cz<=radius*sqrtf(1+u*u);
}
static VRTrackVertex VRTrackLerp(VRTrackVertex a,VRTrackVertex b) {
    float t=(1-a.z)/(b.z-a.z);
    return (VRTrackVertex){a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t,1,
        a.u+(b.u-a.u)*t,a.v+(b.v-a.v)*t,
        a.r,a.g,a.b /* Native NearClipLerp keeps the behind vertex color. */};
}
static void VRTrackDraw(const int *face,float far,float farCull) {
    const unsigned char *bytes=(const unsigned char *)face;
    unsigned flags=bytes[0x2e],count=(flags&1)?4:3;
    VRTrackVertex input[4],poly[6];unsigned n=0;
    for(unsigned i=0;i<count;i++) {
        unsigned index=((const unsigned short *)(bytes+0x20))[i];
        const SrcVertex *v=g_vertexArrayBase+index;const float *p=vrTrackXYZ[index];
        if(p[2]>farCull)return; /* Native any-vertex far rejection. */
        input[i]=(VRTrackVertex){p[0],p[1],p[2],TrackUV(face[i*2]),TrackUV(face[i*2+1]),v->colorR,v->colorG,v->colorB};
    }
    /* Preserve the native fan root/diagonal and behind-to-front interpolation
     * order, including its diagonal-pair rejection for non-convex quads. */
    int behind[4],back=0;for(unsigned i=0;i<count;i++){behind[i]=input[i].z<1;back+=behind[i];}
    if(back==(int)count)return;
    if(!back){memcpy(poly,input,count*sizeof *poly);n=count;}
    else if(count==4) {
        if(back==1){
            int k=behind[0]?0:behind[1]?1:behind[2]?2:3;
            poly[0]=input[(k+2)&3];poly[1]=input[(k+3)&3];
            poly[2]=VRTrackLerp(input[k],input[(k+3)&3]);
            poly[3]=VRTrackLerp(input[k],input[(k+1)&3]);poly[4]=input[(k+1)&3];n=5;
        } else if(back==2){
            int k=-1;for(int i=0;i<4;i++)if(behind[i] && behind[(i+1)&3]){k=i;break;}
            if(k<0)return;
            poly[0]=VRTrackLerp(input[(k+1)&3],input[(k+2)&3]);
            poly[1]=input[(k+2)&3];poly[2]=input[(k+3)&3];
            poly[3]=VRTrackLerp(input[k],input[(k+3)&3]);n=4;
        } else {
            int k=!behind[0]?0:!behind[1]?1:!behind[2]?2:3;
            poly[0]=VRTrackLerp(input[(k+3)&3],input[k]);poly[1]=input[k];
            poly[2]=VRTrackLerp(input[(k+1)&3],input[k]);n=3;
        }
    } else {
        int k=back==1?(behind[0]?0:behind[1]?1:2):(!behind[0]?0:!behind[1]?1:2);
        if(back==1){poly[0]=VRTrackLerp(input[k],input[(k+1)%3]);poly[1]=input[(k+1)%3];poly[2]=input[(k+2)%3];poly[3]=VRTrackLerp(input[k],input[(k+2)%3]);n=4;}
        else {poly[0]=input[k];poly[1]=VRTrackLerp(input[(k+1)%3],input[k]);poly[2]=VRTrackLerp(input[(k+2)%3],input[k]);n=3;}
    }
    if(n<3)return;
    RenderVertex out[6];int left=1,right=1,top=1,bottom=1;
    for(unsigned i=0;i<n;i++) {
        VRTrackVertex *p=poly+i;RenderVertex *v=out+i;
        v->sx=g_screenCenterX+g_projScaleXCurrent*p->x/p->z;
        v->sy=g_screenCenterY-g_projScaleY*p->y/p->z;
        left&=v->sx<g_clipLeft;right&=v->sx>g_clipRight;top&=v->sy<g_clipTop;bottom&=v->sy>g_clipBottom;
        v->sz=p->z*reciprocal(far);v->rhw=reciprocal(p->z);v->u=p->u;v->v=p->v;v->specular=0;
        int r=p->r>>13,g=p->g>>13,b=p->b>>13;
        if(g_colorTintEnable){r+=g_colorTintR;g+=g_colorTintG;b+=g_colorTintB;}
        if(r>255)r=255;if(g>255)g=255;if(b>255)b=255;
        double fog=(double)p->z/far;
        int alpha=fog>FOG_FAR?0:fog<=FOG_NEAR?255:SRABS((int)((fog+FOG_NEG_FAR)*FOG_MUL));
        v->color=(unsigned)alpha<<24|(unsigned)r<<16|(unsigned)g<<8|(unsigned)b;
    }
    if(left || right || top || bottom)return;
    if(!(flags&4)) {
        int front=0;
        for(unsigned i=1;i+1<n;i++) {
            double cross=((double)out[0].sy-out[i].sy)*((double)out[i+1].sx-out[i].sx)-
                ((double)out[0].sx-out[i].sx)*((double)out[i+1].sy-out[i].sy);
            if(g_mirrorMode && !(count==3 && back))cross=-cross;
            if(cross>=0)front=1;
            if(back || !(flags&2))break;
        }
        if(!front)return;
    }
    R_SetTexture(bytes[0x28]);R_SetTexEnv(R_TEXENV_ADD_SIGNED);R_DrawTriFan(out,(int)n);
}
#endif
