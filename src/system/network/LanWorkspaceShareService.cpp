#include "system/network/LanWorkspaceShareService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <random>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {
    using SocketHandle = std::intptr_t;
#if defined(_WIN32)
    using NativeSocket = SOCKET;
    using SocketLen = int;
    constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
    using NativeSocket = int;
    using SocketLen = socklen_t;
    constexpr NativeSocket kInvalidSocket = -1;
#endif
    constexpr SocketHandle kInvalidSocketHandle = static_cast<SocketHandle>(kInvalidSocket);

    constexpr uint16_t kDiscoveryPort = 39541;
    constexpr uint16_t kPreferredTransferPort = 39542;
    constexpr std::string_view kProtocolPrefix = "JITGL_LAN_V1";
    constexpr double kHelloIntervalSeconds = 1.5;
    constexpr double kPeerTimeoutSeconds = 6.0;
    constexpr double kOfferPayloadTimeoutSeconds = 300.0;
    constexpr std::size_t kMaxUdpPacketBytes = 1400;
    constexpr std::size_t kMaxControlLineBytes = 512;
    constexpr std::size_t kMaxPackageBytes = 20u * 1024u * 1024u;

    NativeSocket ToNativeSocket(SocketHandle socketHandle) {
        return static_cast<NativeSocket>(socketHandle);
    }

    SocketHandle ToSocketHandle(NativeSocket socketHandle) {
        return static_cast<SocketHandle>(socketHandle);
    }

    bool IsValidSocket(SocketHandle socketHandle) {
        return ToNativeSocket(socketHandle) != kInvalidSocket;
    }

    void CloseNativeSocket(NativeSocket socketHandle) {
        if (socketHandle == kInvalidSocket) {
            return;
        }
#if defined(_WIN32)
        closesocket(socketHandle);
#else
        close(socketHandle);
#endif
    }

    void CloseSocketHandle(SocketHandle* socketHandle) {
        if (socketHandle == nullptr || !IsValidSocket(*socketHandle)) {
            return;
        }
        CloseNativeSocket(ToNativeSocket(*socketHandle));
        *socketHandle = kInvalidSocketHandle;
    }

    template <typename ValueType>
    int SetSocketOption(NativeSocket socketHandle, int level, int optionName, const ValueType& value) {
#if defined(_WIN32)
        return setsockopt(socketHandle,
                          level,
                          optionName,
                          reinterpret_cast<const char*>(&value),
                          static_cast<int>(sizeof(ValueType)));
#else
        return setsockopt(socketHandle,
                          level,
                          optionName,
                          &value,
                          static_cast<socklen_t>(sizeof(ValueType)));
#endif
    }

    double NowSeconds() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration<double>(now).count();
    }

    std::string SanitizeField(std::string value) {
        for (char& c : value) {
            if (c == '|' || c == '\n' || c == '\r') {
                c = '_';
            }
        }
        return value;
    }

    std::string HostnameOrDefault() {
        std::array<char, 256> buffer{};
        if (gethostname(buffer.data(), buffer.size() - 1) == 0 && buffer[0] != '\0') {
            return SanitizeField(buffer.data());
        }
        return "JITGL-PC";
    }

    std::string GeneratePeerId() {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        const uint64_t randomPart = gen();
        const double now = NowSeconds();

        std::string id = HostnameOrDefault();
        id += "-";
        id += std::to_string(static_cast<uint64_t>(now * 1000.0));
        id += "-";
        id += std::to_string(randomPart);
        return SanitizeField(id);
    }

    std::vector<std::string> SplitPipeDelimited(const std::string& value) {
        std::vector<std::string> tokens;
        std::size_t start = 0;
        while (start <= value.size()) {
            const std::size_t separator = value.find('|', start);
            if (separator == std::string::npos) {
                tokens.push_back(value.substr(start));
                break;
            }
            tokens.push_back(value.substr(start, separator - start));
            start = separator + 1;
        }
        return tokens;
    }

    bool SendAll(NativeSocket socketFd, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const std::size_t chunkSize = std::min<std::size_t>(
                size - sent,
                static_cast<std::size_t>(std::numeric_limits<int>::max()));
            const int wrote = send(socketFd, data + sent, static_cast<int>(chunkSize), 0);
            if (wrote <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(wrote);
        }
        return true;
    }

    bool RecvAll(NativeSocket socketFd, char* data, std::size_t size) {
        std::size_t received = 0;
        while (received < size) {
            const std::size_t chunkSize = std::min<std::size_t>(
                size - received,
                static_cast<std::size_t>(std::numeric_limits<int>::max()));
            const int got = recv(socketFd, data + received, static_cast<int>(chunkSize), 0);
            if (got <= 0) {
                return false;
            }
            received += static_cast<std::size_t>(got);
        }
        return true;
    }

    bool RecvLine(NativeSocket socketFd, std::string* lineOut, std::size_t maxBytes) {
        if (lineOut == nullptr) {
            return false;
        }
        lineOut->clear();

        char ch = '\0';
        while (lineOut->size() < maxBytes) {
            const int got = recv(socketFd, &ch, 1, 0);
            if (got <= 0) {
                return false;
            }
            if (ch == '\n') {
                return true;
            }
            if (ch != '\r') {
                lineOut->push_back(ch);
            }
        }
        return false;
    }

    bool ParsePort(const std::string& value, uint16_t* portOut) {
        if (portOut == nullptr || value.empty()) {
            return false;
        }
        unsigned int parsed = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size() || parsed == 0 || parsed > 65535u) {
            return false;
        }
        *portOut = static_cast<uint16_t>(parsed);
        return true;
    }

    bool ParseSize(const std::string& value, std::size_t* sizeOut) {
        if (sizeOut == nullptr || value.empty()) {
            return false;
        }
        std::size_t parsed = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (ec != std::errc{} || ptr != value.data() + value.size()) {
            return false;
        }
        *sizeOut = parsed;
        return true;
    }

    SocketHandle CreateUdpDiscoverySocket() {
        const NativeSocket sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == kInvalidSocket) {
            return kInvalidSocketHandle;
        }

        int enable = 1;
        (void)SetSocketOption(sock, SOL_SOCKET, SO_REUSEADDR, enable);
        (void)SetSocketOption(sock, SOL_SOCKET, SO_BROADCAST, enable);

        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_port = htons(kDiscoveryPort);
        bindAddress.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddress), static_cast<SocketLen>(sizeof(bindAddress))) != 0) {
            CloseNativeSocket(sock);
            return kInvalidSocketHandle;
        }

        return ToSocketHandle(sock);
    }

    SocketHandle CreateTcpListenSocket(uint16_t* boundPortOut) {
        if (boundPortOut == nullptr) {
            return kInvalidSocketHandle;
        }

        const NativeSocket sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kInvalidSocket) {
            return kInvalidSocketHandle;
        }

        int enable = 1;
        (void)SetSocketOption(sock, SOL_SOCKET, SO_REUSEADDR, enable);

        auto bindOnPort = [&](uint16_t port) {
            sockaddr_in bindAddress{};
            bindAddress.sin_family = AF_INET;
            bindAddress.sin_port = htons(port);
            bindAddress.sin_addr.s_addr = INADDR_ANY;
            return bind(sock,
                        reinterpret_cast<sockaddr*>(&bindAddress),
                        static_cast<SocketLen>(sizeof(bindAddress))) == 0;
        };

        if (!bindOnPort(kPreferredTransferPort) && !bindOnPort(0)) {
            CloseNativeSocket(sock);
            return kInvalidSocketHandle;
        }

        if (listen(sock, 16) != 0) {
            CloseNativeSocket(sock);
            return kInvalidSocketHandle;
        }

        sockaddr_in boundAddress{};
        SocketLen boundAddressLen = static_cast<SocketLen>(sizeof(boundAddress));
        if (getsockname(sock, reinterpret_cast<sockaddr*>(&boundAddress), &boundAddressLen) != 0) {
            CloseNativeSocket(sock);
            return kInvalidSocketHandle;
        }
        *boundPortOut = ntohs(boundAddress.sin_port);
        if (*boundPortOut == 0) {
            CloseNativeSocket(sock);
            return kInvalidSocketHandle;
        }

        return ToSocketHandle(sock);
    }

    bool SendUdpPacket(SocketHandle udpSocket,
                       const std::string& payload,
                       const std::string& destinationIpAddress,
                       bool broadcast) {
        if (!IsValidSocket(udpSocket) || payload.empty() || payload.size() > kMaxUdpPacketBytes ||
            payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        const NativeSocket socketFd = ToNativeSocket(udpSocket);

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(kDiscoveryPort);

        if (broadcast) {
            destination.sin_addr.s_addr = INADDR_BROADCAST;
        } else if (inet_pton(AF_INET, destinationIpAddress.c_str(), &destination.sin_addr) != 1) {
            return false;
        }

        const int sent = sendto(socketFd,
                                payload.data(),
                                static_cast<int>(payload.size()),
                                0,
                                reinterpret_cast<sockaddr*>(&destination),
                                static_cast<SocketLen>(sizeof(destination)));
        return sent == static_cast<int>(payload.size());
    }

    void ConfigureSocketTimeouts(SocketHandle socketFd, int timeoutMs) {
        if (!IsValidSocket(socketFd)) {
            return;
        }
        const NativeSocket nativeSocket = ToNativeSocket(socketFd);
#if defined(_WIN32)
        const DWORD timeoutMsValue = static_cast<DWORD>(std::max(0, timeoutMs));
        (void)SetSocketOption(nativeSocket, SOL_SOCKET, SO_RCVTIMEO, timeoutMsValue);
        (void)SetSocketOption(nativeSocket, SOL_SOCKET, SO_SNDTIMEO, timeoutMsValue);
#else
        timeval timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        (void)SetSocketOption(nativeSocket, SOL_SOCKET, SO_RCVTIMEO, timeout);
        (void)SetSocketOption(nativeSocket, SOL_SOCKET, SO_SNDTIMEO, timeout);
#endif
    }

#if defined(_WIN32)
    bool StartupNetworkStack() {
        WSADATA wsaData{};
        return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }

    void CleanupNetworkStack() {
        WSACleanup();
    }
#endif
}

