#include "Services/WebSocketApi.h"

#include "Core/AxisEvents.h"
#include "Core/EventQueue.h"
#include "Core/PlaybackEvents.h"
#include "Core/ProjectLifecycleEvents.h"
#include "Core/ScriptProject.h"
#include "Format/AppSettings.h"
#include "Format/Funscript.h"
#include "Services/WebSocketProtocol.h"
#include "Util/Log.h"
#include "Util/PathUtil.h"
#include "Util/Version.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ofs {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
#endif

constexpr size_t kMaxClients = 16;
constexpr size_t kMaxHandshake = 64u * 1024u;
constexpr size_t kMaxMessage = 16u * 1024u * 1024u;
constexpr size_t kMaxBufferedInput = kMaxMessage * 2u;
constexpr size_t kMaxQueuedOutput = 64u * 1024u * 1024u;
constexpr double kScriptDebounceSeconds = 0.2;

void closeSocket(Socket socket) {
    if (socket == kInvalidSocket)
        return;
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

bool setNonBlocking(Socket socket) {
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    const int error = WSAGetLastError();
    return error == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN;
#endif
}

int sendFlags() {
#ifdef MSG_NOSIGNAL
    return MSG_NOSIGNAL;
#else
    return 0;
#endif
}

nlohmann::json event(std::string_view name, nlohmann::json data) {
    return {{"type", "event"}, {"name", name}, {"data", std::move(data)}};
}

bool projectActive(const ScriptProject &project) {
    if (!project.state.mediaPath.empty() || !project.state.filePath.empty())
        return true;
    return std::ranges::any_of(project.axes, [](const AxisState &axis) { return axis.showInStrip; });
}

std::string scriptBaseName(const ScriptProject &project) {
    const std::string *path = !project.state.mediaPath.empty() ? &project.state.mediaPath : &project.state.filePath;
    if (!path->empty()) {
        const std::filesystem::path fsPath = util::fromUtf8(*path);
        std::string stem = util::toUtf8(fsPath.stem());
        if (!stem.empty())
            return stem;
    }
    return "untitled";
}

std::string scriptName(const ScriptProject &project, StandardAxis role) {
    std::string name = scriptBaseName(project);
    if (role != StandardAxis::L0) {
        name.push_back('.');
        name.append(standardAxisTag(role));
    }
    name += ".funscript";
    return name;
}

nlohmann::json funscriptEvent(const ScriptProject &project, StandardAxis role, double duration) {
    Funscript script = Funscript::fromActions(project.axes[static_cast<size_t>(role)].actions);
    script.metadata = project.metadata;
    script.bookmarks = project.bookmarks.bookmarks;
    script.chapters = project.bookmarks.chapters;
    script.duration = (std::max<int64_t>)(0, std::llround(duration));
    return event("funscript_change", {{"name", scriptName(project, role)}, {"funscript", script}});
}

} // namespace

struct WebSocketApi::Impl {
    struct Client {
        Socket socket = kInvalidSocket;
        bool upgraded = false;
        bool closeAfterWrite = false;
        std::vector<uint8_t> input;
        std::string output;
        size_t outputOffset = 0;
        std::string fragmentedText;
        bool receivingFragmentedText = false;
    };

    ScriptProject &project;
    EventQueue &eq;
    const AppSettings &settings;
    Socket listener = kInvalidSocket;
    std::vector<Client> clients;
    std::string error;
    int activePort = 0;
    int attemptedPort = 0;
    bool attemptedEnabled = false;
    bool socketsInitialized = false;
    bool playing = false;
    float speed = 1.0f;
    double duration = 0.0;
    double lastPosition = -1.0;
    uint64_t lastRevision = 0;
    std::string lastProjectKey;
    std::bitset<kStandardAxisCount> announcedAxes;
    std::bitset<kStandardAxisCount> dirtyAxes;
    double dirtyForSeconds = 0.0;
    bool fullResyncPending = false;

