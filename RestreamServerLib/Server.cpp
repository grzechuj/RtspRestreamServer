#include "Server.h"

#include <set>
#include <map>

#include <CxxPtr/GstRtspServerPtr.h>

#include "Log.h"
#include "StaticSources.h"
#include "Types.h"
#include "RtspAuth.h"
#include "RtspMountPoints.h"

#if GST_CHECK_VERSION(1, 12, 0)
#define ENABLE_LIMITS 1
#endif


namespace RestreamServerLib
{

namespace
{

struct ClientInfo
{
    std::set<std::string> refPaths;
};

struct PathInfo
{
    std::set<const GstRTSPClient*> refClients;
    unsigned playCount;
    const GstRTSPClient* recordClient;
    std::string recordSessionId;
};

}

struct Server::Private
{
    Private(
        const Callbacks& callbacks,
        unsigned short staticPort,
        unsigned short restreamPort,
        unsigned maxPathsCount,
        unsigned maxClientsPerPath);

    Callbacks callbacks;

    const unsigned short staticPort;
    const unsigned short restreamPort;
    const unsigned maxPathsCount;
    const unsigned maxClientsPerPath;

    GstRTSPServerPtr staticServer;

    GstRTSPServerPtr restreamServer;
    GstRTSPAuthPtr auth;
    GstRTSPTokenPtr anonymousToken;
    GstRTSPMountPointsPtr mountPoints;

    std::map<const GstRTSPClient*, ClientInfo> clients;
    std::map<std::string, PathInfo> paths;

    inline const gchar* user(const GstRTSPContext*) const;

    bool isRecording(const GstRTSPClient* client, const std::string& path);
    PathInfo& registerPath(const GstRTSPClient*, const std::string& path);

    void onClientConnected(GstRTSPClient*);

#if ENABLE_LIMITS
    GstRTSPStatusCode beforePlay(const GstRTSPClient*, const GstRTSPUrl*, const gchar* sessionId);
    GstRTSPStatusCode beforeRecord(const GstRTSPClient*, const GstRTSPUrl*, const gchar* sessionId);
#endif

    void onPlay(const GstRTSPClient*, const GstRTSPContext*, const gchar* sessionId);
    void onRecord(const GstRTSPClient*, const GstRTSPContext*, const gchar* sessionId);
    void onTeardown(const GstRTSPClient*, const GstRTSPUrl*, const gchar* sessionId);

    void onClientClosed(const GstRTSPClient*);

    void firstPlayerConnected(const GstRTSPContext* ctx, const std::string& path);
    void lastPlayerDisconnected(const std::string& path);

