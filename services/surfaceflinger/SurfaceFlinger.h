/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_SURFACE_FLINGER_H
#define ANDROID_SURFACE_FLINGER_H

#include <stdint.h>
#include <sys/types.h>

#include <EGL/egl.h>

/*
 * NOTE: Make sure this file doesn't include  anything from <gl/ > or <gl2/ >
 */

#include <cutils/compiler.h>

#include <utils/Atomic.h>
#include <utils/Errors.h>
#include <utils/KeyedVector.h>
#include <utils/RefBase.h>
#include <utils/SortedVector.h>
#include <utils/threads.h>

#include <binder/IMemory.h>

#include <ui/PixelFormat.h>
#include <ui/mat4.h>

#include <gui/ISurfaceComposer.h>
#include <gui/ISurfaceComposerClient.h>

#include <hardware/hwcomposer_defs.h>

#include <private/gui/LayerState.h>

#include "Barrier.h"
#include "DisplayDevice.h"
#include "DispSync.h"
#include "FrameTracker.h"
#include "MessageQueue.h"

#include "DisplayHardware/HWComposer.h"
#include "Effects/Daltonizer.h"

namespace android {

// ---------------------------------------------------------------------------

class Client;
class DisplayEventConnection;
class EventThread;
class IGraphicBufferAlloc;
class Layer;
class LayerDim;
class Surface;
class RenderEngine;
class EventControlThread;

// ---------------------------------------------------------------------------

enum {
    eTransactionNeeded        = 0x01,
    eTraversalNeeded          = 0x02,
    eDisplayTransactionNeeded = 0x04,
    eOtherNeeded                = 0x08,
    eWant3DNeeded            = 0x10,
    eTransactionMask           = 0x1f
};

static  int STEREOSCOPIC_3D_FORMAT_OFF=0;
static  int REQUEST_3D_FORMAT_SIDE_BY_SIDE=8;
static  int REQUEST_3D_FORMAT_TOP_BOTTOM=16;

class SurfaceFlinger : public BnSurfaceComposer,
                       private IBinder::DeathRecipient,
                       private HWComposer::EventHandler
{
public:
    static char const* getServiceName() ANDROID_API {
        return "SurfaceFlinger";
    }

    SurfaceFlinger() ANDROID_API;

    // must be called before clients can connect
    void init() ANDROID_API;

    // starts SurfaceFlinger main loop in the current thread
    void run() ANDROID_API;

    enum {
        EVENT_VSYNC = HWC_EVENT_VSYNC
    };

    enum{
        REQUEST_DISPLAY_FORMAT_1080P=0,
        REQUEST_DISPLAY_FORMAT_1080I,
        REQUEST_DISPLAY_FORMAT_720P,
        REQUEST_DISPLAY_FORMAT_720I,
        REQUEST_DISPLAY_FORMAT_576P,
        REQUEST_DISPLAY_FORMAT_576I,
        REQUEST_DISPLAY_FORMAT_480P,
        REQUEST_DISPLAY_FORMAT_480I,
        REQUEST_DISPLAY_FORMAT_MAX
    };

    enum{
        DISPLAY_X=0,
        DISPLAY_Y,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT
    };

    // post an asynchronous message to the main thread
    status_t postMessageAsync(const sp<MessageBase>& msg, nsecs_t reltime = 0, uint32_t flags = 0);

    // post a synchronous message to the main thread
    status_t postMessageSync(const sp<MessageBase>& msg, nsecs_t reltime = 0, uint32_t flags = 0);

    // force full composition on all displays
    void repaintEverything();

    // returns the default Display
    sp<const DisplayDevice> getDefaultDisplayDevice() const {
        return getDisplayDevice(mBuiltinDisplays[DisplayDevice::DISPLAY_PRIMARY]);
    }

    // utility function to delete a texture on the main thread
    void deleteTextureAsync(uint32_t texture);

    // enable/disable h/w composer event
    // TODO: this should be made accessible only to EventThread
    void eventControl(int disp, int event, int enabled);

    // called on the main thread by MessageQueue when an internal message
    // is received
    // TODO: this should be made accessible only to MessageQueue
    void onMessageReceived(int32_t what);

    // for debugging only
    // TODO: this should be made accessible only to HWComposer
    const Vector< sp<Layer> >& getLayerSortedByZForHwcDisplay(int id);

    RenderEngine& getRenderEngine() const {
        return *mRenderEngine;
    }

    bool skipPrimaryDisplay(size_t dpy) const {
#ifdef SINGLE_EXTERNAL_DISPLAY_USE_FB1
        for (size_t i=0 ; i<mDisplays.size() ; i++) {
            if (mDisplays[i]->getDisplayType() == HWC_DISPLAY_EXTERNAL
                && mDisplays[dpy]->getDisplayType() == HWC_DISPLAY_PRIMARY) {
                return true;
            }
        }
#endif
        return false;
    }

private:
    friend class Client;
    friend class DisplayEventConnection;
    friend class Layer;
    friend class MonitoredProducer;

    // This value is specified in number of frames.  Log frame stats at most
    // every half hour.
    enum { LOG_FRAME_STATS_PERIOD =  30*60*60 };

    // We're reference counted, never destroy SurfaceFlinger directly
    virtual ~SurfaceFlinger();

    /* ------------------------------------------------------------------------
     * Internal data structures
     */

    class LayerVector : public SortedVector< sp<Layer> > {
    public:
        LayerVector();
        LayerVector(const LayerVector& rhs);
        virtual int do_compare(const void* lhs, const void* rhs) const;
    };

    struct DisplayDeviceState {
        DisplayDeviceState();
        DisplayDeviceState(DisplayDevice::DisplayType type);
        bool isValid() const { return type >= 0; }
        bool isMainDisplay() const { return type == DisplayDevice::DISPLAY_PRIMARY; }
        bool isVirtualDisplay() const { return type >= DisplayDevice::DISPLAY_VIRTUAL; }
        DisplayDevice::DisplayType type;
        sp<IGraphicBufferProducer> surface;
        uint32_t layerStack;
        Rect viewport;
        uint32_t vFormat;
        uint32_t vX,vY,vWidth,vHeight;
        uint32_t d3Format;
        Rect frame;
        uint8_t orientation;
        uint32_t width, height;
        String8 displayName;
        bool isSecure;
    };

    struct State {
        LayerVector layersSortedByZ;
        DefaultKeyedVector< wp<IBinder>, DisplayDeviceState> displays;
    };

    /* ------------------------------------------------------------------------
     * IBinder interface
     */
    virtual status_t onTransact(uint32_t code, const Parcel& data,
        Parcel* reply, uint32_t flags);
    virtual status_t dump(int fd, const Vector<String16>& args);

    /* ------------------------------------------------------------------------
     * ISurfaceComposer interface
     */
    virtual sp<ISurfaceComposerClient> createConnection();
    virtual sp<IGraphicBufferAlloc> createGraphicBufferAlloc();
    virtual sp<IBinder> createDisplay(const String8& displayName, bool secure);
    virtual void destroyDisplay(const sp<IBinder>& display);
    virtual sp<IBinder> getBuiltInDisplay(int32_t id);
    virtual void setTransactionState(const Vector<ComposerState>& state,
            const Vector<DisplayState>& displays, uint32_t flags);
    virtual void bootFinished();
    virtual bool authenticateSurfaceTexture(
        const sp<IGraphicBufferProducer>& bufferProducer) const;
    virtual sp<IDisplayEventConnection> createDisplayEventConnection();
    virtual status_t captureScreen(const sp<IBinder>& display,
            const sp<IGraphicBufferProducer>& producer,
            Rect sourceCrop, uint32_t reqWidth, uint32_t reqHeight,
            uint32_t minLayerZ, uint32_t maxLayerZ,
            bool useIdentityTransform, ISurfaceComposer::Rotation rotation);
    virtual status_t getDisplayStats(const sp<IBinder>& display,
            DisplayStatInfo* stats);
    virtual status_t getDisplayConfigs(const sp<IBinder>& display,
            Vector<DisplayInfo>* configs);
    virtual int getActiveConfig(const sp<IBinder>& display);
    virtual void setPowerMode(const sp<IBinder>& display, int mode);
    virtual status_t setActiveConfig(const sp<IBinder>& display, int id);
    virtual status_t clearAnimationFrameStats();
    virtual status_t getAnimationFrameStats(FrameStats* outStats) const;

    /* ------------------------------------------------------------------------
     * DeathRecipient interface
     */
    virtual void binderDied(const wp<IBinder>& who);

    /* ------------------------------------------------------------------------
     * RefBase interface
     */
    virtual void onFirstRef();

    /* ------------------------------------------------------------------------
     * HWComposer::EventHandler interface
     */
    virtual void onVSyncReceived(int type, nsecs_t timestamp);
    virtual void onHotplugReceived(int disp, bool connected);

    /* ------------------------------------------------------------------------
     * Message handling
     */
    void waitForEvent();
    void signalTransaction();
    void signalLayerUpdate();
    void signalRefresh();

    // called on the main thread in response to initializeDisplays()
    void onInitializeDisplays();
    // called on the main thread in response to setActiveConfig()
    void setActiveConfigInternal(const sp<DisplayDevice>& hw, int mode);
    // called on the main thread in response to setPowerMode()
    void setPowerModeInternal(const sp<DisplayDevice>& hw, int mode);

    void handleMessageTransaction();
    void handleMessageInvalidate();
    void handleMessageRefresh();

    void handleTransaction(uint32_t transactionFlags);
    void handleTransactionLocked(uint32_t transactionFlags);

    void updateCursorAsync();

    /* handlePageFilp: this is were we latch a new buffer
     * if available and compute the dirty region.
     */
    void handlePageFlip();

    /* ------------------------------------------------------------------------
     * Transactions
     */
    uint32_t getTransactionFlags(uint32_t flags);
    uint32_t peekTransactionFlags(uint32_t flags);
    uint32_t setTransactionFlags(uint32_t flags);
    uint32_t getDisplayFormatInfo(uint32_t format , uint32_t type);
    void commitTransaction();
    uint32_t setClientStateLocked(const sp<Client>& client, const layer_state_t& s);
    uint32_t setDisplayStateLocked(const DisplayState& s);

    /* ------------------------------------------------------------------------
     * Layer management
     */
    status_t createLayer(const String8& name, const sp<Client>& client,
            uint32_t w, uint32_t h, PixelFormat format, uint32_t flags,
            sp<IBinder>* handle, sp<IGraphicBufferProducer>* gbp);

    status_t createNormalLayer(const sp<Client>& client, const String8& name,
            uint32_t w, uint32_t h, uint32_t flags, PixelFormat& format,
            sp<IBinder>* outHandle, sp<IGraphicBufferProducer>* outGbp,
            sp<Layer>* outLayer);

    status_t createDimLayer(const sp<Client>& client, const String8& name,
            uint32_t w, uint32_t h, uint32_t flags, sp<IBinder>* outHandle,
            sp<IGraphicBufferProducer>* outGbp, sp<Layer>* outLayer);

    // called in response to the window-manager calling
    // ISurfaceComposerClient::destroySurface()
    status_t onLayerRemoved(const sp<Client>& client, const sp<IBinder>& handle);

    // called when all clients have released all their references to
    // this layer meaning it is entirely safe to destroy all
    // resources associated to this layer.
    status_t onLayerDestroyed(const wp<Layer>& layer);

    // remove a layer from SurfaceFlinger immediately
    status_t removeLayer(const sp<Layer>& layer);

    // add a layer to SurfaceFlinger
    void addClientLayer(const sp<Client>& client,
            const sp<IBinder>& handle,
            const sp<IGraphicBufferProducer>& gbc,
            const sp<Layer>& lbc);

    /* ------------------------------------------------------------------------
     * Boot animation, on/off animations and screen capture
     */

    void startBootAnim();

    void renderScreenImplLocked(
            const sp<const DisplayDevice>& hw,
            Rect sourceCrop, uint32_t reqWidth, uint32_t reqHeight,
            uint32_t minLayerZ, uint32_t maxLayerZ,
            bool yswap, bool useIdentityTransform, Transform::orientation_flags rotation);

    status_t captureScreenImplLocked(
            const sp<const DisplayDevice>& hw,
            const sp<IGraphicBufferProducer>& producer,
            Rect sourceCrop, uint32_t reqWidth, uint32_t reqHeight,
            uint32_t minLayerZ, uint32_t maxLayerZ,
            bool useIdentityTransform, Transform::orientation_flags rotation);

    /* ------------------------------------------------------------------------
     * EGL
     */
    size_t getMaxTextureSize() const;
    size_t getMaxViewportDims() const;

    /* ------------------------------------------------------------------------
     * Display and layer stack management
     */
    // called when starting, or restarting after system_server death
    void initializeDisplays();

    void unblankSignalRefresh();

    // Create an IBinder for a builtin display and add it to current state
    void createBuiltinDisplayLocked(DisplayDevice::DisplayType type);

    // NOTE: can only be called from the main thread or with mStateLock held
    sp<const DisplayDevice> getDisplayDevice(const wp<IBinder>& dpy) const {
        return mDisplays.valueFor(dpy);
    }

    // NOTE: can only be called from the main thread or with mStateLock held
    sp<DisplayDevice> getDisplayDevice(const wp<IBinder>& dpy) {
        return mDisplays.valueFor(dpy);
    }

    // mark a region of a layer stack dirty. this updates the dirty
    // region of all screens presenting this layer stack.
    void invalidateLayerStack(uint32_t layerStack, const Region& dirty);

    // allocate a h/w composer display id
    int32_t allocateHwcDisplayId(DisplayDevice::DisplayType type);

    /* ------------------------------------------------------------------------
     * H/W composer
     */

    HWComposer& getHwComposer() const { return *mHwc; }

    /* ------------------------------------------------------------------------
     * Compositing
     */
    void invalidateHwcGeometry();
    static void computeVisibleRegions(
            const LayerVector& currentLayers, uint32_t layerStack,
            Region& dirtyRegion, Region& opaqueRegion);

    void preComposition();
    void postComposition();
    void rebuildLayerStacks();
    void setUpHWComposer();
    void doComposition();
    void doDebugFlashRegions();
    void doDisplayComposition(const sp<const DisplayDevice>& hw, const Region& dirtyRegion);

    // compose surfaces for display hw. this fails if using GL and the surface
    // has been destroyed and is no longer valid.
    bool doComposeSurfaces(const sp<const DisplayDevice>& hw, const Region& dirty);
    bool doComposeSurfacesEx(const sp<const DisplayDevice>& hw, const Region& dirty);

    void postFramebuffer();
    void drawWormhole(const sp<const DisplayDevice>& hw, const Region& region) const;

    //return true is video buffer is in used
    bool isVideoBufUsed(void) const;
    void setCursorRange(int src_w, int src_h, int des_w, int des_h);
    void setDisplayAxis(int offset_x, int offset_y, int w, int h);

    /* ------------------------------------------------------------------------
     * Display management
     */

    /* ------------------------------------------------------------------------
     * VSync
     */
     void enableHardwareVsync();
     void disableHardwareVsync(bool makeUnavailable);
     void resyncToHardwareVsync(bool makeAvailable);

    /* ------------------------------------------------------------------------
     * Debugging & dumpsys
     */
    void listLayersLocked(const Vector<String16>& args, size_t& index, String8& result) const;
    void dumpStatsLocked(const Vector<String16>& args, size_t& index, String8& result) const;
    void clearStatsLocked(const Vector<String16>& args, size_t& index, String8& result);
    void dumpAllLocked(const Vector<String16>& args, size_t& index, String8& result) const;
    bool startDdmConnection();
    static void appendSfConfigString(String8& result);
    void checkScreenshot(size_t w, size_t s, size_t h, void const* vaddr,
            const sp<const DisplayDevice>& hw,
            uint32_t minLayerZ, uint32_t maxLayerZ);

    void logFrameStats();

    /* ------------------------------------------------------------------------
     * Attributes
     */

    // access must be protected by mStateLock
    mutable Mutex mStateLock;
    State mCurrentState;
    volatile int32_t mTransactionFlags;
    Condition mTransactionCV;
    bool mTransactionPending;
    bool mAnimTransactionPending;
    Vector< sp<Layer> > mLayersPendingRemoval;
    SortedVector< wp<IBinder> > mGraphicBufferProducerList;

    // protected by mStateLock (but we could use another lock)
    bool mLayersRemoved;

    // access must be protected by mInvalidateLock
    volatile int32_t mRepaintEverything;

    // constant members (no synchronization needed for access)
    HWComposer* mHwc;
    RenderEngine* mRenderEngine;
    nsecs_t mBootTime;
    bool mGpuToCpuSupported;
    sp<EventThread> mEventThread;
    sp<EventThread> mSFEventThread;
    sp<EventControlThread> mEventControlThread;
    EGLContext mEGLContext;
    EGLDisplay mEGLDisplay;
    sp<IBinder> mBuiltinDisplays[DisplayDevice::NUM_BUILTIN_DISPLAY_TYPES];

    // Can only accessed from the main thread, these members
    // don't need synchronization
    State mDrawingState;
    bool mVisibleRegionsDirty;
    bool mHwWorkListDirty;
    bool mAnimCompositionPending;

    // this may only be written from the main thread with mStateLock held
    // it may be read from other threads with mStateLock held
    DefaultKeyedVector< wp<IBinder>, sp<DisplayDevice> > mDisplays;

    // don't use a lock for these, we don't care
    int mDebugRegion;
    int mDebugDDMS;
    int mDebugDisableHWC;
    int mDebugDisableTransformHint;
    volatile nsecs_t mDebugInSwapBuffers;
    nsecs_t mLastSwapBufferTime;
    volatile nsecs_t mDebugInTransaction;
    nsecs_t mLastTransactionTime;
    bool mBootFinished;

    // these are thread safe
    mutable MessageQueue mEventQueue;
    FrameTracker mAnimFrameTracker;
    DispSync mPrimaryDispSync;

    // protected by mDestroyedLayerLock;
    mutable Mutex mDestroyedLayerLock;
    Vector<Layer const *> mDestroyedLayers;

    // protected by mHWVsyncLock
    Mutex mHWVsyncLock;
    bool mPrimaryHWVsyncEnabled;
    bool mHWVsyncAvailable;

    /* ------------------------------------------------------------------------
     * Feature prototyping
     */

    Daltonizer mDaltonizer;
    bool mDaltonize;

    mat4 mColorMatrix;
    bool mHasColorMatrix;

    bool mNeed2XScale;
    //add for scale
    uint32_t mQuestX;
    uint32_t mQuestY;
    uint32_t mQuestWidth;
    uint32_t mQuestHeight;
    uint32_t mFormat;
    uint32_t  mLastFormat;
    bool mLastNeed2xscale;
};

}; // namespace android

#endif // ANDROID_SURFACE_FLINGER_H