LanWorkspaceShareService::LanWorkspaceShareService()
    : localPeerId_(GeneratePeerId()),
      localDisplayName_(HostnameOrDefault()) {}

LanWorkspaceShareService::~LanWorkspaceShareService() {
    Stop();
}

bool LanWorkspaceShareService::Start() {
    if (running_.load()) {
        return true;
    }

#if defined(_WIN32)
    const bool startedWinsockThisCall = !winsockInitialized_;
    if (startedWinsockThisCall) {
        if (!StartupNetworkStack()) {
            return false;
        }
        winsockInitialized_ = true;
    }
    localDisplayName_ = HostnameOrDefault();
#endif

    {
        std::scoped_lock lock(stateMutex_);
        peersById_.clear();
        seenIncomingOffers_.clear();
        incomingOffers_.clear();
        pendingUdpPackets_.clear();
        sharedPayloadsByOfferId_.clear();
    }
    offerCounter_.store(1);
    activeClientHandlers_.store(0);
    localTransferPort_.store(0);

    udpSocket_ = CreateUdpDiscoverySocket();
    if (!IsValidSocket(udpSocket_)) {
#if defined(_WIN32)
        if (startedWinsockThisCall) {
            CleanupNetworkStack();
            winsockInitialized_ = false;
        }
#endif
        return false;
    }

    uint16_t boundTransferPort = 0;
    tcpListenSocket_ = CreateTcpListenSocket(&boundTransferPort);
    if (!IsValidSocket(tcpListenSocket_)) {
        CloseSocketHandle(&udpSocket_);
#if defined(_WIN32)
        if (startedWinsockThisCall) {
            CleanupNetworkStack();
            winsockInitialized_ = false;
        }
#endif
        return false;
    }
    localTransferPort_.store(boundTransferPort);

    running_.store(true);
    networkThread_ = std::jthread([this](std::stop_token) { NetworkLoop(); });
    return true;
}

