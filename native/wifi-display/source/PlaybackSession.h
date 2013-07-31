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

#ifndef PLAYBACK_SESSION_H_

#define PLAYBACK_SESSION_H_

#include "Sender.h"
#include "WifiDisplaySource.h"

namespace android {

struct ABuffer;
struct BufferQueue;
struct IHDCP;
struct ISurfaceTexture;
struct MediaPuller;
struct MediaSource;
struct TSPacketizer;

// Encapsulates the state of an RTP/RTCP session in the context of wifi
// display.
struct WifiDisplaySource::PlaybackSession : public AHandler {
    PlaybackSession(
            const sp<ANetworkSession> &netSession,
            const sp<AMessage> &notify,
            const struct in_addr &interfaceAddr,
            const sp<IHDCP> &hdcp);

    status_t init(
            const char *clientIP, int32_t clientRtp, int32_t clientRtcp,
            Sender::TransportMode transportMode,
            bool usePCMAudio);

    void destroyAsync();

    int32_t getRTPPort() const;

    int64_t getLastLifesignUs() const;
    void updateLiveness();

    status_t play();
    status_t finishPlay();
    status_t pause();

    sp<ISurfaceTexture> getSurfaceTexture();
    int32_t width() const;
    int32_t height() const;

    void requestIDRFrame();

    enum {
        kWhatSessionDead,
        kWhatBinaryData,
        kWhatSessionEstablished,
        kWhatSessionDestroyed,
    };

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~PlaybackSession();

private:
    struct Track;

    enum {
        kWhatMediaPullerNotify,
        kWhatConverterNotify,
        kWhatTrackNotify,
        kWhatSenderNotify,
        kWhatUpdateSurface,
        kWhatFinishPlay,
        kWhatPacketize,
    };

    sp<ANetworkSession> mNetSession;
    sp<Sender> mSender;
    sp<ALooper> mSenderLooper;
    sp<AMessage> mNotify;
    in_addr mInterfaceAddr;
    sp<IHDCP> mHDCP;
    bool mWeAreDead;

    int64_t mLastLifesignUs;

    sp<TSPacketizer> mPacketizer;
    sp<BufferQueue> mBufferQueue;

    KeyedVector<size_t, sp<Track> > mTracks;
    ssize_t mVideoTrackIndex;

    int64_t mPrevTimeUs;

    bool mAllTracksHavePacketizerIndex;

    status_t setupPacketizer(bool usePCMAudio);

    status_t addSource(
            bool isVideo,
            const sp<MediaSource> &source,
            bool isRepeaterSource,
            bool usePCMAudio,
            size_t *numInputBuffers);

    status_t addVideoSource();
    status_t addAudioSource(bool usePCMAudio);

    ssize_t appendTSData(
            const void *data, size_t size, bool timeDiscontinuity, bool flush);

    status_t onFinishPlay();
    status_t onFinishPlay2();

    bool allTracksHavePacketizerIndex();

    status_t packetizeAccessUnit(
            size_t trackIndex, sp<ABuffer> accessUnit,
            sp<ABuffer> *packets);

    status_t packetizeQueuedAccessUnits();

    void notifySessionDead();

    void drainAccessUnits();

    // Returns true iff an access unit was successfully drained.
    bool drainAccessUnit();

    DISALLOW_EVIL_CONSTRUCTORS(PlaybackSession);
};

}  // namespace android

#endif  // PLAYBACK_SESSION_H_

