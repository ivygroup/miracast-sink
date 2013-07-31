/*
 * Copyright 2012, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "wfd"
#include <utils/Log.h>

#include "sink/WifiDisplaySink.h"
#include "source/WifiDisplaySource.h"

#include <binder/ProcessState.h>
#include <binder/IServiceManager.h>
#include <gui/SurfaceComposerClient.h>
#include <media/AudioSystem.h>
#include <media/IMediaPlayerService.h>
#include <media/IRemoteDisplay.h>
#include <media/IRemoteDisplayClient.h>
#include <media/stagefright/DataSource.h>
#include <media/stagefright/foundation/ADebug.h>

namespace android {

static void usage(const char *me) {
    fprintf(stderr,
            "usage:\n"
            "           %s -c host[:port]\tconnect to wifi source\n"
            "               -u uri        \tconnect to an rtsp uri\n"
            "               -l ip[:port] \tlisten on the specified port "
            "(create a sink)\n",
            me);
}

struct RemoteDisplayClient : public BnRemoteDisplayClient {
    RemoteDisplayClient();

    virtual void onDisplayConnected(
            const sp<ISurfaceTexture> &surfaceTexture,
            uint32_t width,
            uint32_t height,
            uint32_t flags);

    virtual void onDisplayDisconnected();
    virtual void onDisplayError(int32_t error);

    void waitUntilDone();

protected:
    virtual ~RemoteDisplayClient();

private:
    Mutex mLock;
    Condition mCondition;

    bool mDone;

    sp<SurfaceComposerClient> mComposerClient;
    sp<ISurfaceTexture> mSurfaceTexture;
    sp<IBinder> mDisplayBinder;

    DISALLOW_EVIL_CONSTRUCTORS(RemoteDisplayClient);
};

RemoteDisplayClient::RemoteDisplayClient()
    : mDone(false) {
    mComposerClient = new SurfaceComposerClient;
    CHECK_EQ(mComposerClient->initCheck(), (status_t)OK);
}

RemoteDisplayClient::~RemoteDisplayClient() {
}

void RemoteDisplayClient::onDisplayConnected(
        const sp<ISurfaceTexture> &surfaceTexture,
        uint32_t width,
        uint32_t height,
        uint32_t flags) {
    ALOGI("onDisplayConnected width=%u, height=%u, flags = 0x%08x",
          width, height, flags);

    mSurfaceTexture = surfaceTexture;
    mDisplayBinder = mComposerClient->createDisplay(
            String8("foo"), false /* secure */);

    SurfaceComposerClient::openGlobalTransaction();
    mComposerClient->setDisplaySurface(mDisplayBinder, mSurfaceTexture);

    Rect layerStackRect(1280, 720);  // XXX fix this.
    Rect displayRect(1280, 720);

    mComposerClient->setDisplayProjection(
            mDisplayBinder, 0 /* 0 degree rotation */,
            layerStackRect,
            displayRect);

    SurfaceComposerClient::closeGlobalTransaction();
}

void RemoteDisplayClient::onDisplayDisconnected() {
    ALOGI("onDisplayDisconnected");

    Mutex::Autolock autoLock(mLock);
    mDone = true;
    mCondition.broadcast();
}

void RemoteDisplayClient::onDisplayError(int32_t error) {
    ALOGI("onDisplayError error=%d", error);

    Mutex::Autolock autoLock(mLock);
    mDone = true;
    mCondition.broadcast();
}

void RemoteDisplayClient::waitUntilDone() {
    Mutex::Autolock autoLock(mLock);
    while (!mDone) {
        mCondition.wait(mLock);
    }
}

static status_t enableAudioSubmix(bool enable) {
    status_t err = AudioSystem::setDeviceConnectionState(
            AUDIO_DEVICE_IN_REMOTE_SUBMIX,
            enable
                ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE
                : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
            NULL /* device_address */);

    if (err != OK) {
        return err;
    }

    err = AudioSystem::setDeviceConnectionState(
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
            enable
                ? AUDIO_POLICY_DEVICE_STATE_AVAILABLE
                : AUDIO_POLICY_DEVICE_STATE_UNAVAILABLE,
            NULL /* device_address */);

    return err;
}