void LanWorkspaceShareService::Stop() {
    if (!running_.exchange(false)) {
#if defined(_WIN32)
        if (winsockInitialized_) {
            CleanupNetworkStack();
            winsockInitialized_ = false;
        }
#endif
        return;
    }

    if (networkThread_.joinable()) {
        networkThread_.join();
    }

    CloseSocketHandle(&udpSocket_);
    CloseSocketHandle(&tcpListenSocket_);
    localTransferPort_.store(0);

    std::unique_lock<std::mutex> lock(clientHandlerMutex_);
    clientHandlerCv_.wait(lock, [this] {
        return activeClientHandlers_.load() == 0;
    });
    lock.unlock();

    std::scoped_lock stateLock(stateMutex_);
    peersById_.clear();
    seenIncomingOffers_.clear();
    incomingOffers_.clear();
    pendingUdpPackets_.clear();
    sharedPayloadsByOfferId_.clear();

#if defined(_WIN32)
    if (winsockInitialized_) {
        CleanupNetworkStack();
        winsockInitialized_ = false;
    }
#endif
}

std::string LanWorkspaceShareService::LocalDisplayName() const {
    return localDisplayName_;
}

std::vector<LanPeerInfo> LanWorkspaceShareService::SnapshotPeers() const {
    std::scoped_lock lock(stateMutex_);
    std::vector<LanPeerInfo> peers;
    peers.reserve(peersById_.size());
    for (const auto& [id, peer] : peersById_) {
        (void)id;
        peers.push_back(peer);
    }
    std::ranges::sort(peers, [](const LanPeerInfo& lhs, const LanPeerInfo& rhs) {
        if (lhs.displayName != rhs.displayName) {
            return lhs.displayName < rhs.displayName;
        }
        return lhs.id < rhs.id;
    });
    return peers;
}

