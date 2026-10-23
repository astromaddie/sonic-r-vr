/*
 * Standalone OpenXR/GLX color probe for the Steam Frame.
 *
 * It creates the SDL2 desktop OpenGL compatibility context used by Sonic R,
 * binds that exact GLX context to XR_KHR_opengl_enable, then submits one dim
 * red and one dim blue sRGB eye image. It contains no game code or game data.
 */
#define XR_USE_PLATFORM_XLIB
#define XR_USE_GRAPHICS_API_OPENGL

#include <SDL.h>
#include <SDL_syswm.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glext.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_SRGB8_ALPHA8
#define GL_SRGB8_ALPHA8 0x8C43
#endif

typedef void (APIENTRYP GenFramebuffersFn)(GLsizei, GLuint *);
typedef void (APIENTRYP BindFramebufferFn)(GLenum, GLuint);
typedef void (APIENTRYP FramebufferTexture2DFn)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum (APIENTRYP CheckFramebufferStatusFn)(GLenum);

typedef struct EyeSwapchain {
    XrSwapchain handle;
    uint32_t width;
    uint32_t height;
    uint32_t imageCount;
    XrSwapchainImageOpenGLKHR *images;
} EyeSwapchain;

static XrInstance g_instance = XR_NULL_HANDLE;

