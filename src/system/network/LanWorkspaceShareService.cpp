#include "system/network/LanWorkspaceShareService.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <limits>
#include <random>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
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
    constexpr std::string_view kDiscoveryMulticastAddress = "239.255.43.21";
    constexpr std::string_view kProtocolPrefix = "JITGL_LAN_V1";
    constexpr double kHelloIntervalSeconds = 1.5;
    constexpr double kPeerTimeoutSeconds = 6.0;
    constexpr double kOfferPayloadTimeoutSeconds = 300.0;
    constexpr std::size_t kMaxUdpPacketBytes = 1400;
    constexpr std::size_t kMaxControlLineBytes = 512;
    constexpr std::size_t kMaxPackageBytes = 20u * 1024u * 1024u;

    struct UdpDiscoverySocketResult {
        SocketHandle socket = kInvalidSocketHandle;
        bool multicastJoinAttempted = false;
        bool multicastJoinSucceeded = false;
        std::string error;
    };

    struct TcpListenSocketResult {
        SocketHandle socket = kInvalidSocketHandle;
        uint16_t boundPort = 0;
        std::string error;
    };

    struct InterfaceDiscoveryResult {
        std::vector<std::string> localIpv4Addresses;
        std::vector<std::string> directedBroadcastAddresses;
    };

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

    std::string IPv4ToString(in_addr address) {
        std::array<char, INET_ADDRSTRLEN> buffer{};
        if (inet_ntop(AF_INET, &address, buffer.data(), static_cast<SocketLen>(buffer.size())) == nullptr) {
            return {};
        }
        return std::string(buffer.data());
    }

    InterfaceDiscoveryResult DiscoverInterfaceTargets() {
        InterfaceDiscoveryResult result;
        std::unordered_set<std::string> seenLocal;
        std::unordered_set<std::string> seenBroadcast;

#if defined(_WIN32)
        ULONG adapterBufferSize = 16u * 1024u;
        std::vector<unsigned char> adapterBuffer(adapterBufferSize);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapterBuffer.data());
        ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &adapterBufferSize);
        if (status == ERROR_BUFFER_OVERFLOW) {
            adapterBuffer.resize(adapterBufferSize);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapterBuffer.data());
            status = GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &adapterBufferSize);
        }

        if (status == NO_ERROR) {
            for (IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
                if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                    continue;
                }
                for (IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
                     unicast != nullptr;
                     unicast = unicast->Next) {
                    if (unicast->Address.lpSockaddr == nullptr || unicast->Address.lpSockaddr->sa_family != AF_INET) {
                        continue;
                    }

                    const auto* sockaddrV4 = reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
                    const in_addr ipAddress = sockaddrV4->sin_addr;
                    const std::string localIp = IPv4ToString(ipAddress);
                    if (!localIp.empty() && seenLocal.insert(localIp).second) {
                        result.localIpv4Addresses.push_back(localIp);
                    }

                    const ULONG prefixLength = std::min<ULONG>(unicast->OnLinkPrefixLength, 32u);
                    const std::uint32_t ipHost = ntohl(ipAddress.s_addr);
                    const std::uint32_t maskHost = (prefixLength == 0) ? 0u : (0xFFFFFFFFu << (32u - prefixLength));
                    const std::uint32_t broadcastHost = (ipHost & maskHost) | (~maskHost);
                    in_addr broadcastAddress{};
                    broadcastAddress.s_addr = htonl(broadcastHost);
                    const std::string broadcastIp = IPv4ToString(broadcastAddress);
                    if (!broadcastIp.empty() &&
                        broadcastIp != "255.255.255.255" &&
                        seenBroadcast.insert(broadcastIp).second) {
                        result.directedBroadcastAddresses.push_back(broadcastIp);
                    }
                }
            }
        }
