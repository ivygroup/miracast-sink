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

#ifndef TUNNEL_RENDERER_H_

#define TUNNEL_RENDERER_H_

#include <gui/Surface.h>
#include <media/stagefright/foundation/AHandler.h>

namespace android {

struct ABuffer;
struct SurfaceComposerClient;
struct SurfaceControl;
struct Surface;
struct IMediaPlayer;
struct IStreamListener;

// This class reassembles incoming RTP packets into the correct order
// and sends the resulting transport stream to a mediaplayer instance
// for playback.
struct TunnelRenderer : public AHandler {
    TunnelRenderer(
            const sp<AMessage> &notifyLost,
            const sp<ISurfaceTexture> &surfaceTex);

    sp<ABuffer> dequeueBuffer();

    enum {
        kWhatQueueBuffer,
    };

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~TunnelRenderer();

private:
    struct PlayerClient;
    struct StreamSource;

    mutable Mutex mLock;

    sp<AMessage> mNotifyLost;
    sp<ISurfaceTexture> mSurfaceTex;

    List<sp<ABuffer> > mPackets;
    int64_t mTotalBytesQueued;

    sp<SurfaceComposerClient> mComposerClient;
    sp<SurfaceControl> mSurfaceControl;
    sp<Surface> mSurface;
    sp<PlayerClient> mPlayerClient;
    sp<IMediaPlayer> mPlayer;
    sp<StreamSource> mStreamSource;

    int32_t mLastDequeuedExtSeqNo;
    int64_t mFirstFailedAttemptUs;
    bool mRequestedRetransmission;

    void initPlayer();
    void destroyPlayer();

    void queueBuffer(const sp<ABuffer> &buffer);

    DISALLOW_EVIL_CONSTRUCTORS(TunnelRenderer);
};

}  // namespace android

#endif  // TUNNEL_RENDERER_H_
