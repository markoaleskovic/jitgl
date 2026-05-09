#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct LanPeerInfo {
    std::string id;
    std::string displayName;
    std::string ipAddress;
    uint16_t transferPort = 0;
    double lastSeenSeconds = 0.0;
};

struct LanWorkspaceOffer {
    std::string offerId;
    std::string senderId;
    std::string senderName;
    std::string senderIpAddress;
    std::string workspaceName;
    uint16_t transferPort = 0;
};

class LanWorkspaceShareService {
public:
    LanWorkspaceShareService();
    ~LanWorkspaceShareService();

    bool Start();
    void Stop();

    std::string LocalDisplayName() const;
    std::vector<LanPeerInfo> SnapshotPeers() const;
    std::vector<LanWorkspaceOffer> DrainIncomingOffers();

    bool ShareWorkspacePackage(const std::string& workspaceName,
                               const std::string& packageData,
                               const std::vector<std::string>& targetPeerIds,
                               bool shareToAll);

    std::optional<std::string> FetchWorkspacePackage(const LanWorkspaceOffer& offer) const;

private:
    using SocketHandle = std::intptr_t;

    struct OutboundUdpPacket {
        std::string payload;
        std::string destinationIpAddress;
        bool broadcast = false;
    };

    struct SharedOfferPayload {
        std::string packageData;
        double createdSeconds = 0.0;
    };

    void NetworkLoop();
    void HandleUdpPacket(const std::string& payload, const std::string& senderIpAddress, double nowSeconds);
    void HandleClientConnection(SocketHandle clientSocket);
    void SendPendingUdpPackets();
    void SendHelloBroadcast();
    void PruneExpiredState(double nowSeconds);

    std::string localPeerId_;
    std::string localDisplayName_;

    mutable std::mutex stateMutex_;
    std::unordered_map<std::string, LanPeerInfo> peersById_;
    std::unordered_map<std::string, double> seenIncomingOffers_;
    std::vector<LanWorkspaceOffer> incomingOffers_;
    std::vector<OutboundUdpPacket> pendingUdpPackets_;
    std::unordered_map<std::string, SharedOfferPayload> sharedPayloadsByOfferId_;
    std::atomic<std::uint64_t> offerCounter_{ 1 };
    std::atomic<std::uint32_t> activeClientHandlers_{ 0 };
    mutable std::mutex clientHandlerMutex_;
    std::condition_variable clientHandlerCv_;

    std::atomic<bool> running_{ false };
    std::jthread networkThread_;
    SocketHandle udpSocket_ = -1;
    SocketHandle tcpListenSocket_ = -1;
    std::atomic<uint16_t> localTransferPort_{ 0 };
#if defined(_WIN32)
    bool winsockInitialized_ = false;
#endif
};