    void recorderConnected(const GstRTSPContext* ctx, const std::string& path);
    void recorderDisconnected(const std::string& path);
};


const std::shared_ptr<spdlog::logger>& Server::Log()
{
    return RestreamServerLib::Log();
}

Server::Private::Private(
    const Callbacks& callbacks,
    unsigned short staticPort,
    unsigned short restreamPort,
    unsigned maxPathsCount,
    unsigned maxClientsPerPath) :
    callbacks(callbacks),
    staticPort(staticPort),
    restreamPort(restreamPort),
    maxPathsCount(maxPathsCount),
    maxClientsPerPath(maxClientsPerPath)
{
}

const gchar* Server::Private::user(const GstRTSPContext* ctx) const
{
    return
        ctx->token ?
            gst_rtsp_token_get_string(ctx->token, GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE) :
            "";
}

void Server::Private::firstPlayerConnected(
    const GstRTSPContext* ctx,
    const std::string& path)
{
    Log()->debug(
        "First player connected. Path: {}",
        path);

    if(callbacks.firstPlayerConnected)
        callbacks.firstPlayerConnected(user(ctx), path);
}

void Server::Private::lastPlayerDisconnected(
    const std::string& path)
{
    Log()->debug(
        "Last player disconnected. Path: {}",
        path);

    if(callbacks.lastPlayerDisconnected)
        callbacks.lastPlayerDisconnected(path);
}

void Server::Private::recorderConnected(
    const GstRTSPContext* ctx,
    const std::string& path)
{
    Log()->debug(
        "Recorder connected. Path: {}",
        path);

    if(callbacks.recorderConnected)
        callbacks.recorderConnected(user(ctx), path);
}

void Server::Private::recorderDisconnected(
    const std::string& path)
{
    Log()->debug(
        "Recorder disconnected. Path: {}",
        path);

    if(callbacks.recorderDisconnected)
        callbacks.recorderDisconnected(path);
}

bool Server::Private::isRecording(const GstRTSPClient* client, const std::string& path)
{
    auto pathIt = paths.find(path);
    if(paths.end() == pathIt)
        return false;
    else {
        const PathInfo& pathInfo = pathIt->second;
        return pathInfo.recordClient || !pathInfo.recordSessionId.empty();
    }
}

PathInfo& Server::Private::registerPath(const GstRTSPClient* client, const std::string& path)
{
    const auto clientIt = clients.find(client);
    if(clients.end() == clientIt) {
        clients.emplace(client, ClientInfo{.refPaths = {path}});
    } else {
        auto& refPaths = clientIt->second.refPaths;
        refPaths.emplace(path);
    }

    auto pathIt = paths.find(path);
    if(paths.end() == pathIt) {
        pathIt =
            paths.emplace(
                path,
                PathInfo {
                .refClients = {client},
                .playCount = 0,
                .recordClient = nullptr}).first;
    } else {
        auto& refClients = pathIt->second.refClients;
        refClients.emplace(client);
    }

    return pathIt->second;
}

void Server::Private::onClientConnected(GstRTSPClient* client)
{
    GstRTSPConnection* connection =
        gst_rtsp_client_get_connection(client);
    Log()->info(
        "New connection from {}",
        gst_rtsp_connection_get_ip(connection));

#if ENABLE_LIMITS
    auto prePlayCallback =
        (GstRTSPStatusCode (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            return p->beforePlay(client, context->uri, sessionId);
        };
    g_signal_connect(client, "pre-play-request", GCallback(prePlayCallback), this);
#endif

    auto playCallback = (void (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            p->onPlay(client, context, sessionId);
        };
    g_signal_connect(client, "play-request", GCallback(playCallback), this);

#if ENABLE_LIMITS
    auto preRecordCallback =
        (GstRTSPStatusCode (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            return p->beforeRecord(client, context->uri, sessionId);
        };
    g_signal_connect(client, "pre-record-request", GCallback(preRecordCallback), this);
#endif

    auto recordCallback = (void (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            p->onRecord(client, context, sessionId);
        };
    g_signal_connect(client, "record-request", GCallback(recordCallback), this);

    auto teardownCallback = (void (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            p->onTeardown(client, context->uri, sessionId);
        };
    g_signal_connect(client, "teardown-request", GCallback(teardownCallback), this);

    auto closedCallback= (void (*)(GstRTSPClient*, gpointer))
        [] (GstRTSPClient* client, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            p->onClientClosed(client);
        };
    g_signal_connect(client, "closed", GCallback(closedCallback), this);
}

#if ENABLE_LIMITS
GstRTSPStatusCode Server::Private::beforePlay(
    const GstRTSPClient* client,
    const GstRTSPUrl* url,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.beforePlay."
        " client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), url->abspath, sessionId);

    const std::string path = url->abspath;

    auto pathIt = paths.find(path);
    if(maxClientsPerPath > 0 && paths.end() != pathIt) {
        if(pathIt->second.playCount >= (maxClientsPerPath - 1)) {
            Log()->error(
                "Max players count limit reached. "
                "client: {}, path: {}, sessionId: {}",
                static_cast<const void*>(client), url->abspath, sessionId);
            return GST_RTSP_STS_FORBIDDEN;
        }
    }

    return GST_RTSP_STS_OK;
}
#endif

void Server::Private::onPlay(
    const GstRTSPClient* client,
    const GstRTSPContext* ctx,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.play."
        " client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), ctx->uri->abspath, sessionId);

    const std::string path = ctx->uri->abspath;

    PathInfo& pathInfo = registerPath(client, path);
    ++pathInfo.playCount;
    if(1 == pathInfo.playCount)
        firstPlayerConnected(ctx, path);
}

#if ENABLE_LIMITS
GstRTSPStatusCode Server::Private::beforeRecord(
    const GstRTSPClient* client,
    const GstRTSPUrl* url,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.beforeRecord."
        " client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), url->abspath, sessionId);

    if(isRecording(client, url->abspath)) {
        Log()->info(
            "Second record on the same path. client: {}, path: {}",
            static_cast<const void*>(client), url->abspath);
        return GST_RTSP_STS_SERVICE_UNAVAILABLE;
    } else
       return GST_RTSP_STS_OK;
}
#endif

void Server::Private::onRecord(
    const GstRTSPClient* client,
    const GstRTSPContext* ctx,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.record. "
        "client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), ctx->uri->abspath, sessionId);

    const std::string path = ctx->uri->abspath;

    PathInfo& pathInfo = registerPath(client, path);
    if(pathInfo.recordClient || !pathInfo.recordSessionId.empty()) {
        Log()->critical(
            "Second record on the same path. client: {}, path: {}",
            static_cast<const void*>(client), ctx->uri->abspath);
    } else {
        pathInfo.recordClient = client;
        pathInfo.recordSessionId = sessionId;

        recorderConnected(ctx, path);
    }
}

void Server::Private::onTeardown(
    const GstRTSPClient* client,
    const GstRTSPUrl* url,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.teardown. "
        "client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), url->abspath, sessionId);

    const std::string path = url->abspath;

    auto pathIt = paths.find(path);
    if(paths.end() == pathIt) {
        Log()->critical(
            "Not registered path teardown. client: {}, path: {}",
            static_cast<const void*>(client), url->abspath);
        return;
    }

    PathInfo& pathInfo = pathIt->second;
    if(client == pathInfo.recordClient &&
       sessionId == pathInfo.recordSessionId)
    {
        pathInfo.recordClient = nullptr;
        pathInfo.recordSessionId.clear();

        recorderDisconnected(path);
    } else {
        if(pathInfo.playCount > 0) {
            --pathInfo.playCount;
            if(0 == pathInfo.playCount)
                lastPlayerDisconnected(path);
        } else {
            Log()->critical(
                "Not registered reader teardown. client: {}, path: {}",
                static_cast<const void*>(client), url->abspath);
        }
    }
}

void Server::Private::onClientClosed(const GstRTSPClient* client)
{
    Log()->debug(
        "Server.clientClosed. "
        "client: {}",
        static_cast<const void*>(client));

    const auto clientIt = clients.find(client);
    if(clients.end() != clientIt) {
        auto& refPaths = clientIt->second.refPaths;
        for(const std::string& path: refPaths) {
            auto pathIt = paths.find(path);
            if(paths.end() != pathIt) {
                PathInfo& pathInfo = pathIt->second;

                auto& refClients = pathIt->second.refClients;
                refClients.erase(client);

                if(refClients.empty()) {
                    if(nullptr == pathInfo.recordClient) {
                        assert(pathInfo.playCount == 0 || pathInfo.playCount == 1);
                        if(1 == pathInfo.playCount) {
                            --pathInfo.playCount;
                            lastPlayerDisconnected(path);
                        }
                    } else {
                        assert(pathInfo.recordClient == client);

                        pathInfo.recordClient = nullptr;
                        pathInfo.recordSessionId.clear();

                        recorderDisconnected(path);
                    }

                    paths.erase(pathIt);
                } else {
                    if(client == pathInfo.recordClient) {
                        pathInfo.recordClient = nullptr;
                        pathInfo.recordSessionId.clear();

                        recorderDisconnected(path);
                    }

                    if(++refClients.begin() == refClients.end()) {
                        if(nullptr != pathInfo.recordClient) {
                            assert(pathInfo.playCount == 0 || pathInfo.playCount == 1);
                            if(1 == pathInfo.playCount) {
                                --pathInfo.playCount;
                                lastPlayerDisconnected(path);
                            }
                        }
                    }
                }
            } else {
                Log()->critical("Inconsitency between clients and paths");
            }
        }
        clients.erase(clientIt);
    }
}


Server::Server(
    const Callbacks& callbacks,
    unsigned short staticPort,
    unsigned short restreamPort,
    bool useTls,
    unsigned maxPathsCount,
    unsigned maxClientsPerPath) :
    _p(
        new Private(
            callbacks,
            staticPort, restreamPort,
            maxPathsCount, maxClientsPerPath))
{
    initStaticServer();
    initRestreamServer(useTls);
}

Server::~Server()
{
    _p.reset();
}

void Server::initStaticServer()
{
    _p->staticServer.reset(gst_rtsp_server_new());
    GstRTSPMountPointsPtr mountPointsPtr(gst_rtsp_mount_points_new());

    GstRTSPServer* server = _p->staticServer.get();
    GstRTSPMountPoints* mountPoints = mountPointsPtr.get();

    gst_rtsp_server_set_service(
        server,
        fmt::format("{}", _p->staticPort).c_str());

    gst_rtsp_server_set_mount_points(server, mountPoints);

    {
        GstRTSPMediaFactory* barsFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            barsFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(barsFactory,
            "( videotestsrc pattern=smpte100 ! "
            "x264enc ! video/x-h264, profile=baseline ! "
            "rtph264pay name=pay0 pt=96 config-interval=-1 )");
        gst_rtsp_media_factory_set_shared(barsFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, BARS, barsFactory);
    }

    {
        GstRTSPMediaFactory* whiteScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            whiteScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(whiteScreenFactory,
            "( videotestsrc pattern=white ! "
            "x264enc ! video/x-h264, profile=baseline ! "
            "rtph264pay name=pay0 pt=96 config-interval=-1 )");
        gst_rtsp_media_factory_set_shared(whiteScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, WHITE, whiteScreenFactory);
    }

    {
        GstRTSPMediaFactory* blackScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            blackScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(blackScreenFactory,
            "( videotestsrc pattern=black ! "
            "x264enc ! video/x-h264, profile=baseline ! "
            "rtph264pay name=pay0 pt=96 config-interval=-1 )");
        gst_rtsp_media_factory_set_shared(blackScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, BLACK, blackScreenFactory);
    }

    {
        GstRTSPMediaFactory* redScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            redScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(redScreenFactory,
            "( videotestsrc pattern=red ! "
            "x264enc ! video/x-h264, profile=baseline ! "
            "rtph264pay name=pay0 pt=96 config-interval=-1 )");
        gst_rtsp_media_factory_set_shared(redScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, RED, redScreenFactory);
    }

    {
        GstRTSPMediaFactory* greenScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            greenScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(greenScreenFactory,
            "( videotestsrc pattern=green ! "
            "x264enc ! video/x-h264, profile=baseline ! "
            "rtph264pay name=pay0 pt=96 config-interval=-1 )");
        gst_rtsp_media_factory_set_shared(greenScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, GREEN, greenScreenFactory);
    }

    {
        GstRTSPMediaFactory* blueScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            blueScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(blueScreenFactory,
            "( videotestsrc pattern=blue ! "
            "x264enc ! video/x-h264, profile=baseline ! "
            "rtph264pay name=pay0 pt=96 config-interval=-1 )");
        gst_rtsp_media_factory_set_shared(blueScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, BLUE, blueScreenFactory);
    }
}

void Server::initRestreamServer(bool useTls)
{
    _p->restreamServer.reset(gst_rtsp_server_new());

    const AuthCallbacks authCallbacks {
        .tlsAuthenticate = _p->callbacks.tlsAuthenticate,
        .authenticationRequired = _p->callbacks.authenticationRequired,
        .authenticate = _p->callbacks.authenticate,
        .authorize = _p->callbacks.authorize };
    _p->auth.reset(GST_RTSP_AUTH(rtsp_auth_new(authCallbacks, useTls)));

    _p->anonymousToken.reset(
        gst_rtsp_token_new(
            GST_RTSP_TOKEN_MEDIA_FACTORY_ROLE, G_TYPE_STRING, "",
            NULL));

    MountPointsCallbacks mountPointsCallbacks;
    if(authCallbacks.authorize) {
        mountPointsCallbacks.authorizeAccess =
            std::bind(
                _p->callbacks.authorize,
                std::placeholders::_1,
                Action::ACCESS,
                std::placeholders::_2,
                std::placeholders::_3);
    };

    _p->mountPoints.reset(
        GST_RTSP_MOUNT_POINTS(
            rtsp_mount_points_new(
                mountPointsCallbacks,
                fmt::format("rtsp://localhost:{}/blue", _p->staticPort).c_str(),
                _p->maxPathsCount,
                _p->maxClientsPerPath)));

    GstRTSPServer* server = _p->restreamServer.get();
    GstRTSPAuth* auth = _p->auth.get();
    GstRTSPMountPoints* mountPoints = _p->mountPoints.get();
    GstRTSPToken* anonymousToken = _p->anonymousToken.get();

    gst_rtsp_server_set_service(
        server,
        fmt::format("{}", _p->restreamPort).c_str());

    gst_rtsp_server_set_mount_points(server, mountPoints);
    gst_rtsp_auth_set_default_token(auth, anonymousToken);
    gst_rtsp_server_set_auth(server, auth);

    auto clientConnectedCallback =
        (void (*)(GstRTSPServer*, GstRTSPClient*, gpointer))
        [] (GstRTSPServer* /*server*/,
            GstRTSPClient* client,
            gpointer userData)
        {
            Private* p =
                static_cast<Private*>(userData);
            p->onClientConnected(client);
        };
    g_signal_connect(
        server, "client-connected",
        (GCallback) clientConnectedCallback, _p.get());
}

void Server::serverMain()
{
    GstRTSPServer* staticServer = _p->staticServer.get();
    GstRTSPServer* restreamServer = _p->restreamServer.get();

    if(!staticServer) {
        Log()->critical("RTSP static server not initialized");
        return;
    }
    if(!restreamServer) {
        Log()->critical("RTSP restream server not initialized");
        return;
    }

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    gst_rtsp_server_attach(staticServer, nullptr);
    gst_rtsp_server_attach(restreamServer, nullptr);

    Log()->info(
        "RTSP static server running on port {}",
        gst_rtsp_server_get_bound_port(staticServer));
    Log()->info(
        "RTSP restream server running on port {}",
        gst_rtsp_server_get_bound_port(restreamServer));

    g_main_loop_run(loop);
}

void Server::setTlsCertificate(GTlsCertificate* certificate)
{
    gst_rtsp_auth_set_tls_certificate(_p->auth.get(), certificate);
}

}