std::vector<LanWorkspaceOffer> LanWorkspaceShareService::DrainIncomingOffers() {
    std::scoped_lock lock(stateMutex_);
    std::vector<LanWorkspaceOffer> offers;
    offers.swap(incomingOffers_);
    return offers;
}

bool LanWorkspaceShareService::ShareWorkspacePackage(const std::string& workspaceName,
                                                     const std::string& packageData,
                                                     const std::vector<std::string>& targetPeerIds,
                                                     bool shareToAll) {
    const uint16_t transferPort = localTransferPort_.load();
    if (!running_.load() || workspaceName.empty() || packageData.empty() || transferPort == 0) {
        return false;
    }

    const std::string safeWorkspaceName = SanitizeField(workspaceName);
    const std::string offerId = localPeerId_ + "-" + std::to_string(offerCounter_.fetch_add(1));

    OutboundUdpPacket broadcastPacket;
    std::vector<OutboundUdpPacket> targetedPackets;
    {
        std::scoped_lock lock(stateMutex_);
        sharedPayloadsByOfferId_[offerId] = SharedOfferPayload{ packageData, NowSeconds() };

        const std::string message = std::string(kProtocolPrefix) + "|OFFER|" + offerId + "|" +
                                    localPeerId_ + "|" + SanitizeField(localDisplayName_) + "|" +
                                    safeWorkspaceName + "|" + std::to_string(transferPort);

        if (shareToAll) {
            broadcastPacket = OutboundUdpPacket{ message, "", true };
            pendingUdpPackets_.push_back(std::move(broadcastPacket));
            return true;
        }

        for (const auto& peerId : targetPeerIds) {
            if (auto peerIt = peersById_.find(peerId); peerIt != peersById_.end()) {
                targetedPackets.push_back(OutboundUdpPacket{ message, peerIt->second.ipAddress, false });
            }
        }

        if (targetedPackets.empty()) {
            sharedPayloadsByOfferId_.erase(offerId);
            return false;
        }

        pendingUdpPackets_.insert(pendingUdpPackets_.end(), targetedPackets.begin(), targetedPackets.end());
    }

    return true;
}

std::optional<std::string> LanWorkspaceShareService::FetchWorkspacePackage(const LanWorkspaceOffer& offer) const {
    if (offer.offerId.empty() || offer.senderIpAddress.empty() || offer.transferPort == 0) {
        return std::nullopt;
    }

    const NativeSocket nativeSock = socket(AF_INET, SOCK_STREAM, 0);
    if (nativeSock == kInvalidSocket) {
        return std::nullopt;
    }
    SocketHandle sock = ToSocketHandle(nativeSock);
    ConfigureSocketTimeouts(sock, 4000);

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(offer.transferPort);
    if (inet_pton(AF_INET, offer.senderIpAddress.c_str(), &serverAddress.sin_addr) != 1) {
        CloseSocketHandle(&sock);
        return std::nullopt;
    }

    if (connect(ToNativeSocket(sock),
                reinterpret_cast<sockaddr*>(&serverAddress),
                static_cast<SocketLen>(sizeof(serverAddress))) != 0) {
        CloseSocketHandle(&sock);
        return std::nullopt;
    }

    const std::string request = std::string(kProtocolPrefix) + "|GET|" + offer.offerId + "\n";
    if (!SendAll(ToNativeSocket(sock), request.data(), request.size())) {
        CloseSocketHandle(&sock);
        return std::nullopt;
    }

    std::string responseLine;
    if (!RecvLine(ToNativeSocket(sock), &responseLine, kMaxControlLineBytes)) {
        CloseSocketHandle(&sock);
        return std::nullopt;
    }

    const std::vector<std::string> response = SplitPipeDelimited(responseLine);
    if (response.size() < 2 || response[0] != kProtocolPrefix || response[1] != "OK") {
        CloseSocketHandle(&sock);
        return std::nullopt;
    }
    if (response.size() != 3) {
        CloseSocketHandle(&sock);
        return std::nullopt;
    }

    std::size_t payloadSize = 0;
    if (!ParseSize(response[2], &payloadSize) || payloadSize == 0 || payloadSize > kMaxPackageBytes) {
        CloseSocketHandle(&sock);
        return std::nullopt;
    }

    std::string payload;
    payload.resize(payloadSize);
    if (!RecvAll(ToNativeSocket(sock), payload.data(), payloadSize)) {
        CloseSocketHandle(&sock);
        return std::nullopt;
    }

    CloseSocketHandle(&sock);
    return payload;
}