#else
        ifaddrs* interfaces = nullptr;
        if (getifaddrs(&interfaces) == 0 && interfaces != nullptr) {
            for (ifaddrs* entry = interfaces; entry != nullptr; entry = entry->ifa_next) {
                if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) {
                    continue;
                }
                const unsigned int flags = entry->ifa_flags;
                if ((flags & IFF_UP) == 0 || (flags & IFF_LOOPBACK) != 0) {
                    continue;
                }

                const auto* address = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
                const in_addr ipAddress = address->sin_addr;
                const std::string localIp = IPv4ToString(ipAddress);
                if (!localIp.empty() && seenLocal.insert(localIp).second) {
                    result.localIpv4Addresses.push_back(localIp);
                }

                in_addr broadcastAddress{};
                bool hasBroadcast = false;
                if ((flags & IFF_BROADCAST) != 0 &&
                    entry->ifa_broadaddr != nullptr &&
                    entry->ifa_broadaddr->sa_family == AF_INET) {
                    const auto* broadcastSockAddr = reinterpret_cast<const sockaddr_in*>(entry->ifa_broadaddr);
                    broadcastAddress = broadcastSockAddr->sin_addr;
                    hasBroadcast = true;
                } else if (entry->ifa_netmask != nullptr && entry->ifa_netmask->sa_family == AF_INET) {
                    const auto* maskSockAddr = reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask);
                    const std::uint32_t ipHost = ntohl(ipAddress.s_addr);
                    const std::uint32_t maskHost = ntohl(maskSockAddr->sin_addr.s_addr);
                    const std::uint32_t broadcastHost = (ipHost & maskHost) | (~maskHost);
                    broadcastAddress.s_addr = htonl(broadcastHost);
                    hasBroadcast = true;
                }

                if (hasBroadcast) {
                    const std::string broadcastIp = IPv4ToString(broadcastAddress);
                    if (!broadcastIp.empty() &&
                        broadcastIp != "255.255.255.255" &&
                        seenBroadcast.insert(broadcastIp).second) {
                        result.directedBroadcastAddresses.push_back(broadcastIp);
                    }
                }
            }
            freeifaddrs(interfaces);
        }
#endif

        std::ranges::sort(result.localIpv4Addresses);
        std::ranges::sort(result.directedBroadcastAddresses);
        return result;
    }

    UdpDiscoverySocketResult CreateUdpDiscoverySocket() {
        UdpDiscoverySocketResult result;
        const NativeSocket sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock == kInvalidSocket) {
            result.error = "Failed to create UDP discovery socket.";
            return result;
        }

        int enable = 1;
        (void)SetSocketOption(sock, SOL_SOCKET, SO_REUSEADDR, enable);
#if defined(SO_REUSEPORT)
        (void)SetSocketOption(sock, SOL_SOCKET, SO_REUSEPORT, enable);
#endif
        (void)SetSocketOption(sock, SOL_SOCKET, SO_BROADCAST, enable);

        sockaddr_in bindAddress{};
        bindAddress.sin_family = AF_INET;
        bindAddress.sin_port = htons(kDiscoveryPort);
        bindAddress.sin_addr.s_addr = INADDR_ANY;
        if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddress), static_cast<SocketLen>(sizeof(bindAddress))) != 0) {
            CloseNativeSocket(sock);
            result.error = "Failed to bind UDP discovery socket on port " + std::to_string(kDiscoveryPort) + ".";
            return result;
        }

        ip_mreq multicastRequest{};
        if (inet_pton(AF_INET, kDiscoveryMulticastAddress.data(), &multicastRequest.imr_multiaddr) == 1) {
            result.multicastJoinAttempted = true;
            multicastRequest.imr_interface.s_addr = htonl(INADDR_ANY);
            if (SetSocketOption(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, multicastRequest) == 0) {
                result.multicastJoinSucceeded = true;
            }

            unsigned char multicastLoopback = 1;
            (void)SetSocketOption(sock, IPPROTO_IP, IP_MULTICAST_LOOP, multicastLoopback);

            unsigned char multicastTtl = 1;
            (void)SetSocketOption(sock, IPPROTO_IP, IP_MULTICAST_TTL, multicastTtl);
        }

        result.socket = ToSocketHandle(sock);
        return result;
    }

    TcpListenSocketResult CreateTcpListenSocket() {
        TcpListenSocketResult result;
        const NativeSocket sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kInvalidSocket) {
            result.error = "Failed to create TCP transfer socket.";
            return result;
        }

        int enable = 1;
        (void)SetSocketOption(sock, SOL_SOCKET, SO_REUSEADDR, enable);