    Impl(ScriptProject &projectRef, EventQueue &queue, const AppSettings &settingsRef)
        : project(projectRef), eq(queue), settings(settingsRef) {
        eq.on<PlayStateChangedEvent>([this](const PlayStateChangedEvent &e) {
            playing = e.playing;
            broadcast(event("play_change", {{"playing", playing}}));
        });
        eq.on<SpeedChangedEvent>([this](const SpeedChangedEvent &e) {
            speed = e.speed;
            broadcast(event("playbackspeed_change", {{"speed", speed}}));
        });
        eq.on<DurationChangedEvent>([this](const DurationChangedEvent &e) {
            duration = (std::max)(0.0, e.duration);
            broadcast(event("duration_change", {{"duration", duration}}));
        });
        eq.on<MediaChangedEvent>([this](const MediaChangedEvent &e) {
            broadcast(event("media_change", {{"path", e.path}}));
            fullResyncPending = true;
        });
        eq.on<AxisModifiedEvent>([this](const AxisModifiedEvent &e) {
            dirtyAxes.set(static_cast<size_t>(e.role));
            dirtyForSeconds = 0.0;
        });
        eq.on<LoadProjectEvent>([this](const LoadProjectEvent &) { fullResyncPending = true; });
        eq.on<NewProjectCreatedEvent>([this](const NewProjectCreatedEvent &) { fullResyncPending = true; });
        eq.on<ProjectClosedEvent>([this](const ProjectClosedEvent &) { fullResyncPending = true; });
    }

    ~Impl() { stop(); }

    bool running() const { return listener != kInvalidSocket; }

    std::string projectKey() const {
        std::string key = project.state.filePath;
        key.push_back('\n');
        key += project.state.mediaPath;
        key.push_back('\n');
        for (const AxisState &axis : project.axes)
            key.push_back(axis.showInStrip ? '1' : '0');
        return key;
    }

    void stop() {
        for (Client &client : clients)
            closeSocket(client.socket);
        clients.clear();
        closeSocket(listener);
        listener = kInvalidSocket;
        activePort = 0;
#ifdef _WIN32
        if (socketsInitialized)
            WSACleanup();
#endif
        socketsInitialized = false;
    }

