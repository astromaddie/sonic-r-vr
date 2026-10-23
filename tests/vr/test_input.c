/* Production OpenXR module + native input consumers; fake only the XR device
 * API and unrelated pause/audio callbacks. No headset/window/Steam required. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "vr/vr_input.h"
#include "vr/vr_math.h"
#include "sonicr_globals.h"
#include "sonicr_functions.h"
#include "platform.h"
#include "pad_bits.h"
#include "gamepad_buttons.h"
extern void InitJoystickConfig(void);
extern char g_joystickSlots[4][282];
static char actionNames[32][64],paths[128][XR_MAX_PATH_LENGTH];
static int actionCount,pathCount,attached,suggested,frameSeen,frameAdvertised=1,rejectFrame,failAttach,destroyed;
static XrResult syncResult=XR_SUCCESS;
static uint32_t physical;
static float axisX,axisY,triggerL,triggerR;
static int buttonForAction(XrAction action) {
    const char *name=actionNames[(uintptr_t)action];
    const char *names[]={"a","b","x","y","view","unused","start","left_stick_click","right_stick_click","left_bumper","right_bumper","dpad_up","dpad_down","dpad_left","dpad_right","left_trigger","right_trigger"};
    for(int i=0;i<GC_BUTTON_COUNT;i++)if(!strcmp(names[i],name))return i;
    return -1;
}
XrResult xrEnumerateInstanceExtensionProperties(const char *layer,uint32_t cap,uint32_t *count,XrExtensionProperties *out) {
    assert(!layer);*count=frameAdvertised?1:0;
    if(cap && *count)strcpy(out[0].extensionName,"XR_VALVE_frame_controller_interaction");
    return XR_SUCCESS;
}
XrResult xrCreateActionSet(XrInstance i,const XrActionSetCreateInfo *info,XrActionSet *out) {
    (void)i;assert(!strcmp(info->actionSetName,"native_pad"));*out=(XrActionSet)(uintptr_t)1;return XR_SUCCESS;
}
XrResult xrCreateAction(XrActionSet set,const XrActionCreateInfo *info,XrAction *out) {
    assert(set && !attached);assert(actionCount+1<32);*out=(XrAction)(uintptr_t)++actionCount;
    strcpy(actionNames[actionCount],info->actionName);return XR_SUCCESS;
}
XrResult xrStringToPath(XrInstance i,const char *name,XrPath *out) {
    (void)i;for(int p=1;p<=pathCount;p++)if(!strcmp(paths[p],name)){*out=p;return XR_SUCCESS;}
    assert(++pathCount<128);strcpy(paths[pathCount],name);*out=pathCount;return XR_SUCCESS;
}
XrResult xrSuggestInteractionProfileBindings(XrInstance i,const XrInteractionProfileSuggestedBinding *info) {
    (void)i;assert(!attached);suggested++;
    int frame=strstr(paths[info->interactionProfile],"frame_controller")!=NULL;
    if(frame){assert(frameAdvertised);frameSeen++;}
    int start=0;
    for(uint32_t b=0;b<info->countSuggestedBindings;b++) {
        const char *path=paths[info->suggestedBindings[b].binding];
        assert(!strstr(path,"system/"));
        if(buttonForAction(info->suggestedBindings[b].action)==GCBTN_START) {
            start++;if(frame)assert(!strcmp(path,"/user/hand/right/input/menu/click"));
        }
    }
    assert(start==1);return frame && rejectFrame?XR_ERROR_PATH_UNSUPPORTED:XR_SUCCESS;
}
XrResult xrAttachSessionActionSets(XrSession session,const XrSessionActionSetsAttachInfo *info) {
    (void)session;assert(info->countActionSets==1 && suggested);if(failAttach)return XR_ERROR_RUNTIME_FAILURE;
    attached=1;return XR_SUCCESS;
}
XrResult xrDestroyActionSet(XrActionSet s) {(void)s;destroyed++;attached=0;return XR_SUCCESS;}
XrResult xrSyncActions(XrSession s,const XrActionsSyncInfo *info) {(void)s;assert(attached && info->countActiveActionSets==1);return syncResult;}
XrResult xrGetActionStateBoolean(XrSession s,const XrActionStateGetInfo *get,XrActionStateBoolean *out) {
    (void)s;int b=buttonForAction(get->action);assert(b>=0);out->isActive=1;out->currentState=(physical>>b)&1;return XR_SUCCESS;
}
XrResult xrGetActionStateFloat(XrSession s,const XrActionStateGetInfo *get,XrActionStateFloat *out) {
    (void)s;int b=buttonForAction(get->action);assert(b==15 || b==16);out->isActive=1;out->currentState=b==15?triggerL:triggerR;return XR_SUCCESS;
}
XrResult xrGetActionStateVector2f(XrSession s,const XrActionStateGetInfo *get,XrActionStateVector2f *out) {
    (void)s;(void)get;out->isActive=1;out->currentState=(XrVector2f){axisX,axisY};return XR_SUCCESS;
}
XrResult xrGetCurrentInteractionProfile(XrSession s,XrPath hand,XrInteractionProfileState *out) {
    (void)s;(void)hand;return xrStringToPath(0,"/interaction_profiles/valve/frame_controller_valve",&out->interactionProfile);
}
XrResult xrPathToString(XrInstance i,XrPath path,uint32_t cap,uint32_t *count,char *out) {
    (void)i;*count=(uint32_t)strlen(paths[path])+1;assert(cap>=*count);strcpy(out,paths[path]);return XR_SUCCESS;
}
XrResult xrEnumerateBoundSourcesForAction(XrSession s,const XrBoundSourcesForActionEnumerateInfo *info,uint32_t cap,uint32_t *count,XrPath *out) {
    (void)s;assert(buttonForAction(info->action)==GCBTN_START && cap>=1);*count=1;return xrStringToPath(0,"/user/hand/right/input/menu/click",out);
}
/* Same production wrapper as vr.c, with the renderer deliberately not linked. */
unsigned short VR_PadBits(void) {return vr_input_read(g_joystickConfigWords);}
static int pauseCalls;
int RunPauseMenu(void){pauseCalls++;g_isPaused=0;return 0;}
void StopAmbientSounds(void){}
void PlaySoundEffect(int a,int b,int c){(void)a;(void)b;(void)c;}
static void neutral(void) {physical=0;axisX=axisY=triggerL=triggerR=0;vr_input_sync(1);vr_input_tick();}
static void resetRuntime(void) {
    vr_input_shutdown();actionCount=pathCount=attached=suggested=frameSeen=0;
    physical=0;axisX=axisY=triggerL=triggerR=0;rejectFrame=failAttach=0;syncResult=XR_SUCCESS;
}
int main(void) {
    assert(SDL_Init(SDL_INIT_EVENTS)==0);
    assert(vr_input_has_frame_extension()==1);
    assert(vr_input_init((XrInstance)(uintptr_t)1,(XrSession)(uintptr_t)2,1));
    assert(frameSeen==1 && suggested==3 && attached);vr_input_profiles_changed();
    InitJoystickConfig();g_diDeviceReady=1;g_numViewports=1;g_introCountdown=0;g_demoMode=DEMO_NONE;g_ghostMaxFrames=0;
    /* Distinct persisted SDL per-device mappings must survive XR sampling. */
    for(int slot=0;slot<4;slot++)for(int b=0;b<JOY_SLOT_CFG_WORDS;b++)
        ((short*)&g_joystickSlots[slot][0x104])[b]=(short)(slot*19+b*7);
    char savedSlots[sizeof(g_joystickSlots)];memcpy(savedSlots,g_joystickSlots,sizeof(savedSlots));
    neutral();
    const int rates[]={72,90,120};
    for(int r=0;r<3;r++) {
        VRClock clock={0};int presses=0;neutral();
        for(int f=0;f<rates[r];f++) {
            physical=f==1?1u<<GCBTN_START:0;vr_input_sync(1);
            if(vr_clock_step(&clock,1000000000LL+(int64_t)f*1000000000LL/rates[r],1)) {
                vr_input_tick();ReadInput();presses+=(g_inputBits&8)!=0;
            }
        }
        assert(presses==1);
    }
    /* Press and release wholly between native ticks, at any display rate. */
    physical=1u<<GCBTN_START;vr_input_sync(1);physical=0;vr_input_sync(1);vr_input_tick();
    ReadInput();assert((g_inputBits&8)!=0);assert(g_initFeatureC==1); /* title Start, no SDL pad */
    ReadInput();assert((g_inputBits&8)!=0); /* repeated native polls are coherent */
    vr_input_tick();ReadInput();assert((g_inputBits&8)==0);
    physical=1u<<GCBTN_START;vr_input_sync(1);vr_input_tick();ReadInput();assert(g_inputBits&8);
    for(int i=0;i<10;i++){vr_input_sync(1);vr_input_tick();ReadInput();assert(!(g_inputBits&8));}
    neutral();
    /* Native menu high-byte mapping and production P1 gameplay routing. */
    physical=(1u<<GCBTN_A);axisY=1;vr_input_sync(1);vr_input_tick();ReadInput();
    assert((g_inputBits&4) && (g_inputBits&0x10));UpdatePerPlayerInput();
    assert((g_perPlayerInput[0]&(PAD_JUMP|PAD_UP))==(PAD_JUMP|PAD_UP));
    neutral();physical=1u<<GCBTN_B;axisX=-1;vr_input_sync(1);vr_input_tick();ReadInput();
    assert(g_inputBits&1);UpdatePerPlayerInput();assert((g_perPlayerInput[0]&(PAD_ACCEL|PAD_LEFT))==(PAD_ACCEL|PAD_LEFT));
    neutral();physical=1u<<GCBTN_X;axisX=1;axisY=-1;triggerL=triggerR=0.5f;vr_input_sync(1);vr_input_tick();ReadInput();UpdatePerPlayerInput();
    assert((g_perPlayerInput[0]&(PAD_ACCEL|PAD_RIGHT|PAD_DOWN|PAD_DRIFTL|PAD_DRIFTR))==(PAD_ACCEL|PAD_RIGHT|PAD_DOWN|PAD_DRIFTL|PAD_DRIFTR));
    /* Focus loss clears published/pending; held controls require release. */
    vr_input_sync(0);ReadInput();assert(!g_combinedInputState);
    vr_input_sync(1);vr_input_tick();ReadInput();assert(!g_combinedInputState);
    physical=0;triggerL=triggerR=0;axisX=-1;axisY=0;vr_input_sync(1);vr_input_tick();ReadInput();assert(!g_combinedInputState);
    neutral();physical=1u<<GCBTN_A;vr_input_sync(1);vr_input_tick();ReadInput();assert(g_inputBits&4);
    syncResult=XR_SESSION_NOT_FOCUSED;vr_input_sync(1);ReadInput();assert(!g_combinedInputState);syncResult=XR_SUCCESS;
    neutral();g_fadeState=FADE_VISIBLE;g_isPaused=g_pauseLatch=0;
    physical=1u<<GCBTN_START;vr_input_sync(1);vr_input_tick();ReadInput();UpdatePerPlayerInput();assert(pauseCalls==1);
    vr_input_sync(1);vr_input_tick();ReadInput();UpdatePerPlayerInput();assert(pauseCalls==1);
    assert(!memcmp(savedSlots,g_joystickSlots,sizeof(savedSlots)));
    resetRuntime();frameAdvertised=0;assert(vr_input_has_frame_extension()==0);
    assert(vr_input_init((XrInstance)(uintptr_t)1,(XrSession)(uintptr_t)2,0));assert(frameSeen==0 && suggested==2);
    resetRuntime();frameAdvertised=1;rejectFrame=1;assert(!vr_input_init((XrInstance)(uintptr_t)1,(XrSession)(uintptr_t)2,1));assert(!attached);
    resetRuntime();failAttach=1;assert(!vr_input_init((XrInstance)(uintptr_t)1,(XrSession)(uintptr_t)2,1));assert(!attached);
    resetRuntime();assert(destroyed==4);SDL_Quit();
    puts("VR input: action lifecycle, Frame right Menu, fallback profiles, native title/menu/race, short/held Start, focus clearing and SDL config preservation passed");
}