#if defined(SO_REUSEPORT)
        (void)SetSocketOption(sock, SOL_SOCKET, SO_REUSEPORT, enable);
#endif

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
            result.error = "Failed to bind TCP transfer socket.";
            return result;
        }

        if (listen(sock, 16) != 0) {
            CloseNativeSocket(sock);
            result.error = "Failed to listen on TCP transfer socket.";
            return result;
        }

        sockaddr_in boundAddress{};
        SocketLen boundAddressLen = static_cast<SocketLen>(sizeof(boundAddress));
        if (getsockname(sock, reinterpret_cast<sockaddr*>(&boundAddress), &boundAddressLen) != 0) {
            CloseNativeSocket(sock);
            result.error = "Failed to query bound TCP transfer port.";
            return result;
        }
        result.boundPort = ntohs(boundAddress.sin_port);
        if (result.boundPort == 0) {
            CloseNativeSocket(sock);
            result.error = "TCP transfer port resolved to zero.";
            return result;
        }

        result.socket = ToSocketHandle(sock);
        return result;
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
      localDisplayName_(HostnameOrDefault()) {
    std::scoped_lock lock(stateMutex_);
    diagnostics_.serviceRunning = false;
    diagnostics_.discoveryPort = kDiscoveryPort;
    diagnostics_.transferPort = 0;
    diagnostics_.localPeerId = localPeerId_;
    diagnostics_.localDisplayName = localDisplayName_;
    diagnostics_.discoveryMulticastAddress = std::string(kDiscoveryMulticastAddress);
    diagnostics_.nowSeconds = NowSeconds();
}

LanWorkspaceShareService::~LanWorkspaceShareService() {
    Stop();
}

