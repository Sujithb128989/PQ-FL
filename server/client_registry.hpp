#ifndef CLIENT_REGISTRY_HPP
#define CLIENT_REGISTRY_HPP

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @struct ClientInfo
 * @brief Represents the metadata and state of a registered federated learning client.
 */
struct ClientInfo {
    std::string tenant_id;
    std::string model_id;
    std::string client_id;
    std::string peer_identity;
    int local_dataset_size = 0;
    std::string hardware_info;
    std::string dataset_fingerprint;
    std::string registered_at;
    int last_active_round = -1;
};

/**
 * @class ClientRegistry
 * @brief Thread-safe registry for tracking federated learning clients.
 */
class ClientRegistry {
public:
    /**
     * @brief Registers a new client or updates an existing client's information.
     * @param info Client metadata.
     * @return True if registration was successful, false if a conflicting client exists.
     */
    bool Register(const ClientInfo& info);

    /**
     * @brief Checks if a client is registered.
     */
    bool IsRegistered(const std::string& tenant_id,
                      const std::string& model_id,
                      const std::string& client_id) const;

    /**
     * @brief Checks if a client is registered and matches a specific peer identity (e.g., from mTLS).
     */
    bool IsRegisteredForPeer(const std::string& tenant_id,
                             const std::string& model_id,
                             const std::string& client_id,
                             const std::string& peer_identity) const;

    /**
     * @brief Binds a peer identity to a registered client if one does not already exist.
     */
    bool BindPeerIdentityIfMissing(const std::string& tenant_id,
                                   const std::string& model_id,
                                   const std::string& client_id,
                                   const std::string& peer_identity);

    /**
     * @brief Retrieves the registered local dataset size of a client.
     */
    int GetDatasetSize(const std::string& tenant_id,
                       const std::string& model_id,
                       const std::string& client_id) const;

    /**
     * @brief Updates the last active round counter for a client.
     */
    void UpdateLastActiveRound(const std::string& tenant_id,
                               const std::string& model_id,
                               const std::string& client_id,
                               int round);

    /**
     * @brief Returns the total number of registered clients.
     */
    size_t GetClientCount() const;

    /**
     * @brief Returns a copy of all registered clients.
     */
    std::vector<ClientInfo> GetAllClients() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ClientInfo> clients_;
};

#endif // CLIENT_REGISTRY_HPP
