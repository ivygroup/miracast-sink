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

#ifndef RTP_SINK_H_

#define RTP_SINK_H_

#include <media/stagefright/foundation/AHandler.h>

#include "LinearRegression.h"

#include <gui/Surface.h>

namespace android {

struct ABuffer;
struct ANetworkSession;
struct TunnelRenderer;

// Creates a pair of sockets for RTP/RTCP traffic, instantiates a renderer
// for incoming transport stream data and occasionally sends statistics over
// the RTCP channel.
struct RTPSink : public AHandler {
    RTPSink(const sp<ANetworkSession> &netSession,
            const sp<ISurfaceTexture> &surfaceTex);

    // If TCP interleaving is used, no UDP sockets are created, instead
    // incoming RTP/RTCP packets (arriving on the RTSP control connection)
    // are manually injected by WifiDisplaySink.
    status_t init(bool useTCPInterleaving);

    status_t connect(
            const char *host, int32_t remoteRtpPort, int32_t remoteRtcpPort);

    int32_t getRTPPort() const;

    status_t injectPacket(bool isRTP, const sp<ABuffer> &buffer);

protected:
    virtual void onMessageReceived(const sp<AMessage> &msg);
    virtual ~RTPSink();

private:
    enum {
        kWhatRTPNotify,
        kWhatRTCPNotify,
        kWhatSendRR,
        kWhatPacketLost,
        kWhatInject,
    };

    struct Source;
    struct StreamSource;

    sp<ANetworkSession> mNetSession;
    sp<ISurfaceTexture> mSurfaceTex;
    KeyedVector<uint32_t, sp<Source> > mSources;

    int32_t mRTPPort;
    int32_t mRTPSessionID;
    int32_t mRTCPSessionID;

    int64_t mFirstArrivalTimeUs;
    int64_t mNumPacketsReceived;
    LinearRegression mRegression;
    int64_t mMaxDelayMs;

    sp<TunnelRenderer> mRenderer;

    status_t parseRTP(const sp<ABuffer> &buffer);
    status_t parseRTCP(const sp<ABuffer> &buffer);
    status_t parseBYE(const uint8_t *data, size_t size);
    status_t parseSR(const uint8_t *data, size_t size);

    void addSDES(const sp<ABuffer> &buffer);
    void onSendRR();
    void onPacketLost(const sp<AMessage> &msg);
    void scheduleSendRR();

    DISALLOW_EVIL_CONSTRUCTORS(RTPSink);
};

}  // namespace android

#endif  // RTP_SINK_H_