static int xr_check(XrResult result, const char *expression, int line)
{
    if (XR_SUCCEEDED(result)) {
        return 1;
    }
    char resultText[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (g_instance != XR_NULL_HANDLE) {
        xrResultToString(g_instance, result, resultText);
    }
    fprintf(stderr, "[openxr-probe] line %d: %s failed: %s (%d)\n",
            line, expression, resultText, (int)result);
    return 0;
}

#define XR_TRY(expression) \
    do { if (!xr_check((expression), #expression, __LINE__)) goto cleanup; } while (0)

static const char *session_state_name(XrSessionState state)
{
    switch (state) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

static double monotonic_seconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0.0;
    }
    return (double)now.tv_sec + (double)now.tv_nsec / 1.0e9;
}

static int extension_available(const char *wanted)
{
    uint32_t count = 0;
    if (!xr_check(xrEnumerateInstanceExtensionProperties(NULL, 0, &count, NULL),
                  "xrEnumerateInstanceExtensionProperties(count)", __LINE__)) {
        return 0;
    }
    XrExtensionProperties *properties = calloc(count, sizeof(*properties));
    if (properties == NULL) {
        fprintf(stderr, "[openxr-probe] out of memory enumerating extensions\n");
        return 0;
    }
    for (uint32_t i = 0; i < count; ++i) {
        properties[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    XrResult result = xrEnumerateInstanceExtensionProperties(NULL, count, &count, properties);
    if (!xr_check(result, "xrEnumerateInstanceExtensionProperties(list)", __LINE__)) {
        free(properties);
        return 0;
    }
    int found = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(properties[i].extensionName, wanted) == 0) {
            found = 1;
            break;
        }
    }
    free(properties);
    return found;
}

static void print_gl_format(int64_t format)
{
    if (format == GL_SRGB8_ALPHA8) {
        printf("GL_SRGB8_ALPHA8");
    } else if (format == GL_RGBA8) {
        printf("GL_RGBA8");
    } else {
        printf("unknown");
    }
}

static void shutdown_running_session(XrSession session, int *running)
{
    if (session == XR_NULL_HANDLE || !*running) {
        return;
    }
    XrResult requestResult = xrRequestExitSession(session);
    if (XR_FAILED(requestResult)) {
        (void)xr_check(requestResult, "xrRequestExitSession(cleanup)", __LINE__);
        return;
    }
    const double deadline = monotonic_seconds() + 10.0;
    while (*running && monotonic_seconds() < deadline) {
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (xrPollEvent(g_instance, &event) == XR_SUCCESS) {
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const XrEventDataSessionStateChanged *changed =
                    (const XrEventDataSessionStateChanged *)&event;
                if (changed->state == XR_SESSION_STATE_STOPPING) {
                    if (xr_check(xrEndSession(session), "xrEndSession(cleanup)", __LINE__)) {
                        *running = 0;
                    }
                }
            }
            event.type = XR_TYPE_EVENT_DATA_BUFFER;
        }
        SDL_Delay(10);
    }
}

int main(int argc, char **argv)
{
    int exitCode = 1;
    int seconds = 10;
    int forceColorOnly = 0;
    const char *forceColorOnlyEnv = getenv("SONICR_PROBE_FORCE_COLOR_ONLY");
    if (forceColorOnlyEnv != NULL && forceColorOnlyEnv[0] != '\0') {
        if (strcmp(forceColorOnlyEnv, "1") != 0) {
            fprintf(stderr, "[openxr-probe] SONICR_PROBE_FORCE_COLOR_ONLY must be unset or 1\n");
            return 2;
        }
        forceColorOnly = 1;
    }
    if (argc > 2) {
        fprintf(stderr, "usage: %s [seconds]\n", argv[0]);
        return 2;
    }
    if (argc == 2) {
        char *end = NULL;
        errno = 0;
        long parsed = strtol(argv[1], &end, 10);
        if (errno != 0 || end == argv[1] || *end != '\0' || parsed < 1 || parsed > 300) {
            fprintf(stderr, "[openxr-probe] seconds must be an integer from 1 to 300\n");
            return 2;
        }
        seconds = (int)parsed;
    }

    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    EyeSwapchain eyes[2] = {0};
    XrViewConfigurationType *viewConfigs = NULL;
    XrEnvironmentBlendMode *blendModes = NULL;
    int64_t *formats = NULL;
    XrReferenceSpaceType *referenceSpaces = NULL;
    GLuint framebuffer = 0;
    int sessionRunning = 0;
    int frameBegun = 0;
    int exitRequested = 0;
    int quit = 0;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    uint64_t waitedFrames = 0;
    uint64_t renderedFrames = 0;
    uint64_t colorClearFrames = 0;
    uint64_t standbyFrames = 0;
    uint64_t projectionSkippedFrames = 0;
    uint64_t validOrientationFrames = 0;
    uint64_t validPositionFrames = 0;
    XrTime firstDisplayTime = 0;
    XrTime lastDisplayTime = 0;
    XrTime lastDisplayPeriod = 0;
    XrTime stopDisplayTime = 0;
    XrFrameState activeFrameState = { .type = XR_TYPE_FRAME_STATE };
    int imageAcquired[2] = {0, 0};
    int imageWaited[2] = {0, 0};
    int pixelChecked[2] = {0, 0};
    double startupDeadline = 0.0;
    SDL_Window *window = NULL;
    SDL_GLContext glContext = NULL;
    Display *xDisplay = NULL;
    GLXFBConfig fbConfig = NULL;
    XVisualInfo *xVisual = NULL;
    XrCompositionLayerProjectionView projectionViews[2] = {
        { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW },
        { .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }
    };
    XrView locatedViews[2] = {
        { .type = XR_TYPE_VIEW }, { .type = XR_TYPE_VIEW }
    };
    GenFramebuffersFn genFramebuffers = NULL;
    BindFramebufferFn bindFramebuffer = NULL;
    FramebufferTexture2DFn framebufferTexture2D = NULL;
    CheckFramebufferStatusFn checkFramebufferStatus = NULL;
    int sdlInitialized = 0;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[sdl] SDL_Init failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    sdlInitialized = 1;
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    window = SDL_CreateWindow("Sonic R OpenXR GLX probe",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              320, 240, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window == NULL) {
        fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    glContext = SDL_GL_CreateContext(window);
    if (glContext == NULL) {
        fprintf(stderr, "[sdl] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        goto cleanup;
    }
    if (SDL_GL_MakeCurrent(window, glContext) != 0) {
        fprintf(stderr, "[sdl] SDL_GL_MakeCurrent failed: %s\n", SDL_GetError());
        goto cleanup;
    }

    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo) || wmInfo.subsystem != SDL_SYSWM_X11) {
        fprintf(stderr, "[sdl] expected SDL X11 window for OpenXR GLX binding (driver=%s)\n",
                SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "unknown");
        goto cleanup;
    }
    xDisplay = wmInfo.info.x11.display;
    GLXContext currentContext = glXGetCurrentContext();
    GLXDrawable currentDrawable = glXGetCurrentDrawable();
    if (xDisplay == NULL || currentContext == NULL || currentDrawable == 0) {
        fprintf(stderr, "[glx] SDL compatibility context is not a current GLX context\n");
        goto cleanup;
    }
    XWindowAttributes xAttributes;
    if (!XGetWindowAttributes(xDisplay, wmInfo.info.x11.window, &xAttributes) ||
        xAttributes.visual == NULL) {
        fprintf(stderr, "[glx] cannot query SDL window visual\n");
        goto cleanup;
    }
    const VisualID visualId = XVisualIDFromVisual(xAttributes.visual);
    int fbConfigId = 0;
    if (glXQueryContext(xDisplay, currentContext, GLX_FBCONFIG_ID, &fbConfigId) != Success) {
        fprintf(stderr, "[glx] cannot query current context FBConfig\n");
        goto cleanup;
    }
    const int fbConfigAttributes[] = { GLX_FBCONFIG_ID, fbConfigId, None };
    int fbConfigCount = 0;
    GLXFBConfig *fbConfigs = glXChooseFBConfig(xDisplay, DefaultScreen(xDisplay),
                                                fbConfigAttributes, &fbConfigCount);
    if (fbConfigs == NULL || fbConfigCount != 1) {
        fprintf(stderr, "[glx] cannot resolve current context FBConfig id=%d\n", fbConfigId);
        if (fbConfigs != NULL) XFree(fbConfigs);
        goto cleanup;
    }
    fbConfig = fbConfigs[0];
    XFree(fbConfigs);
    int configVisualId = 0;
    if (glXGetFBConfigAttrib(xDisplay, fbConfig, GLX_VISUAL_ID, &configVisualId) != Success ||
        (VisualID)configVisualId != visualId) {
        fprintf(stderr, "[glx] context FBConfig visual %#x does not match SDL window visual %#lx\n",
                configVisualId, (unsigned long)visualId);
        goto cleanup;
    }
    xVisual = glXGetVisualFromFBConfig(xDisplay, fbConfig);
    if (xVisual == NULL) {
        fprintf(stderr, "[glx] cannot resolve current FBConfig visual\n");
        goto cleanup;
    }
    const char *glVersion = (const char *)glGetString(GL_VERSION);
    const char *glVendor = (const char *)glGetString(GL_VENDOR);
    const char *glRenderer = (const char *)glGetString(GL_RENDERER);
    if (glVersion == NULL || glVendor == NULL || glRenderer == NULL) {
        fprintf(stderr, "[gl] context returned incomplete GL identification\n");
        goto cleanup;
    }
    printf("[sdl] driver=%s subsystem=x11 context=compatibility hidden=1\n",
           SDL_GetCurrentVideoDriver());
    printf("[gl] vendor=%s renderer=%s version=%s\n", glVendor, glRenderer, glVersion);
    printf("[glx] display=%p context=%p drawable=0x%lx fbconfig=%d visual=0x%lx\n",
           (void *)xDisplay, (void *)currentContext, (unsigned long)currentDrawable,
           fbConfigId, (unsigned long)visualId);

    if (!extension_available(XR_KHR_OPENGL_ENABLE_EXTENSION_NAME)) {
        fprintf(stderr, "[openxr-probe] runtime does not advertise %s\n",
                XR_KHR_OPENGL_ENABLE_EXTENSION_NAME);
        goto cleanup;
    }
    const char *instanceExtensions[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    XrInstanceCreateInfo instanceInfo = { .type = XR_TYPE_INSTANCE_CREATE_INFO };
    strncpy(instanceInfo.applicationInfo.applicationName, "Sonic R GLX colour probe",
            XR_MAX_APPLICATION_NAME_SIZE - 1);
    instanceInfo.applicationInfo.applicationVersion = 1;
    strncpy(instanceInfo.applicationInfo.engineName, "Sonic R native port",
            XR_MAX_ENGINE_NAME_SIZE - 1);
    instanceInfo.applicationInfo.engineVersion = 1;
    instanceInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    instanceInfo.enabledExtensionCount = 1;
    instanceInfo.enabledExtensionNames = instanceExtensions;
    XR_TRY(xrCreateInstance(&instanceInfo, &g_instance));

    XrInstanceProperties instanceProperties = { .type = XR_TYPE_INSTANCE_PROPERTIES };
    XR_TRY(xrGetInstanceProperties(g_instance, &instanceProperties));
    printf("[openxr] runtime=%s version=%u.%u.%u\n", instanceProperties.runtimeName,
           XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
           XR_VERSION_MINOR(instanceProperties.runtimeVersion),
           XR_VERSION_PATCH(instanceProperties.runtimeVersion));
    XrSystemGetInfo systemInfo = { .type = XR_TYPE_SYSTEM_GET_INFO };
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XR_TRY(xrGetSystem(g_instance, &systemInfo, &systemId));
    XrSystemProperties systemProperties = { .type = XR_TYPE_SYSTEM_PROPERTIES };
    XR_TRY(xrGetSystemProperties(g_instance, systemId, &systemProperties));
    printf("[system] name=%s vendor=%u maxSwapchain=%ux%u maxLayers=%u orientation=%u position=%u\n",
           systemProperties.systemName, systemProperties.vendorId,
           systemProperties.graphicsProperties.maxSwapchainImageWidth,
           systemProperties.graphicsProperties.maxSwapchainImageHeight,
           systemProperties.graphicsProperties.maxLayerCount,
           systemProperties.trackingProperties.orientationTracking,
           systemProperties.trackingProperties.positionTracking);

    uint32_t viewConfigCount = 0;
    XR_TRY(xrEnumerateViewConfigurations(g_instance, systemId, 0, &viewConfigCount, NULL));
    viewConfigs = calloc(viewConfigCount, sizeof(*viewConfigs));
    if (viewConfigs == NULL) {
        fprintf(stderr, "[openxr-probe] out of memory enumerating view configurations\n");
        goto cleanup;
    }
    XR_TRY(xrEnumerateViewConfigurations(g_instance, systemId, viewConfigCount,
                                         &viewConfigCount, viewConfigs));
    int hasStereo = 0;
    printf("[views] configurations=");
    for (uint32_t i = 0; i < viewConfigCount; ++i) {
        printf("%s%d", i == 0 ? "" : ",", (int)viewConfigs[i]);
        if (viewConfigs[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) hasStereo = 1;
    }
    printf(" (2=PRIMARY_STEREO)\n");
    free(viewConfigs);
    viewConfigs = NULL;
    if (!hasStereo) {
        fprintf(stderr, "[openxr-probe] runtime has no primary stereo view configuration\n");
        goto cleanup;
    }

    uint32_t configuredViews = 0;
    XrViewConfigurationView recommendedViews[2] = {
        { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW },
        { .type = XR_TYPE_VIEW_CONFIGURATION_VIEW }
    };
    XR_TRY(xrEnumerateViewConfigurationViews(g_instance, systemId,
                                              XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                              2, &configuredViews, recommendedViews));
    if (configuredViews != 2) {
        fprintf(stderr, "[openxr-probe] expected two stereo views, runtime reported %u\n", configuredViews);
        goto cleanup;
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        printf("[view%u] recommended=%ux%u maximum=%ux%u samples=%u\n", eye,
               recommendedViews[eye].recommendedImageRectWidth,
               recommendedViews[eye].recommendedImageRectHeight,
               recommendedViews[eye].maxImageRectWidth,
               recommendedViews[eye].maxImageRectHeight,
               recommendedViews[eye].recommendedSwapchainSampleCount);
    }

    uint32_t blendCount = 0;
    XR_TRY(xrEnumerateEnvironmentBlendModes(g_instance, systemId,
                                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                            0, &blendCount, NULL));
    blendModes = calloc(blendCount, sizeof(*blendModes));
    if (blendModes == NULL) {
        fprintf(stderr, "[openxr-probe] out of memory enumerating blend modes\n");
        goto cleanup;
    }
    XR_TRY(xrEnumerateEnvironmentBlendModes(g_instance, systemId,
                                            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                            blendCount, &blendCount, blendModes));
    int hasOpaque = 0;
    printf("[blend] modes=");
    for (uint32_t i = 0; i < blendCount; ++i) {
        printf("%s%d", i == 0 ? "" : ",", (int)blendModes[i]);
        if (blendModes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) hasOpaque = 1;
    }
    printf(" (1=OPAQUE)\n");
    free(blendModes);
    blendModes = NULL;
    if (!hasOpaque) {
        fprintf(stderr, "[openxr-probe] runtime has no opaque environment blend mode\n");
        goto cleanup;
    }

    PFN_xrGetOpenGLGraphicsRequirementsKHR getGraphicsRequirements = NULL;
    XR_TRY(xrGetInstanceProcAddr(g_instance, "xrGetOpenGLGraphicsRequirementsKHR",
                                 (PFN_xrVoidFunction *)&getGraphicsRequirements));
    XrGraphicsRequirementsOpenGLKHR graphicsRequirements = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR
    };
    XR_TRY(getGraphicsRequirements(g_instance, systemId, &graphicsRequirements));
    int glMajor = 0;
    int glMinor = 0;
    if (sscanf(glVersion, "%d.%d", &glMajor, &glMinor) != 2) {
        fprintf(stderr, "[gl] cannot parse OpenGL version string: %s\n", glVersion);
        goto cleanup;
    }
    const XrVersion actualGlVersion = XR_MAKE_VERSION((uint16_t)glMajor, (uint16_t)glMinor, 0);
    printf("[gl-requirements] runtime=%u.%u..%u.%u current=%d.%d\n",
           XR_VERSION_MAJOR(graphicsRequirements.minApiVersionSupported),
           XR_VERSION_MINOR(graphicsRequirements.minApiVersionSupported),
           XR_VERSION_MAJOR(graphicsRequirements.maxApiVersionSupported),
           XR_VERSION_MINOR(graphicsRequirements.maxApiVersionSupported), glMajor, glMinor);
    if (actualGlVersion < graphicsRequirements.minApiVersionSupported ||
        actualGlVersion > graphicsRequirements.maxApiVersionSupported) {
        fprintf(stderr, "[gl] current compatibility context is outside runtime requirements\n");
        goto cleanup;
    }

    XrGraphicsBindingOpenGLXlibKHR graphicsBinding = {
        .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR
    };
    graphicsBinding.xDisplay = xDisplay;
    graphicsBinding.visualid = visualId;
    graphicsBinding.glxFBConfig = fbConfig;
    graphicsBinding.glxDrawable = currentDrawable;
    graphicsBinding.glxContext = currentContext;
    XrSessionCreateInfo sessionInfo = { .type = XR_TYPE_SESSION_CREATE_INFO };
    sessionInfo.next = &graphicsBinding;
    sessionInfo.systemId = systemId;
    XR_TRY(xrCreateSession(g_instance, &sessionInfo, &session));
    printf("[session] graphicsBinding=XR_KHR_opengl_enable/Xlib+GLX created\n");

    uint32_t formatCount = 0;
    XR_TRY(xrEnumerateSwapchainFormats(session, 0, &formatCount, NULL));
    formats = calloc(formatCount, sizeof(*formats));
    if (formats == NULL) {
        fprintf(stderr, "[openxr-probe] out of memory enumerating swapchain formats\n");
        goto cleanup;
    }
    XR_TRY(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats));
    int hasSrgb = 0;
    printf("[formats] available=");
    for (uint32_t i = 0; i < formatCount; ++i) {
        printf("%s0x%" PRIx64 "(", i == 0 ? "" : ",", (uint64_t)formats[i]);
        print_gl_format(formats[i]);
        printf(")");
        if (formats[i] == GL_SRGB8_ALPHA8) hasSrgb = 1;
    }
    printf("\n");
    free(formats);
    formats = NULL;
    if (!hasSrgb) {
        fprintf(stderr, "[openxr-probe] runtime does not offer required GL_SRGB8_ALPHA8 swapchains\n");
        goto cleanup;
    }

    uint32_t referenceSpaceCount = 0;
    XR_TRY(xrEnumerateReferenceSpaces(session, 0, &referenceSpaceCount, NULL));
    referenceSpaces = calloc(referenceSpaceCount, sizeof(*referenceSpaces));
    if (referenceSpaces == NULL) {
        fprintf(stderr, "[openxr-probe] out of memory enumerating reference spaces\n");
        goto cleanup;
    }
    XR_TRY(xrEnumerateReferenceSpaces(session, referenceSpaceCount,
                                      &referenceSpaceCount, referenceSpaces));
    int hasLocal = 0;
    printf("[spaces] available=");
    for (uint32_t i = 0; i < referenceSpaceCount; ++i) {
        printf("%s%d", i == 0 ? "" : ",", (int)referenceSpaces[i]);
        if (referenceSpaces[i] == XR_REFERENCE_SPACE_TYPE_LOCAL) hasLocal = 1;
    }
    printf(" (2=LOCAL)\n");
    free(referenceSpaces);
    referenceSpaces = NULL;
    if (!hasLocal) {
        fprintf(stderr, "[openxr-probe] runtime does not offer LOCAL reference space\n");
        goto cleanup;
    }
    XrReferenceSpaceCreateInfo spaceInfo = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XR_TRY(xrCreateReferenceSpace(session, &spaceInfo, &localSpace));

    for (uint32_t eye = 0; eye < 2; ++eye) {
        EyeSwapchain *swapchain = &eyes[eye];
        swapchain->width = recommendedViews[eye].recommendedImageRectWidth;
        swapchain->height = recommendedViews[eye].recommendedImageRectHeight;
        XrSwapchainCreateInfo swapchainInfo = { .type = XR_TYPE_SWAPCHAIN_CREATE_INFO };
        swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        swapchainInfo.format = GL_SRGB8_ALPHA8;
        swapchainInfo.sampleCount = 1;
        swapchainInfo.width = swapchain->width;
        swapchainInfo.height = swapchain->height;
        swapchainInfo.faceCount = 1;
        swapchainInfo.arraySize = 1;
        swapchainInfo.mipCount = 1;
        XR_TRY(xrCreateSwapchain(session, &swapchainInfo, &swapchain->handle));
        XR_TRY(xrEnumerateSwapchainImages(swapchain->handle, 0,
                                          &swapchain->imageCount, NULL));
        swapchain->images = calloc(swapchain->imageCount, sizeof(*swapchain->images));
        if (swapchain->images == NULL) {
            fprintf(stderr, "[openxr-probe] out of memory enumerating eye%u images\n", eye);
            goto cleanup;
        }
        for (uint32_t image = 0; image < swapchain->imageCount; ++image) {
            swapchain->images[image].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
        }
        XR_TRY(xrEnumerateSwapchainImages(swapchain->handle, swapchain->imageCount,
                                          &swapchain->imageCount,
                                          (XrSwapchainImageBaseHeader *)swapchain->images));
        printf("[swapchain] eye%u format=GL_SRGB8_ALPHA8 size=%ux%u images=%u\n",
               eye, swapchain->width, swapchain->height, swapchain->imageCount);
    }

    genFramebuffers = (GenFramebuffersFn)SDL_GL_GetProcAddress("glGenFramebuffers");
    bindFramebuffer = (BindFramebufferFn)SDL_GL_GetProcAddress("glBindFramebuffer");
    framebufferTexture2D = (FramebufferTexture2DFn)SDL_GL_GetProcAddress("glFramebufferTexture2D");
    checkFramebufferStatus = (CheckFramebufferStatusFn)SDL_GL_GetProcAddress("glCheckFramebufferStatus");
    if (genFramebuffers == NULL || bindFramebuffer == NULL || framebufferTexture2D == NULL ||
        checkFramebufferStatus == NULL) {
        fprintf(stderr, "[gl] required framebuffer entry points are unavailable\n");
        goto cleanup;
    }
    genFramebuffers(1, &framebuffer);
    if (framebuffer == 0) {
        fprintf(stderr, "[gl] glGenFramebuffers returned zero\n");
        goto cleanup;
    }

    XrSessionBeginInfo beginInfo = { .type = XR_TYPE_SESSION_BEGIN_INFO };
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    const double programStart = monotonic_seconds();
    startupDeadline = programStart + 30.0;
    printf("[probe] running up to %d seconds of XR frame timing; mode=%s; close window to stop\n",
           seconds, forceColorOnly ? "forced color-only diagnostic" : "normal tracked projection");
    if (forceColorOnly) {
        printf("[pose] color-only mode uses no submitted tracking pose and submits no projection layer\n");
    }
    fflush(stdout);

    while (!quit) {
        XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
        while (1) {
            XrResult pollResult = xrPollEvent(g_instance, &event);
            if (pollResult == XR_EVENT_UNAVAILABLE) break;
            if (!xr_check(pollResult, "xrPollEvent", __LINE__)) goto cleanup;
            if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const XrEventDataSessionStateChanged *changed =
                    (const XrEventDataSessionStateChanged *)&event;
                sessionState = changed->state;
                printf("[state] %s\n", session_state_name(sessionState));
                if (sessionState == XR_SESSION_STATE_READY && !sessionRunning) {
                    XR_TRY(xrBeginSession(session, &beginInfo));
                    sessionRunning = 1;
                } else if (sessionState == XR_SESSION_STATE_STOPPING && sessionRunning) {
                    XR_TRY(xrEndSession(session));
                    sessionRunning = 0;
                    if (exitRequested) quit = 1;
                } else if (sessionState == XR_SESSION_STATE_EXITING ||
                           sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                    quit = 1;
                }
            } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                printf("[state] instance loss pending\n");
                quit = 1;
            }
            event.type = XR_TYPE_EVENT_DATA_BUFFER;
        }
        if (quit) break;

        SDL_Event sdlEvent;
        while (SDL_PollEvent(&sdlEvent)) {
            if (sdlEvent.type == SDL_QUIT ||
                (sdlEvent.type == SDL_KEYDOWN && sdlEvent.key.keysym.sym == SDLK_ESCAPE)) {
                if (!exitRequested && sessionRunning) {
                    XR_TRY(xrRequestExitSession(session));
                    exitRequested = 1;
                } else if (!sessionRunning) {
                    quit = 1;
                }
            }
        }
        if (quit) break;
        if (!sessionRunning) {
            if (startupDeadline > 0.0 && monotonic_seconds() > startupDeadline) {
                fprintf(stderr, "[openxr-probe] session did not become READY within 30 seconds\n");
                goto cleanup;
            }
            SDL_Delay(10);
            continue;
        }

        XrFrameWaitInfo waitInfo = { .type = XR_TYPE_FRAME_WAIT_INFO };
        activeFrameState = (XrFrameState){ .type = XR_TYPE_FRAME_STATE };
        XR_TRY(xrWaitFrame(session, &waitInfo, &activeFrameState));
        ++waitedFrames;
        if (firstDisplayTime == 0) {
            firstDisplayTime = activeFrameState.predictedDisplayTime;
            stopDisplayTime = firstDisplayTime + (XrTime)seconds * 1000000000LL;
        }
        lastDisplayTime = activeFrameState.predictedDisplayTime;
        lastDisplayPeriod = activeFrameState.predictedDisplayPeriod;
        XrFrameBeginInfo frameBeginInfo = { .type = XR_TYPE_FRAME_BEGIN_INFO };
        XR_TRY(xrBeginFrame(session, &frameBeginInfo));
        frameBegun = 1;

        XrCompositionLayerProjection layer = {
            .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION
        };
        const XrCompositionLayerBaseHeader *layers[1] = { NULL };
        uint32_t layerCount = 0;
        int orientationValid = 0;
        int positionValid = 0;
        int submitProjection = 0;
        XrFlags64 locatedViewFlags = 0;
        const int locateViewsThisFrame = activeFrameState.shouldRender || forceColorOnly;
        if (locateViewsThisFrame) {
            XrViewLocateInfo locateInfo = { .type = XR_TYPE_VIEW_LOCATE_INFO };
            locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            locateInfo.displayTime = activeFrameState.predictedDisplayTime;
            locateInfo.space = localSpace;
            XrViewState viewState = { .type = XR_TYPE_VIEW_STATE };
            uint32_t locatedCount = 0;
            XR_TRY(xrLocateViews(session, &locateInfo, &viewState, 2, &locatedCount, locatedViews));
            if (locatedCount != 2) {
                fprintf(stderr, "[openxr-probe] xrLocateViews returned %u views\n", locatedCount);
                goto cleanup;
            }
            locatedViewFlags = viewState.viewStateFlags;
            orientationValid = (locatedViewFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
            positionValid = (locatedViewFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
            if (orientationValid)
                ++validOrientationFrames;
            if (positionValid)
                ++validPositionFrames;

            submitProjection = activeFrameState.shouldRender && orientationValid && positionValid &&
                               !forceColorOnly;
            if (activeFrameState.shouldRender && !orientationValid) {
                ++projectionSkippedFrames;
                if (projectionSkippedFrames == 1) {
                    printf("[pose] projection suppressed: orientation is invalid\n");
                }
            } else if (activeFrameState.shouldRender && !positionValid) {
                ++projectionSkippedFrames;
                if (projectionSkippedFrames == 1) {
                    printf("[pose] projection suppressed: position is invalid\n");
                }
            }
            if (forceColorOnly && !orientationValid && projectionSkippedFrames == 0) {
                printf("[pose] color-only diagnostic: orientation invalid; no projection pose will be submitted\n");
            }
            if (forceColorOnly && !positionValid && projectionSkippedFrames == 0) {
                printf("[pose] color-only diagnostic: position invalid; no projection pose will be submitted\n");
            }
        } else if (!activeFrameState.shouldRender) {
            ++standbyFrames;
        }

        const int clearColorOnly = forceColorOnly;
        const int renderSwapchains = submitProjection || clearColorOnly;
        if (renderSwapchains) {

            for (uint32_t eye = 0; eye < 2; ++eye) {
                EyeSwapchain *swapchain = &eyes[eye];
                uint32_t imageIndex = 0;
                XrSwapchainImageAcquireInfo acquireInfo = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO
                };
                XR_TRY(xrAcquireSwapchainImage(swapchain->handle, &acquireInfo, &imageIndex));
                imageAcquired[eye] = 1;
                XrSwapchainImageWaitInfo imageWaitInfo = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO
                };
                imageWaitInfo.timeout = XR_INFINITE_DURATION;
                XR_TRY(xrWaitSwapchainImage(swapchain->handle, &imageWaitInfo));
                imageWaited[eye] = 1;

                bindFramebuffer(GL_FRAMEBUFFER, framebuffer);
                framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                     swapchain->images[imageIndex].image, 0);
                glDrawBuffer(GL_COLOR_ATTACHMENT0);
                glReadBuffer(GL_COLOR_ATTACHMENT0);
                if (checkFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    fprintf(stderr, "[gl] eye%u framebuffer is incomplete\n", eye);
                    goto cleanup;
                }
                glViewport(0, 0, (GLsizei)swapchain->width, (GLsizei)swapchain->height);
                glDisable(GL_FRAMEBUFFER_SRGB);
                unsigned char expectedPixel[4];
                if (eye == 0) {
                    glClearColor(0.18f, 0.035f, 0.035f, 1.0f);
                    expectedPixel[0] = (unsigned char)(0.18f * 255.0f + 0.5f);
                    expectedPixel[1] = (unsigned char)(0.035f * 255.0f + 0.5f);
                    expectedPixel[2] = (unsigned char)(0.035f * 255.0f + 0.5f);
                } else {
                    glClearColor(0.035f, 0.055f, 0.18f, 1.0f);
                    expectedPixel[0] = (unsigned char)(0.035f * 255.0f + 0.5f);
                    expectedPixel[1] = (unsigned char)(0.055f * 255.0f + 0.5f);
                    expectedPixel[2] = (unsigned char)(0.18f * 255.0f + 0.5f);
                }
                expectedPixel[3] = 255;
                glClear(GL_COLOR_BUFFER_BIT);
                GLenum glError = glGetError();
                if (glError != GL_NO_ERROR) {
                    fprintf(stderr, "[gl] eye%u clear failed with 0x%x\n", eye, glError);
                    goto cleanup;
                }
                if (forceColorOnly && !pixelChecked[eye]) {
                    unsigned char pixel[4] = {0, 0, 0, 0};
                    glReadPixels((GLint)(swapchain->width / 2), (GLint)(swapchain->height / 2),
                                 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
                    glError = glGetError();
                    if (glError != GL_NO_ERROR) {
                        fprintf(stderr, "[gl] eye%u center readback failed with 0x%x\n", eye, glError);
                        bindFramebuffer(GL_FRAMEBUFFER, 0);
                        goto cleanup;
                    }
                    const int pixelMatches =
                        abs((int)pixel[0] - (int)expectedPixel[0]) <= 3 &&
                        abs((int)pixel[1] - (int)expectedPixel[1]) <= 3 &&
                        abs((int)pixel[2] - (int)expectedPixel[2]) <= 3 &&
                        abs((int)pixel[3] - (int)expectedPixel[3]) <= 3;
                    printf("[clear-check] eye%u center=%u,%u,%u,%u expected=%u,%u,%u,%u %s\n",
                           eye, pixel[0], pixel[1], pixel[2], pixel[3], expectedPixel[0],
                           expectedPixel[1], expectedPixel[2], expectedPixel[3],
                           pixelMatches ? "PASS" : "FAIL");
                    pixelChecked[eye] = pixelMatches ? 1 : -1;
                    fflush(stdout);
                    if (!pixelMatches) {
                        bindFramebuffer(GL_FRAMEBUFFER, 0);
                        goto cleanup;
                    }
                }
                bindFramebuffer(GL_FRAMEBUFFER, 0);
                /* Zink's OpenXR bridge does not wait for pending GL work here. */
                glFlush();
                glFinish();
                XrSwapchainImageReleaseInfo releaseInfo = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO
                };
                XR_TRY(xrReleaseSwapchainImage(swapchain->handle, &releaseInfo));
                imageAcquired[eye] = 0;
                imageWaited[eye] = 0;

                if (submitProjection) {
                    projectionViews[eye] = (XrCompositionLayerProjectionView){
                        .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW
                    };
                    projectionViews[eye].pose = locatedViews[eye].pose;
                    projectionViews[eye].fov = locatedViews[eye].fov;
                    projectionViews[eye].subImage.swapchain = swapchain->handle;
                    projectionViews[eye].subImage.imageRect.offset.x = 0;
                    projectionViews[eye].subImage.imageRect.offset.y = 0;
                    projectionViews[eye].subImage.imageRect.extent.width = (int32_t)swapchain->width;
                    projectionViews[eye].subImage.imageRect.extent.height = (int32_t)swapchain->height;
                }
            }
            if (submitProjection) {
                layer.space = localSpace;
                layer.viewCount = 2;
                layer.views = projectionViews;
                layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
                layerCount = 1;
                ++renderedFrames;
            } else {
                ++colorClearFrames;
            }
            if (renderedFrames == 1) {
                printf("[pose] left=(%.3f,%.3f,%.3f) right=(%.3f,%.3f,%.3f) orientationValid=%u positionValid=%u\n",
                       locatedViews[0].pose.position.x, locatedViews[0].pose.position.y,
                       locatedViews[0].pose.position.z, locatedViews[1].pose.position.x,
                       locatedViews[1].pose.position.y, locatedViews[1].pose.position.z,
                       (locatedViewFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0,
                       (locatedViewFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0);
            }
        }

        XrFrameEndInfo frameEndInfo = { .type = XR_TYPE_FRAME_END_INFO };
        frameEndInfo.displayTime = activeFrameState.predictedDisplayTime;
        frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        frameEndInfo.layerCount = layerCount;
        frameEndInfo.layers = layerCount ? layers : NULL;
        XR_TRY(xrEndFrame(session, &frameEndInfo));
        frameBegun = 0;

        if (!exitRequested && activeFrameState.predictedDisplayTime >= stopDisplayTime) {
            XR_TRY(xrRequestExitSession(session));
            exitRequested = 1;
            printf("[probe] requested orderly session exit at predictedDisplayTime\n");
        }
    }

    if (waitedFrames > 0) {
        const double displaySpan = (double)(lastDisplayTime - firstDisplayTime) / 1.0e9;
        const double rate = displaySpan > 0.0 && waitedFrames > 1
            ? (double)(waitedFrames - 1) / displaySpan : 0.0;
        printf("[frames] waited=%" PRIu64 " projection_submitted=%" PRIu64 " color_clear=%" PRIu64
               " standby=%" PRIu64 " pose_suppressed=%" PRIu64
               " orientation_valid=%" PRIu64 " position_valid=%" PRIu64
               " period=%.3fms predicted_rate=%.2fHz\n",
               waitedFrames, renderedFrames, colorClearFrames, standbyFrames,
               projectionSkippedFrames, validOrientationFrames, validPositionFrames,
               (double)lastDisplayPeriod / 1.0e6, rate);
    }
    if (renderedFrames > 0) {
        exitCode = 0;
        printf("[result] PROJECTION SUBMISSION VERIFIED; valid located eye poses submitted\n");
    } else if (forceColorOnly && colorClearFrames > 0 && pixelChecked[0] == 1 && pixelChecked[1] == 1) {
        exitCode = 0;
        printf("[result] COLOR CLEARS VERIFIED; submitted_layers=0; tracking_validity=not_claimed\n");
    } else {
        exitCode = 2;
        printf("[openxr-probe] NO STEREO FRAME SUBMITTED\n");
    }

cleanup:
    if (frameBegun && session != XR_NULL_HANDLE) {
        for (uint32_t eye = 0; eye < 2; ++eye) {
            if (!imageAcquired[eye]) continue;
            EyeSwapchain *swapchain = &eyes[eye];
            if (!imageWaited[eye]) {
                XrSwapchainImageWaitInfo imageWaitInfo = {
                    .type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO,
                    .timeout = XR_INFINITE_DURATION
                };
                XrResult waitResult = xrWaitSwapchainImage(swapchain->handle, &imageWaitInfo);
                if (XR_FAILED(waitResult)) {
                    (void)xr_check(waitResult, "xrWaitSwapchainImage(cleanup)", __LINE__);
                    continue;
                }
                imageWaited[eye] = 1;
            }
            glFlush();
            glFinish();
            XrSwapchainImageReleaseInfo releaseInfo = {
                .type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO
            };
            XrResult releaseResult = xrReleaseSwapchainImage(swapchain->handle, &releaseInfo);
            if (XR_FAILED(releaseResult)) {
                (void)xr_check(releaseResult, "xrReleaseSwapchainImage(cleanup)", __LINE__);
            } else {
                imageAcquired[eye] = 0;
                imageWaited[eye] = 0;
            }
        }
        XrFrameEndInfo frameEndInfo = { .type = XR_TYPE_FRAME_END_INFO };
        frameEndInfo.displayTime = activeFrameState.predictedDisplayTime;
        frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        XrResult endResult = xrEndFrame(session, &frameEndInfo);
        if (XR_FAILED(endResult)) {
            (void)xr_check(endResult, "xrEndFrame(cleanup)", __LINE__);
        } else {
            frameBegun = 0;
        }
    }
    if (session != XR_NULL_HANDLE && sessionRunning) {
        shutdown_running_session(session, &sessionRunning);
    }
    if (framebuffer != 0 && glContext != NULL) {
        BindFramebufferFn cleanupBindFramebuffer = (BindFramebufferFn)SDL_GL_GetProcAddress("glBindFramebuffer");
        typedef void (APIENTRYP DeleteFramebuffersFn)(GLsizei, const GLuint *);
        DeleteFramebuffersFn deleteFramebuffers =
            (DeleteFramebuffersFn)SDL_GL_GetProcAddress("glDeleteFramebuffers");
        if (cleanupBindFramebuffer != NULL && deleteFramebuffers != NULL) {
            cleanupBindFramebuffer(GL_FRAMEBUFFER, 0);
            deleteFramebuffers(1, &framebuffer);
        }
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        free(eyes[eye].images);
        if (eyes[eye].handle != XR_NULL_HANDLE) {
            XrResult result = xrDestroySwapchain(eyes[eye].handle);
            if (XR_FAILED(result)) (void)xr_check(result, "xrDestroySwapchain", __LINE__);
        }
    }
    if (localSpace != XR_NULL_HANDLE) {
        XrResult result = xrDestroySpace(localSpace);
        if (XR_FAILED(result)) (void)xr_check(result, "xrDestroySpace", __LINE__);
    }
    if (session != XR_NULL_HANDLE) {
        XrResult result = xrDestroySession(session);
        if (XR_FAILED(result)) (void)xr_check(result, "xrDestroySession", __LINE__);
    }
    if (g_instance != XR_NULL_HANDLE) {
        XrResult result = xrDestroyInstance(g_instance);
        if (XR_FAILED(result)) (void)xr_check(result, "xrDestroyInstance", __LINE__);
    }
    free(referenceSpaces);
    free(formats);
    free(blendModes);
    free(viewConfigs);
    if (xVisual != NULL) XFree(xVisual);
    if (glContext != NULL) SDL_GL_DeleteContext(glContext);
    if (window != NULL) SDL_DestroyWindow(window);
    if (sdlInitialized) SDL_Quit();
    return exitCode;
}