void LanWorkspaceShareService::NetworkLoop() {
    double nextHelloAt = 0.0;

    while (running_.load()) {
        const double now = NowSeconds();
        if (now >= nextHelloAt) {
            SendHelloBroadcast();
            nextHelloAt = now + kHelloIntervalSeconds;
        }

        SendPendingUdpPackets();
        PruneExpiredState(now);

        fd_set readSet;
        FD_ZERO(&readSet);
#if !defined(_WIN32)
        int maxFd = -1;
#endif
        if (IsValidSocket(udpSocket_)) {
            FD_SET(ToNativeSocket(udpSocket_), &readSet);
#if !defined(_WIN32)
            maxFd = std::max(maxFd, static_cast<int>(ToNativeSocket(udpSocket_)));
#endif
        }
        if (IsValidSocket(tcpListenSocket_)) {
            FD_SET(ToNativeSocket(tcpListenSocket_), &readSet);
#if !defined(_WIN32)
            maxFd = std::max(maxFd, static_cast<int>(ToNativeSocket(tcpListenSocket_)));
#endif
        }

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;
#if defined(_WIN32)
        const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
        const int ready = select(maxFd + 1, &readSet, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) {
            continue;
        }

        if (IsValidSocket(udpSocket_) && FD_ISSET(ToNativeSocket(udpSocket_), &readSet)) {
            std::array<char, kMaxUdpPacketBytes> buffer{};
            sockaddr_in from{};
            SocketLen fromLen = static_cast<SocketLen>(sizeof(from));
            const int bytes = recvfrom(ToNativeSocket(udpSocket_),
                                       buffer.data(),
                                       static_cast<int>(buffer.size() - 1),
                                       0,
                                       reinterpret_cast<sockaddr*>(&from),
                                       &fromLen);
            if (bytes > 0) {
                buffer[static_cast<std::size_t>(bytes)] = '\0';
                std::array<char, INET_ADDRSTRLEN> ipBuffer{};
                if (inet_ntop(AF_INET, &from.sin_addr, ipBuffer.data(), ipBuffer.size()) != nullptr) {
                    HandleUdpPacket(buffer.data(), ipBuffer.data(), now);
                }
            }
        }

        if (IsValidSocket(tcpListenSocket_) && FD_ISSET(ToNativeSocket(tcpListenSocket_), &readSet)) {
            sockaddr_in from{};
            SocketLen fromLen = static_cast<SocketLen>(sizeof(from));
            const NativeSocket client = accept(ToNativeSocket(tcpListenSocket_),
                                               reinterpret_cast<sockaddr*>(&from),
                                               &fromLen);
            if (client != kInvalidSocket) {
                activeClientHandlers_.fetch_add(1);
                std::thread([this, clientHandle = ToSocketHandle(client)] {
                    HandleClientConnection(clientHandle);
                    if (activeClientHandlers_.fetch_sub(1) == 1) {
                        clientHandlerCv_.notify_all();
                    }
                }).detach();
            }
        }
    }
}

