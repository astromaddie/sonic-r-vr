#include "vr.h"
#if SONICR_VR
#include "vr_input.h"
#include "gamepad_buttons.h"
#include "pad_bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XrInstance instance;
static XrSession session;
static XrActionSet actionSet;
static XrAction buttons[GC_BUTTON_COUNT],stick;
static uint32_t held,pending,blocked,published;
static int wasFocused,ready,trace,stickBlocked;
static unsigned short lastReported;
static XrResult lastSyncStatus=XR_SUCCESS;
/* Stick directions are separate from rebindable buttons, as in SDL. */
#define STICK_LEFT (1u<<17)
#define STICK_RIGHT (1u<<18)
#define STICK_UP (1u<<19)
#define STICK_DOWN (1u<<20)
#define START (1u<<GCBTN_START)
#define STICK_DIRECTIONS (STICK_LEFT|STICK_RIGHT|STICK_UP|STICK_DOWN)
static const char *names[GC_BUTTON_COUNT]={"a","b","x","y","view",NULL,"start","left_stick_click","right_stick_click","left_bumper","right_bumper","dpad_up","dpad_down","dpad_left","dpad_right","left_trigger","right_trigger"};

typedef struct { int button;const char *path; } Binding;
#define LEFT "/user/hand/left/input/"
#define RIGHT "/user/hand/right/input/"
#define STICK -1
static const Binding frameBindings[]={
    {0,RIGHT "a/click"},{1,RIGHT "b/click"},{2,RIGHT "x/click"},{3,RIGHT "y/click"},
    {4,LEFT "view/click"},{6,RIGHT "menu/click"},
    {7,LEFT "thumbstick/click"},{8,RIGHT "thumbstick/click"},
    {9,LEFT "bumper/click"},{10,RIGHT "bumper/click"},
    {11,LEFT "dpad_up/click"},{12,LEFT "dpad_down/click"},{13,LEFT "dpad_left/click"},{14,LEFT "dpad_right/click"},
    {15,LEFT "trigger/value"},{16,RIGHT "trigger/value"},{STICK,LEFT "thumbstick"}
};
static const Binding touchBindings[]={
    {0,RIGHT "a/click"},{1,RIGHT "b/click"},{2,LEFT "x/click"},{3,LEFT "y/click"},
    {6,LEFT "menu/click"},{7,LEFT "thumbstick/click"},{8,RIGHT "thumbstick/click"},
    {9,LEFT "squeeze/value"},{10,RIGHT "squeeze/value"},
    {15,LEFT "trigger/value"},{16,RIGHT "trigger/value"},{STICK,LEFT "thumbstick"}
};
static const Binding indexBindings[]={
    {0,RIGHT "a/click"},{1,RIGHT "b/click"},{2,LEFT "a/click"},
    {6,LEFT "b/click"},{7,LEFT "thumbstick/click"},{8,RIGHT "thumbstick/click"},
    {9,LEFT "squeeze/value"},{10,RIGHT "squeeze/value"},
    {15,LEFT "trigger/value"},{16,RIGHT "trigger/value"},{STICK,LEFT "thumbstick"}
};
static int ok(XrResult r,const char *call) {
    if(r==XR_SUCCESS)return 1;
    fprintf(stderr,"[vr-input] %s result=%d\n",call,r);return 0;
}
int vr_input_has_frame_extension(void) {
    uint32_t count=0;
    if(!ok(xrEnumerateInstanceExtensionProperties(NULL,0,&count,NULL),"enumerate extensions"))return -1;
    XrExtensionProperties *ext=calloc(count,sizeof(*ext));if(!ext)return -1;
    for(uint32_t i=0;i<count;i++)ext[i].type=XR_TYPE_EXTENSION_PROPERTIES;
    XrResult result=xrEnumerateInstanceExtensionProperties(NULL,count,&count,ext);
    int found=0;
    if(ok(result,"enumerate extensions"))for(uint32_t i=0;i<count;i++)
        if(strcmp(ext[i].extensionName,"XR_VALVE_frame_controller_interaction")==0)found=1;
    free(ext);return result==XR_SUCCESS?found:-1;
}
static int suggest(const char *profile,const Binding *map,size_t count) {
    XrActionSuggestedBinding bindings[GC_BUTTON_COUNT+1];
    XrPath profilePath;
    if(!ok(xrStringToPath(instance,profile,&profilePath),"profile path"))return 0;
    for(size_t i=0;i<count;i++) {
        bindings[i].action=map[i].button==STICK?stick:buttons[map[i].button];
        if(!ok(xrStringToPath(instance,map[i].path,&bindings[i].binding),"binding path"))return 0;
    }
    XrInteractionProfileSuggestedBinding info={.type=XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING,
        .interactionProfile=profilePath,.countSuggestedBindings=(uint32_t)count,.suggestedBindings=bindings};
    XrResult r=xrSuggestInteractionProfileBindings(instance,&info);
    fprintf(stderr,"[vr-input] profile=%s bindings=%zu result=%d\n",profile,count,r);
    return r==XR_SUCCESS;
}
void vr_input_clear(void) { held=pending=blocked=published=0;wasFocused=stickBlocked=0; }
int vr_input_init(XrInstance inst,XrSession sess,int frameExtension) {
    instance=inst;session=sess;vr_input_clear();
    trace=getenv("SONICR_VR_INPUT_TRACE") && strcmp(getenv("SONICR_VR_INPUT_TRACE"),"1")==0;
    XrActionSetCreateInfo setInfo={.type=XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(setInfo.actionSetName,"native_pad");strcpy(setInfo.localizedActionSetName,"Sonic R gamepad");
    if(!ok(xrCreateActionSet(instance,&setInfo,&actionSet),"create action set"))return 0;
    for(int b=0;b<=GC_BUTTON_COUNT;b++) {
        if(b<GC_BUTTON_COUNT && !names[b])continue; /* System is never bound. */
        XrActionCreateInfo ai={.type=XR_TYPE_ACTION_CREATE_INFO,
            .actionType=b==GC_BUTTON_COUNT?XR_ACTION_TYPE_VECTOR2F_INPUT:
                b>=GCBTN_TRIGGER_LEFT?XR_ACTION_TYPE_FLOAT_INPUT:XR_ACTION_TYPE_BOOLEAN_INPUT};
        snprintf(ai.actionName,sizeof(ai.actionName),"%s",b==GC_BUTTON_COUNT?"left_stick":names[b]);
        snprintf(ai.localizedActionName,sizeof(ai.localizedActionName),"%s",ai.actionName);
        if(!ok(xrCreateAction(actionSet,&ai,b==GC_BUTTON_COUNT?&stick:&buttons[b]),"create action"))return 0;
    }
    int accepted=0;
    if(frameExtension) {
        /* A rejected advertised Frame map must not silently remap Menu to View. */
        if(!suggest("/interaction_profiles/valve/frame_controller_valve",frameBindings,sizeof(frameBindings)/sizeof(*frameBindings)))return 0;
        accepted=1;
    }
    accepted|=suggest("/interaction_profiles/valve/index_controller",indexBindings,sizeof(indexBindings)/sizeof(*indexBindings));
    accepted|=suggest("/interaction_profiles/oculus/touch_controller",touchBindings,sizeof(touchBindings)/sizeof(*touchBindings));
    if(!accepted)return 0;
    XrSessionActionSetsAttachInfo attach={.type=XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO,.countActionSets=1,.actionSets=&actionSet};
    if(!ok(xrAttachSessionActionSets(session,&attach),"attach action set"))return 0;
    ready=1;fprintf(stderr,"[vr-input] attached; Frame extension=%d; Frame right Menu=Start\n",frameExtension);return 1;
}
void vr_input_profiles_changed(void) {
    if(!ready)return;
    const char *hands[]={"/user/hand/left","/user/hand/right"};
    for(int h=0;h<2;h++) {
        XrPath path;XrInteractionProfileState state={.type=XR_TYPE_INTERACTION_PROFILE_STATE};
        char name[XR_MAX_PATH_LENGTH]="none";uint32_t size=0;
        if(xrStringToPath(instance,hands[h],&path)==XR_SUCCESS && xrGetCurrentInteractionProfile(session,path,&state)==XR_SUCCESS && state.interactionProfile)
            xrPathToString(instance,state.interactionProfile,sizeof(name),&size,name);
        fprintf(stderr,"[vr-input] hand=%s active_profile=%s\n",hands[h],name);
    }
    XrBoundSourcesForActionEnumerateInfo info={.type=XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO,.action=buttons[GCBTN_START]};
    XrPath sources[16];uint32_t count=0;
    XrResult result=xrEnumerateBoundSourcesForAction(session,&info,16,&count,sources);
    fprintf(stderr,"[vr-input] Start bound_sources=%u result=%d\n",count,result);
    if(result==XR_SUCCESS)for(uint32_t i=0;i<count;i++) {
        char name[XR_MAX_PATH_LENGTH];uint32_t size=0;
        if(xrPathToString(instance,sources[i],sizeof(name),&size,name)==XR_SUCCESS)
            fprintf(stderr,"[vr-input] Start source=%s\n",name);
    }
}
void vr_input_sync(int focused) {
    if(!ready || !focused){vr_input_clear();return;}
    XrActiveActionSet active={.actionSet=actionSet};
    XrActionsSyncInfo sync={.type=XR_TYPE_ACTIONS_SYNC_INFO,.countActiveActionSets=1,.activeActionSets=&active};
    /* XR_SESSION_NOT_FOCUSED is a positive status, but must release input. */
    XrResult status=xrSyncActions(session,&sync);
    if(status!=lastSyncStatus){fprintf(stderr,"[vr-input] sync result=%d\n",status);lastSyncStatus=status;}
    if(status!=XR_SUCCESS){vr_input_clear();return;}
    uint32_t sample=0;
    for(int b=0;b<GC_BUTTON_COUNT;b++) {
        if(!buttons[b])continue;
        XrActionStateGetInfo get={.type=XR_TYPE_ACTION_STATE_GET_INFO,.action=buttons[b]};
        int down=0;
        if(b>=GCBTN_TRIGGER_LEFT) {
            XrActionStateFloat value={.type=XR_TYPE_ACTION_STATE_FLOAT};
            if(xrGetActionStateFloat(session,&get,&value)!=XR_SUCCESS){vr_input_clear();return;}
            down=value.isActive && value.currentState>8192.0f/32767.0f;
        } else {
            XrActionStateBoolean value={.type=XR_TYPE_ACTION_STATE_BOOLEAN};
            if(xrGetActionStateBoolean(session,&get,&value)!=XR_SUCCESS){vr_input_clear();return;}
            down=value.isActive && value.currentState;
        }
        if(down)sample|=1u<<b;
    }
    XrActionStateGetInfo get={.type=XR_TYPE_ACTION_STATE_GET_INFO,.action=stick};
    XrActionStateVector2f value={.type=XR_TYPE_ACTION_STATE_VECTOR2F};
    if(xrGetActionStateVector2f(session,&get,&value)!=XR_SUCCESS){vr_input_clear();return;}
    if(value.isActive) {
        float threshold=19660.0f/32767.0f;
        if(value.currentState.x < -threshold)sample|=STICK_LEFT;
        if(value.currentState.x > threshold)sample|=STICK_RIGHT;
        if(value.currentState.y > threshold)sample|=STICK_UP; /* XR Y is up. */
        if(value.currentState.y < -threshold)sample|=STICK_DOWN;
    }
    if(!wasFocused){blocked=sample;stickBlocked=(sample&STICK_DIRECTIONS)!=0;wasFocused=1;}
    if(!(sample&STICK_DIRECTIONS))stickBlocked=0;
    blocked&=sample;sample&=~blocked;
    if(stickBlocked)sample&=~STICK_DIRECTIONS;
    pending|=sample&~held;held=sample;
}
void vr_input_tick(void) {
    /* Stable across ReadInput + UpdatePerPlayerInput in the same native tick.
     * Start is an edge; holding it cannot skip successive screens/pause twice. */
    published=(held&~START)|pending;pending=0;
}
unsigned short vr_input_read(const short *bindings) {
    unsigned short bits=0;
    for(int b=0;b<GC_BUTTON_COUNT;b++)if(published&(1u<<b))
        bits|=b==GCBTN_START?PAD_START:(unsigned short)bindings[b];
    if(published&STICK_LEFT)bits|=PAD_LEFT;
    if(published&STICK_RIGHT)bits|=PAD_RIGHT;
    if(published&STICK_UP)bits|=PAD_UP;
    if(published&STICK_DOWN)bits|=PAD_DOWN;
    if(trace && bits!=lastReported){fprintf(stderr,"[vr-input] native_pad=0x%04x\n",bits);lastReported=bits;}
    return bits;
}
void vr_input_shutdown(void) {
    vr_input_clear();ready=0;
    if(actionSet)xrDestroyActionSet(actionSet);
    actionSet=XR_NULL_HANDLE;instance=XR_NULL_HANDLE;session=XR_NULL_HANDLE;stick=XR_NULL_HANDLE;
    memset(buttons,0,sizeof(buttons));
}
#endif
