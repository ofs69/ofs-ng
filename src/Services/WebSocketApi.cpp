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
#include <charconv>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WS2tcpip.h>
#include <WinSock2.h>
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
        explicit Client(Socket clientSocket) : socket(clientSocket) {
            input.reserve(kMaxHandshake);
            output.reserve(kMaxHandshake);
            fragmentedText.reserve(kMaxHandshake);
        }

        Socket socket;
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
    WebSocketApiStatus status;
    Socket listener = kInvalidSocket;
    std::vector<Client> clients;
    int activePort = 0;
    int attemptedPort = 0;
    bool attemptedEnabled = false;
    bool socketsInitialized = false;
    bool playing = false;
    float speed = 1.0f;
    double duration = 0.0;
    double lastPosition = -1.0;
    uint64_t lastRevision = 0;
    std::string lastProjectPath;
    std::string lastMediaPath;
    std::bitset<kStandardAxisCount> lastShownAxes;
    std::bitset<kStandardAxisCount> announcedAxes;
    std::bitset<kStandardAxisCount> dirtyAxes;
    double dirtyForSeconds = 0.0;
    bool fullResyncPending = false;
    std::string serializedPayload;
    std::string serializedFrame;

    Impl(ScriptProject &projectRef, EventQueue &queue, const AppSettings &settingsRef)
        : project(projectRef), eq(queue), settings(settingsRef) {
        clients.reserve(kMaxClients);
        serializedPayload.reserve(256);
        serializedFrame.reserve(272);
        cacheProjectIdentity();
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

    std::bitset<kStandardAxisCount> shownAxes() const {
        std::bitset<kStandardAxisCount> result;
        for (size_t i = 0; i < kStandardAxisCount; ++i)
            result.set(i, project.axes[i].showInStrip);
        return result;
    }

    void cacheProjectIdentity() {
        lastProjectPath = project.state.filePath;
        lastMediaPath = project.state.mediaPath;
        lastShownAxes = shownAxes();
    }

    bool projectIdentityChanged() {
        const auto currentShownAxes = shownAxes();
        if (lastProjectPath == project.state.filePath && lastMediaPath == project.state.mediaPath &&
            lastShownAxes == currentShownAxes)
            return false;
        cacheProjectIdentity();
        return true;
    }

    void stop() {
        for (Client &client : clients)
            closeSocket(client.socket);
        clients.clear();
        status.clientCount = 0;
        closeSocket(listener);
        listener = kInvalidSocket;
        activePort = 0;
        status.running = false;
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
            status.error = "Unable to initialize Winsock";
            return false;
        }
        socketsInitialized = true;
#endif
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalidSocket) {
            status.error = "Unable to create the listening socket";
            stop();
            return false;
        }

        // SO_REUSEADDR means opposite things on the two platforms. On POSIX it only lets us rebind our
        // own address through TIME_WAIT. On Windows it lets an *unrelated* process bind the same
        // 127.0.0.1:port and take over the endpoint — measured: a second listener with SO_REUSEADDR
        // steals the port outright. SO_EXCLUSIVEADDRUSE is the Windows option that reserves it (the
        // thief then gets WSAEACCES), and it costs no restart latency: a listener closed after its
        // accepted connections rebinds immediately.
#ifdef _WIN32
        const int exclusive = 1;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                   sizeof(exclusive));
