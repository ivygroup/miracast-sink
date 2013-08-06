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
struct ISurfaceTexture;

class SinkPlayer : public RefBase  {
public:
    SinkPlayer();

    status_t setSurfaceTexture(
            const sp<ISurfaceTexture>& surfaceTexture);

    status_t start(const char *host, int32_t port);
    status_t dispose();

protected:
    virtual ~SinkPlayer();

public:



private:
    sp<ALooper> mNetLooper;
    sp<ALooper> mLooper;
    sp<ANetworkSession> mNetSession;
    sp<WifiDisplaySink> mSink;
    sp<ISurfaceTexture> mSurfaceTexture;

    DISALLOW_EVIL_CONSTRUCTORS(SinkPlayer);
};

}

#endif // SINK_PLAYER_H_