static void createSource(const AString &addr, int32_t port) {
    sp<IServiceManager> sm = defaultServiceManager();
    sp<IBinder> binder = sm->getService(String16("media.player"));
    sp<IMediaPlayerService> service =
        interface_cast<IMediaPlayerService>(binder);

    CHECK(service.get() != NULL);

    enableAudioSubmix(true /* enable */);

    String8 iface;
    iface.append(addr.c_str());
    iface.append(StringPrintf(":%d", port).c_str());

    sp<RemoteDisplayClient> client = new RemoteDisplayClient;
    sp<IRemoteDisplay> display = service->listenForRemoteDisplay(client, iface);

    client->waitUntilDone();

    display->dispose();
    display.clear();

    enableAudioSubmix(false /* enable */);
}

}  // namespace android

int main(int argc, char **argv) {
    using namespace android;

    ProcessState::self()->startThreadPool();

    DataSource::RegisterDefaultSniffers();

    AString connectToHost;
    int32_t connectToPort = -1;
    AString uri;

    AString listenOnAddr;
    int32_t listenOnPort = -1;

    int res;
    while ((res = getopt(argc, argv, "hc:l:u:")) >= 0) {
        switch (res) {
            case 'c':
            {
                const char *colonPos = strrchr(optarg, ':');

                if (colonPos == NULL) {
                    connectToHost = optarg;
                    connectToPort = WifiDisplaySource::kWifiDisplayDefaultPort;
                } else {
                    connectToHost.setTo(optarg, colonPos - optarg);

                    char *end;
                    connectToPort = strtol(colonPos + 1, &end, 10);

                    if (*end != '\0' || end == colonPos + 1
                            || connectToPort < 1 || connectToPort > 65535) {
                        fprintf(stderr, "Illegal port specified.\n");
                        exit(1);
                    }
                }
                break;
            }

            case 'u':
            {
                uri = optarg;
                break;
            }

            case 'l':
            {
                const char *colonPos = strrchr(optarg, ':');

                if (colonPos == NULL) {
                    listenOnAddr = optarg;
                    listenOnPort = WifiDisplaySource::kWifiDisplayDefaultPort;
                } else {
                    listenOnAddr.setTo(optarg, colonPos - optarg);

                    char *end;
                    listenOnPort = strtol(colonPos + 1, &end, 10);

                    if (*end != '\0' || end == colonPos + 1
                            || listenOnPort < 1 || listenOnPort > 65535) {
                        fprintf(stderr, "Illegal port specified.\n");
                        exit(1);
                    }
                }
                break;
            }

            case '?':
            case 'h':
            default:
                usage(argv[0]);
                exit(1);
        }
    }

    if (connectToPort >= 0 && listenOnPort >= 0) {
        fprintf(stderr,
                "You can connect to a source or create one, "
                "but not both at the same time.\n");
        exit(1);
    }

    if (listenOnPort >= 0) {
        createSource(listenOnAddr, listenOnPort);
        exit(0);
    }

    if (connectToPort < 0 && uri.empty()) {
        fprintf(stderr,
                "You need to select either source host or uri.\n");

        exit(1);
    }

    if (connectToPort >= 0 && !uri.empty()) {
        fprintf(stderr,
                "You need to either connect to a wfd host or an rtsp url, "
                "not both.\n");
        exit(1);
    }

    sp<ANetworkSession> session = new ANetworkSession;
    session->start();

    sp<ALooper> looper = new ALooper;

    sp<WifiDisplaySink> sink = new WifiDisplaySink(session);
    looper->registerHandler(sink);

    if (connectToPort >= 0) {
        sink->start(connectToHost.c_str(), connectToPort);
    } else {
        sink->start(uri.c_str());
    }

    looper->start(true /* runOnCallingThread */);

    return 0;
}