#else
        const int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
            listen(listener, static_cast<int>(kMaxClients)) != 0 || !setNonBlocking(listener)) {
            status.error = "Unable to listen on 127.0.0.1:" + std::to_string(port);
            stop();
            return false;
        }
        activePort = port;
        status.running = true;
        status.error.clear();
        OFS_CORE_INFO("Classic OFS WebSocket API listening on ws://127.0.0.1:{}/ofs", port);
        return true;
    }

    void reconcileSettings() {
        const bool enabled = settings.webSocketServerEnabled;
        const int port =
            std::clamp(settings.webSocketPort, 1, static_cast<int>((std::numeric_limits<uint16_t>::max)()));
        if (!enabled) {
            if (running())
                stop();
            attemptedEnabled = false;
            attemptedPort = port;
            status.error.clear();
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

    static bool appendOutput(Client &client, std::string_view data) {
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

    void serializeJson(const nlohmann::json &json) {
        serializedPayload = json.dump();
        ws::encodeFrame(serializedFrame, 0x1, serializedPayload);
    }

    bool queueJson(Client &client, const nlohmann::json &json) {
        serializeJson(json);
        return appendOutput(client, serializedFrame);
    }

    void broadcastFrame(std::string_view frame) {
        if (clients.empty())
            return;
        for (Client &client : clients)
            if (client.upgraded && !appendOutput(client, frame))
                client.closeAfterWrite = true;
    }

    void broadcast(const nlohmann::json &json) {
        serializeJson(json);
        broadcastFrame(serializedFrame);
    }

    void broadcastPosition(double position) {
        std::array<char, 32> number{};
        const auto [end, errorCode] = std::to_chars(number.data(), number.data() + number.size(), position);
        if (errorCode != std::errc{})
            return;
        serializedPayload.clear();
        serializedPayload.append(R"({"data":{"time":)");
        serializedPayload.append(number.data(), end);
        serializedPayload.append(R"(},"name":"time_change","type":"event"})");
        ws::encodeFrame(serializedFrame, 0x1, serializedPayload);
        broadcastFrame(serializedFrame);
    }

    template <class Sink> bool emitFullState(Sink &&sink) {
        bool emitted = sink(event("project_change", nlohmann::json::object()), std::nullopt);
        emitted = sink(event("media_change", {{"path", project.state.mediaPath}}), std::nullopt) && emitted;
        emitted = sink(event("playbackspeed_change", {{"speed", speed}}), std::nullopt) && emitted;
        emitted = sink(event("play_change", {{"playing", playing}}), std::nullopt) && emitted;
        const double currentDuration = duration > 0.0 ? duration : project.state.dummyDuration;
        emitted = sink(event("duration_change", {{"duration", currentDuration}}), std::nullopt) && emitted;
        emitted = sink(event("time_change", {{"time", (std::max)(0.0, project.playback.cursorPos)}}), std::nullopt) &&
                  emitted;
        if (!projectActive(project))
            return emitted;
        for (size_t i = 0; i < kStandardAxisCount; ++i) {
            const StandardAxis role = static_cast<StandardAxis>(i);
            if (role == StandardAxis::L0 || project.axes[i].exists())
                emitted = sink(funscriptEvent(project, role, currentDuration), i) && emitted;
        }
        return emitted;
    }

    bool sendFullState(Client &client) {
        bool queued = queueJson(client, nlohmann::json{{"connected", "OFS " + versionTitle()}});
        queued = emitFullState([this, &client](const nlohmann::json &json, std::optional<size_t> axis) {
                     // Record what this client was told exists, or a later removal of the axis would find
                     // its announcedAxes bit clear and skip the funscript_remove.
                     if (axis)
                         announcedAxes.set(*axis);
                     return queueJson(client, json);
                 }) &&
                 queued;
        return queued;
    }

    // Adopt the project's current state as already-published without sending anything. Used while no
    // client is connected: the bookkeeping would otherwise go stale and the first poll after a connect
    // would re-broadcast every axis on top of the full state the new client was just sent.
    void adoptCurrentStateAsPublished() {
        projectIdentityChanged();
        lastRevision = project.editRevision;
        dirtyAxes.reset();
        dirtyForSeconds = 0.0;
        fullResyncPending = false;
    }

    void broadcastFullState() {
        announcedAxes.reset();
        emitFullState([this](const nlohmann::json &json, std::optional<size_t> axis) {
            broadcast(json);
            if (axis)
                announcedAxes.set(*axis);
            return true;
        });
        dirtyAxes.reset();
        dirtyForSeconds = 0.0;
    }

    void acceptClients() {
        while (clients.size() < kMaxClients) {
            Socket accepted = accept(listener, nullptr, nullptr);
            if (accepted == kInvalidSocket) {
                if (!wouldBlock())
                    status.error = "Failed while accepting a WebSocket client";
                break;
            }
            if (!setNonBlocking(accepted)) {
                closeSocket(accepted);
                continue;
            }
            clients.emplace_back(accepted);
        }
        status.clientCount = clients.size();
    }

    void runCommand(const ws::Command &command) {
        std::visit(
            [this](const auto &typedCommand) {
                using T = std::decay_t<decltype(typedCommand)>;
                if constexpr (std::is_same_v<T, ws::SeekCommand>) {
                    eq.push(SeekEvent{typedCommand.time});
                } else if constexpr (std::is_same_v<T, ws::SetPlayingCommand>) {
                    if (typedCommand.playing != playing) {
                        // Latch the requested state until PlayStateChangedEvent arrives next frame so two clients
                        // asking for the same state cannot enqueue two toggles that cancel each other out.
                        playing = typedCommand.playing;
                        eq.push(PlayPauseEvent{});
                    }
                } else if constexpr (std::is_same_v<T, ws::SetSpeedCommand>) {
                    eq.push(PlaybackSpeedEvent{typedCommand.speed});
                }
            },
            command);
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
            const int count =
                recv(client.socket, reinterpret_cast<char *>(chunk.data()), static_cast<int>(chunk.size()), 0);
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
            const int length =
                static_cast<int>((std::min)(remaining, static_cast<size_t>((std::numeric_limits<int>::max)())));
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
        status.clientCount = clients.size();
    }

    void publishProjectChanges(float dt) {
        if (projectIdentityChanged()) {
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
        if (clients.empty()) {
            adoptCurrentStateAsPublished();
            announcedAxes.reset();
            return;
        }
        const double position = (std::max)(0.0, project.playback.cursorPos);
        if (position != lastPosition) {
            lastPosition = position;
            broadcastPosition(position);
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
        status.clientCount = clients.size();
    }
};

WebSocketApi::WebSocketApi(ScriptProject &project, EventQueue &eq, const AppSettings &settings)
    : impl(std::make_unique<Impl>(project, eq, settings)) {}

WebSocketApi::~WebSocketApi() = default;

void WebSocketApi::update(float dt) {
    impl->update(dt);
}

const WebSocketApiStatus &WebSocketApi::status() const {
    return impl->status;
}

} // namespace ofs