bool LanWorkspaceShareService::Start() {
    if (running_.load()) {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.nowSeconds = NowSeconds();
        return true;
    }

#if defined(_WIN32)
    const bool startedWinsockThisCall = !winsockInitialized_;
    if (startedWinsockThisCall) {
        if (!StartupNetworkStack()) {
            std::scoped_lock lock(stateMutex_);
            diagnostics_.lastError = "WSAStartup failed.";
            diagnostics_.winsockInitialized = false;
            diagnostics_.nowSeconds = NowSeconds();
            return false;
        }
        winsockInitialized_ = true;
    }
    localDisplayName_ = HostnameOrDefault();
#endif

    const InterfaceDiscoveryResult interfaces = DiscoverInterfaceTargets();

    {
        std::scoped_lock lock(stateMutex_);
        diagnostics_ = LanNetworkDiagnostics{};
        diagnostics_.serviceRunning = false;
        diagnostics_.discoveryPort = kDiscoveryPort;
        diagnostics_.transferPort = 0;
        diagnostics_.localPeerId = localPeerId_;
        diagnostics_.localDisplayName = localDisplayName_;
        diagnostics_.discoveryMulticastAddress = std::string(kDiscoveryMulticastAddress);
        localIpv4Addresses_ = interfaces.localIpv4Addresses;
        directedBroadcastTargets_ = interfaces.directedBroadcastAddresses;
        diagnostics_.localIpv4Addresses = localIpv4Addresses_;
        diagnostics_.directedBroadcastAddresses = directedBroadcastTargets_;
#if defined(_WIN32)
        diagnostics_.winsockInitialized = winsockInitialized_;
#endif
        diagnostics_.nowSeconds = NowSeconds();

        peersById_.clear();
        seenIncomingOffers_.clear();
        incomingOffers_.clear();
        pendingUdpPackets_.clear();
        sharedPayloadsByOfferId_.clear();
    }
    offerCounter_.store(1);
    activeClientHandlers_.store(0);
    localTransferPort_.store(0);

    const UdpDiscoverySocketResult udpResult = CreateUdpDiscoverySocket();
    udpSocket_ = udpResult.socket;
    {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.multicastJoinAttempted = udpResult.multicastJoinAttempted;
        diagnostics_.multicastJoinSucceeded = udpResult.multicastJoinSucceeded;
        diagnostics_.udpSocketBound = IsValidSocket(udpSocket_);
        diagnostics_.nowSeconds = NowSeconds();
    }
    if (!IsValidSocket(udpSocket_)) {
#if defined(_WIN32)
        if (startedWinsockThisCall) {
            CleanupNetworkStack();
            winsockInitialized_ = false;
        }
#endif
        std::scoped_lock lock(stateMutex_);
        diagnostics_.lastError = udpResult.error.empty() ? "UDP discovery initialization failed." : udpResult.error;
#if defined(_WIN32)
        diagnostics_.winsockInitialized = winsockInitialized_;
#endif
        diagnostics_.nowSeconds = NowSeconds();
        return false;
    }

    const TcpListenSocketResult tcpResult = CreateTcpListenSocket();
    tcpListenSocket_ = tcpResult.socket;
    if (!IsValidSocket(tcpListenSocket_)) {
        CloseSocketHandle(&udpSocket_);
#if defined(_WIN32)
        if (startedWinsockThisCall) {
            CleanupNetworkStack();
            winsockInitialized_ = false;
        }
#endif
        std::scoped_lock lock(stateMutex_);
        diagnostics_.udpSocketBound = false;
        diagnostics_.tcpSocketBound = false;
        diagnostics_.transferPort = 0;
        diagnostics_.lastError = tcpResult.error.empty() ? "TCP transfer initialization failed." : tcpResult.error;
#if defined(_WIN32)
        diagnostics_.winsockInitialized = winsockInitialized_;
#endif
        diagnostics_.nowSeconds = NowSeconds();
        return false;
    }
    localTransferPort_.store(tcpResult.boundPort);

    running_.store(true);
    networkThread_ = std::jthread([this](std::stop_token) { NetworkLoop(); });
    {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.serviceRunning = true;
        diagnostics_.udpSocketBound = true;
        diagnostics_.tcpSocketBound = true;
        diagnostics_.transferPort = tcpResult.boundPort;
        diagnostics_.lastError.clear();
#if defined(_WIN32)
        diagnostics_.winsockInitialized = winsockInitialized_;
#endif
        diagnostics_.nowSeconds = NowSeconds();
    }
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
        std::scoped_lock lock(stateMutex_);
        diagnostics_.serviceRunning = false;
        diagnostics_.udpSocketBound = IsValidSocket(udpSocket_);
        diagnostics_.tcpSocketBound = IsValidSocket(tcpListenSocket_);
        diagnostics_.transferPort = localTransferPort_.load();
#if defined(_WIN32)
        diagnostics_.winsockInitialized = winsockInitialized_;
#endif
        diagnostics_.nowSeconds = NowSeconds();
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
    diagnostics_.serviceRunning = false;
    diagnostics_.udpSocketBound = false;
    diagnostics_.tcpSocketBound = false;
    diagnostics_.transferPort = 0;
    diagnostics_.peersKnown = 0;
    diagnostics_.pendingIncomingOffers = 0;
    diagnostics_.pendingOutgoingPackets = 0;
    diagnostics_.cachedSharedPayloads = 0;
    diagnostics_.nowSeconds = NowSeconds();

#if defined(_WIN32)
    if (winsockInitialized_) {
        CleanupNetworkStack();
        winsockInitialized_ = false;
    }
    diagnostics_.winsockInitialized = winsockInitialized_;
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
    diagnostics_.pendingIncomingOffers = incomingOffers_.size();
    diagnostics_.nowSeconds = NowSeconds();
    return offers;
}

LanNetworkDiagnostics LanWorkspaceShareService::SnapshotDiagnostics() const {
    std::scoped_lock lock(stateMutex_);
    LanNetworkDiagnostics snapshot = diagnostics_;
    snapshot.nowSeconds = NowSeconds();
    snapshot.serviceRunning = running_.load();
    snapshot.transferPort = localTransferPort_.load();
#if defined(_WIN32)
    snapshot.winsockInitialized = winsockInitialized_;
#endif
    snapshot.peersKnown = peersById_.size();
    snapshot.pendingIncomingOffers = incomingOffers_.size();
    snapshot.pendingOutgoingPackets = pendingUdpPackets_.size();
    snapshot.cachedSharedPayloads = sharedPayloadsByOfferId_.size();
    snapshot.localIpv4Addresses = localIpv4Addresses_;
    snapshot.directedBroadcastAddresses = directedBroadcastTargets_;
    return snapshot;
}

