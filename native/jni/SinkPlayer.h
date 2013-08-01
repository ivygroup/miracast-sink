#ifndef SINK_PLAYER_H_

#define SINK_PLAYER_H_

#include <media/stagefright/foundation/ABase.h>
#include <utils/Errors.h>
#include <utils/RefBase.h>

namespace android {

struct ALooper;
struct ANetworkSession;
struct IRemoteDisplayClient;
struct WifiDisplaySink;

class SinkPlayer {
    SinkPlayer(const char *host, int32_t port);

    virtual status_t dispose();

protected:
    virtual ~SinkPlayer();

private:
    sp<ALooper> mNetLooper;
    sp<ALooper> mLooper;
    sp<ANetworkSession> mNetSession;
    sp<WifiDisplaySink> mSink;

    DISALLOW_EVIL_CONSTRUCTORS(SinkPlayer);
};

}

#endif // SINK_PLAYER_H_