    bool start(int port) {
#ifdef _WIN32
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            error = "Unable to initialize Winsock";
            return false;
        }
        socketsInitialized = true;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalidSocket) {
            error = "Unable to create the listening socket";
            stop();
            return false;
        }

        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(listener, static_cast<int>(kMaxClients)) != 0 || !setNonBlocking(listener)) {
            error = "Unable to listen on 127.0.0.1:" + std::to_string(port);
            stop();
            return false;
        }
        activePort = port;
        error.clear();
        OFS_CORE_INFO("Classic OFS WebSocket API listening on ws://127.0.0.1:{}/ofs", port);
        return true;
    }

    void reconcileSettings() {
        const bool enabled = settings.webSocketServerEnabled;
        const int port = std::clamp(settings.webSocketPort, 1,
                                    static_cast<int>((std::numeric_limits<uint16_t>::max)()));
        if (!enabled) {
            if (running())
                stop();
            attemptedEnabled = false;
            attemptedPort = port;
            error.clear();
            return;
        }
        if (running() && activePort == port)
            return;
        if (running())
            stop();
        if (attemptedEnabled && attemptedPort == port)
            return;
        attemptedEnabled = true;
        attemptedPort = port;
        start(port);
    }

    static bool appendOutput(Client &client, std::string data) {
        if (client.outputOffset == client.output.size()) {
            client.output.clear();
            client.outputOffset = 0;
        }
        const size_t pending = client.output.size() - client.outputOffset;
        if (pending > kMaxQueuedOutput || data.size() > kMaxQueuedOutput - pending)
            return false;
        client.output += data;
        return true;
    }

    static bool queueJson(Client &client, const nlohmann::json &json) {
        return appendOutput(client, ws::encodeFrame(0x1, json.dump()));
    }

    void broadcast(const nlohmann::json &json) {
        if (clients.empty())
            return;
        const std::string frame = ws::encodeFrame(0x1, json.dump());
        for (Client &client : clients)
            if (client.upgraded && !appendOutput(client, frame))
                client.closeAfterWrite = true;
    }

    bool sendFullState(Client &client) {
        bool queued = queueJson(client, nlohmann::json{{"connected", "OFS " + versionTitle()}});
        queued = queueJson(client, event("project_change", nlohmann::json::object())) && queued;
        queued = queueJson(client, event("media_change", {{"path", project.state.mediaPath}})) && queued;
        queued = queueJson(client, event("playbackspeed_change", {{"speed", speed}})) && queued;
        queued = queueJson(client, event("play_change", {{"playing", playing}})) && queued;
        const double currentDuration = duration > 0.0 ? duration : project.state.dummyDuration;
        queued = queueJson(client, event("duration_change", {{"duration", currentDuration}})) && queued;
        queued = queueJson(client, event("time_change", {{"time", (std::max)(0.0, project.playback.cursorPos)}})) && queued;
        if (!projectActive(project))
            return queued;
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            const StandardAxis role = static_cast<StandardAxis>(i);
            if (role == StandardAxis::L0 || project.axes[i].exists())
                queued = queueJson(client, funscriptEvent(project, role, currentDuration)) && queued;
        }
        return queued;
    }

    void broadcastFullState() {
        broadcast(event("project_change", nlohmann::json::object()));
        broadcast(event("media_change", {{"path", project.state.mediaPath}}));
        broadcast(event("playbackspeed_change", {{"speed", speed}}));
        broadcast(event("play_change", {{"playing", playing}}));
        const double currentDuration = duration > 0.0 ? duration : project.state.dummyDuration;
        broadcast(event("duration_change", {{"duration", currentDuration}}));
        broadcast(event("time_change", {{"time", (std::max)(0.0, project.playback.cursorPos)}}));
        announcedAxes.reset();
        if (projectActive(project)) {
            for (size_t i = 0; i < kStandardAxisCount; ++i) {
                const StandardAxis role = static_cast<StandardAxis>(i);
                if (role == StandardAxis::L0 || project.axes[i].exists()) {
                    broadcast(funscriptEvent(project, role, currentDuration));
                    announcedAxes.set(i);
                }
            }
        }
        dirtyAxes.reset();
        dirtyForSeconds = 0.0;
    }

    void acceptClients() {
        while (clients.size() < kMaxClients) {
            Socket accepted = accept(listener, nullptr, nullptr);
            if (accepted == kInvalidSocket) {
                if (!wouldBlock())
                    error = "Failed while accepting a WebSocket client";
                break;
            }
            if (!setNonBlocking(accepted)) {
                closeSocket(accepted);
                continue;
            }
            clients.push_back(Client{.socket = accepted});
        }
    }

    void runCommand(const ws::Command &command) {
        switch (command.kind) {
        case ws::CommandKind::Seek:
            if (std::isfinite(command.number) && command.number >= 0.0)
                eq.push(SeekEvent{command.number});
            break;
        case ws::CommandKind::SetPlaying:
            if (command.boolean != playing)
                eq.push(PlayPauseEvent{});
            break;
        case ws::CommandKind::SetSpeed:
            if (std::isfinite(command.number) && command.number > 0.0)
                eq.push(PlaybackSpeedEvent{static_cast<float>(command.number)});
            break;
        }
    }

    bool processFrames(Client &client) {
        while (true) {
            bool protocolError = false;
            std::optional<ws::Frame> frame = ws::consumeFrame(client.input, protocolError);
            if (protocolError)
                return false;
            if (!frame)
                return true;
            switch (frame->opcode) {
            case 0x0:
                if (!client.receivingFragmentedText)
                    return false;
                if (frame->payload.size() > kMaxMessage - client.fragmentedText.size())
                    return false;
                client.fragmentedText += frame->payload;
                if (frame->final) {
                    if (const auto command = ws::parseCommand(client.fragmentedText))
                        runCommand(*command);
                    client.fragmentedText.clear();
                    client.receivingFragmentedText = false;
                }
                break;
            case 0x1:
                if (client.receivingFragmentedText)
                    return false;
                if (frame->final) {
                    if (const auto command = ws::parseCommand(frame->payload))
                        runCommand(*command);
                } else {
                    client.fragmentedText = std::move(frame->payload);
                    client.receivingFragmentedText = true;
                }
                break;
            case 0x8:
                appendOutput(client, ws::encodeFrame(0x8, frame->payload));
                client.closeAfterWrite = true;
                return true;
            case 0x9:
                if (!appendOutput(client, ws::encodeFrame(0xA, frame->payload)))
                    return false;
                break;
            case 0xA:
                break;
            default:
                return false;
            }
        }
    }

    bool receive(Client &client) {
        std::array<uint8_t, 16u * 1024u> chunk{};
        while (true) {
            const int count = recv(client.socket, reinterpret_cast<char *>(chunk.data()), static_cast<int>(chunk.size()), 0);
            if (count == 0)
                return false;
            if (count < 0) {
                if (wouldBlock())
                    break;
                return false;
            }
            client.input.insert(client.input.end(), chunk.begin(), chunk.begin() + count);
            if (!client.upgraded && client.input.size() > kMaxHandshake)
                return false;
            if (client.upgraded && client.input.size() > kMaxBufferedInput)
                return false;
        }

        if (!client.upgraded) {
            static constexpr std::array<uint8_t, 4> end = {'\r', '\n', '\r', '\n'};
            const auto headerEnd = std::search(client.input.begin(), client.input.end(), end.begin(), end.end());
            if (headerEnd == client.input.end())
                return true;
            const size_t headerSize = static_cast<size_t>(std::distance(client.input.begin(), headerEnd)) + end.size();
            const std::string_view request(reinterpret_cast<const char *>(client.input.data()), headerSize);
            const auto response = ws::handshakeResponse(request);
            if (!response)
                return false;
            if (!appendOutput(client, *response))
                return false;
            client.input.erase(client.input.begin(), client.input.begin() + static_cast<std::ptrdiff_t>(headerSize));
            client.upgraded = true;
            if (!sendFullState(client))
                return false;
        }
        return processFrames(client);
    }

    bool flush(Client &client) {
        while (client.outputOffset < client.output.size()) {
            const size_t remaining = client.output.size() - client.outputOffset;
            const int length = static_cast<int>(
                (std::min)(remaining, static_cast<size_t>((std::numeric_limits<int>::max)())));
            const int count = send(client.socket, client.output.data() + client.outputOffset, length, sendFlags());
            if (count < 0) {
                if (wouldBlock())
                    return true;
                return false;
            }
            if (count == 0)
                return false;
            client.outputOffset += static_cast<size_t>(count);
        }
        client.output.clear();
        client.outputOffset = 0;
        return !client.closeAfterWrite;
    }

    void pollClients() {
        acceptClients();
        for (size_t i = 0; i < clients.size();) {
            Client &client = clients[i];
            if (!receive(client) || !flush(client)) {
                closeSocket(client.socket);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }

    void publishProjectChanges(float dt) {
        const std::string key = projectKey();
        if (key != lastProjectKey) {
            lastProjectKey = key;
            fullResyncPending = true;
        }
        if (project.editRevision != lastRevision) {
            lastRevision = project.editRevision;
            dirtyAxes.set();
            dirtyForSeconds = 0.0;
        }
        if (fullResyncPending) {
            broadcastFullState();
            fullResyncPending = false;
            return;
        }
        if (dirtyAxes.none())
            return;
        dirtyForSeconds += dt;
        if (dirtyForSeconds < kScriptDebounceSeconds)
            return;

        const double currentDuration = duration > 0.0 ? duration : project.state.dummyDuration;
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            if (!dirtyAxes.test(i))
                continue;
            const StandardAxis role = static_cast<StandardAxis>(i);
            const bool exists = projectActive(project) && (role == StandardAxis::L0 || project.axes[i].exists());
            if (exists) {
                broadcast(funscriptEvent(project, role, currentDuration));
                announcedAxes.set(i);
            } else if (announcedAxes.test(i)) {
                broadcast(event("funscript_remove", {{"name", scriptName(project, role)}}));
                announcedAxes.reset(i);
            }
        }
        dirtyAxes.reset();
        dirtyForSeconds = 0.0;
    }

    void update(float dt) {
        reconcileSettings();
        if (!running())
            return;
        pollClients();
        if (clients.empty())
            return;
        const double position = (std::max)(0.0, project.playback.cursorPos);
        if (position != lastPosition) {
            lastPosition = position;
            broadcast(event("time_change", {{"time", position}}));
        }
        publishProjectChanges(dt);
        for (size_t i = 0; i < clients.size();) {
            if (!flush(clients[i])) {
                closeSocket(clients[i].socket);
                clients.erase(clients.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
};

WebSocketApi::WebSocketApi(ScriptProject &project, EventQueue &eq, const AppSettings &settings)
    : impl(std::make_unique<Impl>(project, eq, settings)) {}

WebSocketApi::~WebSocketApi() = default;

void WebSocketApi::update(float dt) {
    impl->update(dt);
}

bool WebSocketApi::isRunning() const {
    return impl->running();
}

size_t WebSocketApi::clientCount() const {
    return impl->clients.size();
}

const std::string &WebSocketApi::lastError() const {
    return impl->error;
}

} // namespace ofs
