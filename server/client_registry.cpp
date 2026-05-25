#include "client_registry.hpp"
#include "federated_helpers.hpp"
#include "logger.hpp"
#include <algorithm>

using namespace pqfl_helpers;

bool ClientRegistry::Register(const ClientInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string key = ClientKey(info.tenant_id, info.model_id, info.client_id);
    auto existing = clients_.find(key);
    if (existing != clients_.end()) {
        if (!existing->second.peer_identity.empty() &&
            existing->second.peer_identity != info.peer_identity) {
            Logger::Warn("Rejected duplicate client registration for " + key +
                         " because peer identity changed from " +
                         existing->second.peer_identity + " to " + info.peer_identity);
        }
        return false;
    }
    clients_[key] = info;
    return true;
}

bool ClientRegistry::IsRegistered(const std::string& tenant_id,
                                  const std::string& model_id,
                                  const std::string& client_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.count(ClientKey(tenant_id, model_id, client_id)) > 0;
}

bool ClientRegistry::IsRegisteredForPeer(const std::string& tenant_id,
                                         const std::string& model_id,
                                         const std::string& client_id,
                                         const std::string& peer_identity) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(ClientKey(tenant_id, model_id, client_id));
    if (it == clients_.end()) {
        return false;
    }
    return !it->second.peer_identity.empty() && it->second.peer_identity == peer_identity;
}

bool ClientRegistry::BindPeerIdentityIfMissing(const std::string& tenant_id,
                                               const std::string& model_id,
                                               const std::string& client_id,
                                               const std::string& peer_identity) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(ClientKey(tenant_id, model_id, client_id));
    if (it == clients_.end()) {
        return false;
    }
    if (it->second.peer_identity.empty()) {
        it->second.peer_identity = peer_identity;
        return true;
    }
    return it->second.peer_identity == peer_identity;
}

int ClientRegistry::GetDatasetSize(const std::string& tenant_id,
                                   const std::string& model_id,
                                   const std::string& client_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(ClientKey(tenant_id, model_id, client_id));
    return it == clients_.end() ? 0 : it->second.local_dataset_size;
}

void ClientRegistry::UpdateLastActiveRound(const std::string& tenant_id,
                                           const std::string& model_id,
                                           const std::string& client_id,
                                           int round) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = clients_.find(ClientKey(tenant_id, model_id, client_id));
    if (it != clients_.end()) {
        it->second.last_active_round = std::max(it->second.last_active_round, round);
    }
}

size_t ClientRegistry::GetClientCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
}

std::vector<ClientInfo> ClientRegistry::GetAllClients() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ClientInfo> out;
    out.reserve(clients_.size());
    for (const auto& [_, info] : clients_) {
        out.push_back(info);
    }
    return out;
}