bool LanWorkspaceShareService::ShareWorkspacePackage(const std::string& workspaceName,
                                                     const std::string& packageData,
                                                     const std::vector<std::string>& targetPeerIds,
                                                     bool shareToAll) {
    const uint16_t transferPort = localTransferPort_.load();
    if (!running_.load() || workspaceName.empty() || packageData.empty() || transferPort == 0) {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.lastError = "Share request rejected: LAN service is not ready.";
        diagnostics_.nowSeconds = NowSeconds();
        return false;
    }

    const std::string safeWorkspaceName = SanitizeField(workspaceName);
    const std::string offerId = localPeerId_ + "-" + std::to_string(offerCounter_.fetch_add(1));

    OutboundUdpPacket broadcastPacket;
    std::vector<OutboundUdpPacket> targetedPackets;
    {
        std::scoped_lock lock(stateMutex_);
        sharedPayloadsByOfferId_[offerId] = SharedOfferPayload{ packageData, NowSeconds() };
        diagnostics_.offersSentCount += 1;

        const std::string message = std::string(kProtocolPrefix) + "|OFFER|" + offerId + "|" +
                                    localPeerId_ + "|" + SanitizeField(localDisplayName_) + "|" +
                                    safeWorkspaceName + "|" + std::to_string(transferPort);

        if (shareToAll) {
            std::unordered_set<std::string> uniqueTargets;
            broadcastPacket = OutboundUdpPacket{ message, "", true };
            pendingUdpPackets_.push_back(std::move(broadcastPacket));
            pendingUdpPackets_.push_back(OutboundUdpPacket{
                message,
                std::string(kDiscoveryMulticastAddress),
                false
            });
            for (const auto& directedBroadcastTarget : directedBroadcastTargets_) {
                if (directedBroadcastTarget.empty() || !uniqueTargets.insert(directedBroadcastTarget).second) {
                    continue;
                }
                pendingUdpPackets_.push_back(OutboundUdpPacket{
                    message,
                    directedBroadcastTarget,
                    false
                });
            }
            diagnostics_.pendingOutgoingPackets = pendingUdpPackets_.size();
            diagnostics_.cachedSharedPayloads = sharedPayloadsByOfferId_.size();
            diagnostics_.lastError.clear();
            diagnostics_.nowSeconds = NowSeconds();
            return true;
        }

        for (const auto& peerId : targetPeerIds) {
            if (auto peerIt = peersById_.find(peerId); peerIt != peersById_.end()) {
                targetedPackets.push_back(OutboundUdpPacket{ message, peerIt->second.ipAddress, false });
            }
        }

        if (targetedPackets.empty()) {
            sharedPayloadsByOfferId_.erase(offerId);
            if (diagnostics_.offersSentCount > 0) {
                diagnostics_.offersSentCount -= 1;
            }
            diagnostics_.cachedSharedPayloads = sharedPayloadsByOfferId_.size();
            diagnostics_.lastError = "Share request rejected: no reachable target peers matched selection.";
            diagnostics_.nowSeconds = NowSeconds();
            return false;
        }

        pendingUdpPackets_.insert(pendingUdpPackets_.end(), targetedPackets.begin(), targetedPackets.end());
        diagnostics_.pendingOutgoingPackets = pendingUdpPackets_.size();
        diagnostics_.cachedSharedPayloads = sharedPayloadsByOfferId_.size();
        diagnostics_.lastError.clear();
        diagnostics_.nowSeconds = NowSeconds();
    }

    return true;
}