void LanWorkspaceShareService::HandleUdpPacket(const std::string& payload,
                                               const std::string& senderIpAddress,
                                               double nowSeconds) {
    const std::vector<std::string> fields = SplitPipeDelimited(payload);
    if (fields.size() < 2 || fields[0] != kProtocolPrefix) {
        return;
    }

    if (fields[1] == "HELLO") {
        if (fields.size() != 5) {
            return;
        }
        uint16_t transferPort = 0;
        if (!ParsePort(fields[4], &transferPort) || fields[2].empty() || fields[2] == localPeerId_) {
            return;
        }

        std::scoped_lock lock(stateMutex_);
        LanPeerInfo& peer = peersById_[fields[2]];
        peer.id = fields[2];
        peer.displayName = fields[3].empty() ? senderIpAddress : fields[3];
        peer.ipAddress = senderIpAddress;
        peer.transferPort = transferPort;
        peer.lastSeenSeconds = nowSeconds;
        return;
    }

    if (fields[1] == "OFFER") {
        if (fields.size() != 7) {
            return;
        }

        uint16_t transferPort = 0;
        if (!ParsePort(fields[6], &transferPort) || fields[2].empty() || fields[3].empty() || fields[3] == localPeerId_) {
            return;
        }

        std::scoped_lock lock(stateMutex_);
        LanPeerInfo& peer = peersById_[fields[3]];
        peer.id = fields[3];
        peer.displayName = fields[4].empty() ? senderIpAddress : fields[4];
        peer.ipAddress = senderIpAddress;
        peer.transferPort = transferPort;
        peer.lastSeenSeconds = nowSeconds;

        if (seenIncomingOffers_.contains(fields[2])) {
            return;
        }
        seenIncomingOffers_[fields[2]] = nowSeconds;

        incomingOffers_.push_back(LanWorkspaceOffer{
            fields[2],
            fields[3],
            fields[4].empty() ? senderIpAddress : fields[4],
            senderIpAddress,
            fields[5],
            transferPort
        });
    }
}

void LanWorkspaceShareService::HandleClientConnection(SocketHandle clientSocket) {
    ConfigureSocketTimeouts(clientSocket, 4000);
    const NativeSocket nativeClientSocket = ToNativeSocket(clientSocket);

    std::string requestLine;
    if (!RecvLine(nativeClientSocket, &requestLine, kMaxControlLineBytes)) {
        CloseSocketHandle(&clientSocket);
        return;
    }

    const std::vector<std::string> fields = SplitPipeDelimited(requestLine);
    if (fields.size() != 3 || fields[0] != kProtocolPrefix || fields[1] != "GET") {
        static constexpr std::string_view kBadRequest = "JITGL_LAN_V1|ERR|bad_request\n";
        (void)SendAll(nativeClientSocket, kBadRequest.data(), kBadRequest.size());
        CloseSocketHandle(&clientSocket);
        return;
    }

    std::string payload;
    {
        std::scoped_lock lock(stateMutex_);
        if (auto payloadIt = sharedPayloadsByOfferId_.find(fields[2]); payloadIt != sharedPayloadsByOfferId_.end()) {
            payload = payloadIt->second.packageData;
        }
    }

    if (payload.empty()) {
        static constexpr std::string_view kMissing = "JITGL_LAN_V1|ERR|not_found\n";
        (void)SendAll(nativeClientSocket, kMissing.data(), kMissing.size());
        CloseSocketHandle(&clientSocket);
        return;
    }

    const std::string header = std::string(kProtocolPrefix) + "|OK|" + std::to_string(payload.size()) + "\n";
    if (!SendAll(nativeClientSocket, header.data(), header.size()) ||
        !SendAll(nativeClientSocket, payload.data(), payload.size())) {
        CloseSocketHandle(&clientSocket);
        return;
    }

    CloseSocketHandle(&clientSocket);
}

void LanWorkspaceShareService::SendPendingUdpPackets() {
    std::vector<OutboundUdpPacket> packets;
    {
        std::scoped_lock lock(stateMutex_);
        packets.swap(pendingUdpPackets_);
    }

    for (const auto& packet : packets) {
        (void)SendUdpPacket(udpSocket_, packet.payload, packet.destinationIpAddress, packet.broadcast);
    }
}

void LanWorkspaceShareService::SendHelloBroadcast() {
    const uint16_t transferPort = localTransferPort_.load();
    if (transferPort == 0) {
        return;
    }
    const std::string message = std::string(kProtocolPrefix) + "|HELLO|" + localPeerId_ + "|" +
                                SanitizeField(localDisplayName_) + "|" + std::to_string(transferPort);
    (void)SendUdpPacket(udpSocket_, message, "", true);
}

void LanWorkspaceShareService::PruneExpiredState(double nowSeconds) {
    std::scoped_lock lock(stateMutex_);

    std::erase_if(peersById_,
                  [&](const auto& entry) {
                      return (nowSeconds - entry.second.lastSeenSeconds) > kPeerTimeoutSeconds;
                  });

    std::erase_if(seenIncomingOffers_,
                  [&](const auto& entry) {
                      return (nowSeconds - entry.second) > kOfferPayloadTimeoutSeconds;
                  });

    std::erase_if(sharedPayloadsByOfferId_,
                  [&](const auto& entry) {
                      return (nowSeconds - entry.second.createdSeconds) > kOfferPayloadTimeoutSeconds;
                  });
}