std::optional<std::string> LanWorkspaceShareService::FetchWorkspacePackage(const LanWorkspaceOffer& offer) {
    {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchAttempts += 1;
        diagnostics_.nowSeconds = NowSeconds();
    }
    if (offer.offerId.empty() || offer.senderIpAddress.empty() || offer.transferPort == 0) {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: invalid offer metadata.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }

    const NativeSocket nativeSock = socket(AF_INET, SOCK_STREAM, 0);
    if (nativeSock == kInvalidSocket) {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: unable to create TCP socket.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }
    SocketHandle sock = ToSocketHandle(nativeSock);
    ConfigureSocketTimeouts(sock, 4000);

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(offer.transferPort);
    if (inet_pton(AF_INET, offer.senderIpAddress.c_str(), &serverAddress.sin_addr) != 1) {
        CloseSocketHandle(&sock);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: sender IP address is invalid.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }

    if (connect(ToNativeSocket(sock),
                reinterpret_cast<sockaddr*>(&serverAddress),
                static_cast<SocketLen>(sizeof(serverAddress))) != 0) {
        CloseSocketHandle(&sock);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: TCP connect to sender failed.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }

    const std::string request = std::string(kProtocolPrefix) + "|GET|" + offer.offerId + "\n";
    if (!SendAll(ToNativeSocket(sock), request.data(), request.size())) {
        CloseSocketHandle(&sock);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: request send failed.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }

    std::string responseLine;
    if (!RecvLine(ToNativeSocket(sock), &responseLine, kMaxControlLineBytes)) {
        CloseSocketHandle(&sock);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: no response from sender.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }

    const std::vector<std::string> response = SplitPipeDelimited(responseLine);
    if (response.size() < 2 || response[0] != kProtocolPrefix || response[1] != "OK") {
        CloseSocketHandle(&sock);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: sender returned error response.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }
    if (response.size() != 3) {
        CloseSocketHandle(&sock);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: malformed sender response.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }

    std::size_t payloadSize = 0;
    if (!ParseSize(response[2], &payloadSize) || payloadSize == 0 || payloadSize > kMaxPackageBytes) {
        CloseSocketHandle(&sock);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: payload size is invalid.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }

    std::string payload;
    payload.resize(payloadSize);
    if (!RecvAll(ToNativeSocket(sock), payload.data(), payloadSize)) {
        CloseSocketHandle(&sock);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchFailures += 1;
        diagnostics_.lastError = "Fetch failed: payload receive interrupted.";
        diagnostics_.nowSeconds = NowSeconds();
        return std::nullopt;
    }

    CloseSocketHandle(&sock);
    {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.outgoingFetchSuccesses += 1;
        diagnostics_.lastError.clear();
        diagnostics_.nowSeconds = NowSeconds();
    }
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
    {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.udpPacketsReceived += 1;
        diagnostics_.lastUdpReceivedSeconds = nowSeconds;
        diagnostics_.lastUdpSenderIp = senderIpAddress;
        diagnostics_.nowSeconds = nowSeconds;
    }

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
        diagnostics_.helloReceivedCount += 1;
        diagnostics_.lastHelloReceivedSeconds = nowSeconds;
        diagnostics_.peersKnown = peersById_.size();
        diagnostics_.nowSeconds = nowSeconds;
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
        diagnostics_.offersReceivedCount += 1;
        diagnostics_.pendingIncomingOffers = incomingOffers_.size();
        diagnostics_.peersKnown = peersById_.size();
        diagnostics_.nowSeconds = nowSeconds;
    }
}

void LanWorkspaceShareService::HandleClientConnection(SocketHandle clientSocket) {
    ConfigureSocketTimeouts(clientSocket, 4000);
    const NativeSocket nativeClientSocket = ToNativeSocket(clientSocket);
    {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.incomingTransferRequests += 1;
        diagnostics_.nowSeconds = NowSeconds();
    }

    std::string requestLine;
    if (!RecvLine(nativeClientSocket, &requestLine, kMaxControlLineBytes)) {
        CloseSocketHandle(&clientSocket);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.incomingTransferFailures += 1;
        diagnostics_.lastError = "Incoming transfer failed: request line receive error.";
        diagnostics_.nowSeconds = NowSeconds();
        return;
    }

    const std::vector<std::string> fields = SplitPipeDelimited(requestLine);
    if (fields.size() != 3 || fields[0] != kProtocolPrefix || fields[1] != "GET") {
        static constexpr std::string_view kBadRequest = "JITGL_LAN_V1|ERR|bad_request\n";
        (void)SendAll(nativeClientSocket, kBadRequest.data(), kBadRequest.size());
        CloseSocketHandle(&clientSocket);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.incomingTransferFailures += 1;
        diagnostics_.lastError = "Incoming transfer failed: malformed GET request.";
        diagnostics_.nowSeconds = NowSeconds();
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
        std::scoped_lock lock(stateMutex_);
        diagnostics_.incomingTransferFailures += 1;
        diagnostics_.lastError = "Incoming transfer failed: requested offer payload not found.";
        diagnostics_.nowSeconds = NowSeconds();
        return;
    }

    const std::string header = std::string(kProtocolPrefix) + "|OK|" + std::to_string(payload.size()) + "\n";
    if (!SendAll(nativeClientSocket, header.data(), header.size()) ||
        !SendAll(nativeClientSocket, payload.data(), payload.size())) {
        CloseSocketHandle(&clientSocket);
        std::scoped_lock lock(stateMutex_);
        diagnostics_.incomingTransferFailures += 1;
        diagnostics_.lastError = "Incoming transfer failed: payload send error.";
        diagnostics_.nowSeconds = NowSeconds();
        return;
    }

    CloseSocketHandle(&clientSocket);
    {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.incomingTransferSuccesses += 1;
        diagnostics_.lastError.clear();
        diagnostics_.nowSeconds = NowSeconds();
    }
}

void LanWorkspaceShareService::SendPendingUdpPackets() {
    std::vector<OutboundUdpPacket> packets;
    {
        std::scoped_lock lock(stateMutex_);
        packets.swap(pendingUdpPackets_);
        diagnostics_.pendingOutgoingPackets = pendingUdpPackets_.size();
        diagnostics_.nowSeconds = NowSeconds();
    }

    std::uint64_t sentCount = 0;
    std::uint64_t failedCount = 0;
    const double nowSeconds = NowSeconds();
    for (const auto& packet : packets) {
        if (SendUdpPacket(udpSocket_, packet.payload, packet.destinationIpAddress, packet.broadcast)) {
            sentCount += 1;
        } else {
            failedCount += 1;
        }
    }

    if (sentCount > 0 || failedCount > 0) {
        std::scoped_lock lock(stateMutex_);
        diagnostics_.udpPacketsSent += sentCount;
        diagnostics_.udpPacketsSendFailed += failedCount;
        if (sentCount > 0) {
            diagnostics_.lastUdpSentSeconds = nowSeconds;
        }
        if (failedCount > 0 && diagnostics_.lastError.empty()) {
            diagnostics_.lastError = "Some queued UDP packets failed to send.";
        }
        diagnostics_.nowSeconds = nowSeconds;
    }
}

void LanWorkspaceShareService::SendHelloBroadcast() {
    const uint16_t transferPort = localTransferPort_.load();
    if (transferPort == 0) {
        return;
    }
    const std::string message = std::string(kProtocolPrefix) + "|HELLO|" + localPeerId_ + "|" +
                                SanitizeField(localDisplayName_) + "|" + std::to_string(transferPort);
    std::uint64_t sentCount = 0;
    std::uint64_t failedCount = 0;

    if (SendUdpPacket(udpSocket_, message, "", true)) {
        sentCount += 1;
    } else {
        failedCount += 1;
    }
    if (SendUdpPacket(udpSocket_, message, std::string(kDiscoveryMulticastAddress), false)) {
        sentCount += 1;
    } else {
        failedCount += 1;
    }
    for (const auto& directedBroadcastTarget : directedBroadcastTargets_) {
        if (directedBroadcastTarget.empty()) {
            continue;
        }
        if (SendUdpPacket(udpSocket_, message, directedBroadcastTarget, false)) {
            sentCount += 1;
        } else {
            failedCount += 1;
        }
    }

    std::scoped_lock lock(stateMutex_);
    diagnostics_.udpPacketsSent += sentCount;
    diagnostics_.udpPacketsSendFailed += failedCount;
    if (sentCount > 0) {
        diagnostics_.helloSentCount += 1;
        diagnostics_.lastHelloSentSeconds = NowSeconds();
        diagnostics_.lastUdpSentSeconds = diagnostics_.lastHelloSentSeconds;
        diagnostics_.lastError.clear();
    } else {
        diagnostics_.lastError = "HELLO announcements failed to send.";
    }
    diagnostics_.nowSeconds = NowSeconds();
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
    diagnostics_.peersKnown = peersById_.size();
    diagnostics_.pendingIncomingOffers = incomingOffers_.size();
    diagnostics_.cachedSharedPayloads = sharedPayloadsByOfferId_.size();
    diagnostics_.nowSeconds = nowSeconds;
}
