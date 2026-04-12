#include "federated_service.hpp"
#include "config.hpp"
#include "identity_utils.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sstream>

namespace {

std::string NormalizeTenant(const std::string& tenant_id) {
    return tenant_id.empty() ? "default-tenant" : tenant_id;
}

std::string NormalizeModel(const std::string& model_id) {
    return model_id.empty() ? "default-model" : model_id;
}

std::string ScopeKey(const std::string& tenant_id, const std::string& model_id) {
    return NormalizeTenant(tenant_id) + "::" + NormalizeModel(model_id);
}

std::string RoundKey(const std::string& tenant_id, const std::string& model_id, int round) {
    return ScopeKey(tenant_id, model_id) + "::round:" + std::to_string(round);
}

std::string ClientKey(const std::string& tenant_id,
                      const std::string& model_id,
                      const std::string& client_id) {
    return ScopeKey(tenant_id, model_id) + "::client:" + client_id;
}



std::string ValidateFederatedRequestIds(const std::string& tenant_id,
                                        const std::string& model_id,
                                        const std::string& client_id) {
    std::string error = ValidateIdentifier(tenant_id, "tenant_id", true);
    if (!error.empty()) return error;
    error = ValidateIdentifier(model_id, "model_id", true);
    if (!error.empty()) return error;
    return ValidateIdentifier(client_id, "client_id");
}

} // namespace

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

int WeightBuffer::GetLatestModelVersionLocked(const std::string& scope) const {
    auto it = latest_models_.find(scope);
    return it == latest_models_.end() ? 0 : it->second.model_version;
}

void WeightBuffer::RemoveSubmissionLocked(const std::string& round_key,
                                          const std::string& client_id) {
    auto weights_it = round_weights_.find(round_key);
    if (weights_it != round_weights_.end()) {
        weights_it->second.erase(client_id);
        if (weights_it->second.empty()) {
            round_weights_.erase(weights_it);
        }
    }

    auto dataset_it = round_dataset_sizes_.find(round_key);
    if (dataset_it != round_dataset_sizes_.end()) {
        dataset_it->second.erase(client_id);
        if (dataset_it->second.empty()) {
            round_dataset_sizes_.erase(dataset_it);
        }
    }
}

void WeightBuffer::ClipWeights(std::vector<float>& weights, float clip_norm) {
    float l2 = 0.0f;
    for (float w : weights) {
        l2 += w * w;
    }
    l2 = std::sqrt(l2);

    if (clip_norm > 0.0f && l2 > clip_norm) {
        float scale = clip_norm / l2;
        for (float& value : weights) {
            value *= scale;
        }
    }
}

void WeightBuffer::ApplyDifferentialPrivacy(std::vector<float>& weights,
                                            float clip_norm,
                                            float epsilon,
                                            float delta,
                                            size_t num_clients) {
    if (epsilon <= 0.0f || delta <= 0.0f || delta >= 1.0f || num_clients == 0) {
        return;
    }

    float sigma = (clip_norm * std::sqrt(2.0f * std::log(1.25f / delta))) /
                  (epsilon * std::sqrt(static_cast<float>(num_clients)));

    std::random_device rd;
    std::mt19937 gen(rd());
    std::normal_distribution<float> noise(0.0f, sigma);
    for (float& value : weights) {
        value += noise(gen);
    }

    Logger::Info("Applied DP noise (sigma=" + std::to_string(sigma) +
                 ", epsilon=" + std::to_string(epsilon) + ")");
}

bool WeightBuffer::ValidateWeights(
    const std::unordered_map<std::string, std::vector<float>>& weights,
    std::string& reason) {
    if (weights.empty()) {
        reason = "No weights submitted";
        return false;
    }

    size_t expected_size = weights.begin()->second.size();
    if (expected_size == 0) {
        reason = "Empty weight tensor";
        return false;
    }

    for (const auto& [client_id, values] : weights) {
        if (values.size() != expected_size) {
            reason = "Mismatched tensor dimensions";
            return false;
        }

        for (float value : values) {
            if (!std::isfinite(value)) {
                reason = "Non-finite tensor value from client " + client_id;
                return false;
            }
        }
    }

    reason.clear();
    return true;
}

std::vector<float> WeightBuffer::AggFedAvg(
    const std::unordered_map<std::string, std::vector<float>>& weights,
    const std::unordered_map<std::string, int>& dataset_sizes) {
    size_t dim = weights.begin()->second.size();
    std::vector<float> result(dim, 0.0f);

    int total_samples = 0;
    for (const auto& [_, size] : dataset_sizes) {
        total_samples += std::max(1, size);
    }

    for (const auto& [client_id, values] : weights) {
        int client_samples = 1;
        auto it = dataset_sizes.find(client_id);
        if (it != dataset_sizes.end()) {
            client_samples = std::max(1, it->second);
        }

        float factor = total_samples > 0
            ? static_cast<float>(client_samples) / static_cast<float>(total_samples)
            : 1.0f / static_cast<float>(weights.size());

        for (size_t i = 0; i < dim; ++i) {
            result[i] += values[i] * factor;
        }
    }

    return result;
}

std::vector<float> WeightBuffer::AggKrum(
    const std::unordered_map<std::string, std::vector<float>>& weights) {
    if (weights.size() < 3) {
        std::unordered_map<std::string, int> equal_sizes;
        for (const auto& [client_id, _] : weights) {
            equal_sizes[client_id] = 1;
        }
        return AggFedAvg(weights, equal_sizes);
    }

    std::vector<std::string> ids;
    std::vector<const std::vector<float>*> vectors;
    for (const auto& [client_id, values] : weights) {
        ids.push_back(client_id);
        vectors.push_back(&values);
    }

    size_t n = ids.size();
    size_t f = (n >= 3) ? (n - 3) / 2 : 0;
    size_t k = n > f + 2 ? n - f - 2 : 1;

    std::vector<std::vector<float>> distances(n, std::vector<float>(n, 0.0f));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            float dist = 0.0f;
            for (size_t d = 0; d < vectors[i]->size(); ++d) {
                float diff = (*vectors[i])[d] - (*vectors[j])[d];
                dist += diff * diff;
            }
            distances[i][j] = dist;
            distances[j][i] = dist;
        }
    }

    size_t best_idx = 0;
    float best_score = std::numeric_limits<float>::max();
    for (size_t i = 0; i < n; ++i) {
        std::vector<float> candidate = distances[i];
        std::sort(candidate.begin(), candidate.end());
        float score = 0.0f;
        for (size_t j = 1; j <= std::min(k, n - 1); ++j) {
            score += candidate[j];
        }
        if (score < best_score) {
            best_score = score;
            best_idx = i;
        }
    }

    Logger::Info("Krum selected client: " + ids[best_idx]);
    return *vectors[best_idx];
}

std::vector<float> WeightBuffer::AggTrimmedMean(
    const std::unordered_map<std::string, std::vector<float>>& weights,
    float beta) {
    size_t n = weights.size();
    size_t dim = weights.begin()->second.size();
    size_t trim_count = static_cast<size_t>(std::floor(std::max(0.0f, beta) * n));

    std::vector<const std::vector<float>*> vectors;
    for (const auto& [_, values] : weights) {
        vectors.push_back(&values);
    }

    std::vector<float> result(dim, 0.0f);
    std::vector<float> axis_values(n);
    for (size_t d = 0; d < dim; ++d) {
        for (size_t i = 0; i < n; ++i) {
            axis_values[i] = (*vectors[i])[d];
        }
        std::sort(axis_values.begin(), axis_values.end());

        size_t start = trim_count;
        size_t end = n > trim_count ? n - trim_count : n;
        if (start >= end) {
            start = 0;
            end = n;
        }

        float sum = 0.0f;
        for (size_t i = start; i < end; ++i) {
            sum += axis_values[i];
        }
        result[d] = sum / static_cast<float>(end - start);
    }

    return result;
}

AggregationAttemptResult WeightBuffer::AddWeightAndCheckAggregate(
    const std::string& tenant_id,
    const std::string& model_id,
    const std::string& client_id,
    int round,
    int dataset_size,
    const std::vector<float>& weights,
    CryptoEngine& crypto,
    StateStore& state_store,
    size_t required_clients,
    const std::string& aggregation_strategy,
    float clip_norm,
    float dp_epsilon,
    float dp_delta,
    float trimmed_beta,
    float server_momentum,
    std::string checkpoint_dir,
    std::vector<float>& aggregated_weights) {
    std::lock_guard<std::mutex> lock(mutex_);
    AggregationAttemptResult outcome;

    std::string scope = ScopeKey(tenant_id, model_id);
    std::string round_key = RoundKey(tenant_id, model_id, round);
    if (completed_rounds_.count(round_key) > 0) {
        Logger::Warn("Ignoring late weight submission for completed round " + std::to_string(round) +
                     " in scope " + scope + " from client " + client_id);
        outcome.message = "Round already aggregated";
        outcome.model_version = GetLatestModelVersionLocked(scope);
        return outcome;
    }

    std::vector<float> clipped = weights;
    ClipWeights(clipped, clip_norm);
    round_weights_[round_key][client_id] = clipped;
    round_dataset_sizes_[round_key][client_id] = std::max(1, dataset_size);
    auto rollback_submission = [&]() {
        RemoveSubmissionLocked(round_key, client_id);
        outcome.model_version = GetLatestModelVersionLocked(scope);
    };

    const auto& client_map = round_weights_[round_key];
    size_t threshold = std::max<size_t>(1, required_clients);
    if (client_map.size() < threshold) {
        outcome.accepted = true;
        outcome.message = "Weights received, waiting for more clients";
        outcome.model_version = GetLatestModelVersionLocked(scope);
        return outcome;
    }

    std::string validation_reason;
    if (!ValidateWeights(client_map, validation_reason)) {
        Logger::Error("Aggregation validation failed for " + scope + ": " + validation_reason);
        rollback_submission();
        outcome.message = "Aggregation validation failed: " + validation_reason;
        return outcome;
    }

    std::vector<float> result;
    if (aggregation_strategy == "krum") {
        result = AggKrum(client_map);
    } else if (aggregation_strategy == "trimmed_mean") {
        result = AggTrimmedMean(client_map, trimmed_beta);
    } else {
        result = AggFedAvg(client_map, round_dataset_sizes_[round_key]);
    }

    auto latest_it = latest_models_.find(scope);
    if (latest_it != latest_models_.end() &&
        !latest_it->second.weights.empty() &&
        latest_it->second.weights.size() == result.size() &&
        server_momentum > 0.0f) {
        auto& velocity = optimizer_velocity_[scope];
        if (velocity.size() != result.size()) {
            velocity.assign(result.size(), 0.0f);
        }

        std::vector<float> smoothed(result.size(), 0.0f);
        for (size_t i = 0; i < result.size(); ++i) {
            float delta = result[i] - latest_it->second.weights[i];
            velocity[i] = (server_momentum * velocity[i]) + delta;
            smoothed[i] = latest_it->second.weights[i] + velocity[i];
        }
        result = smoothed;
    }

    if (dp_epsilon > 0.0f) {
        ApplyDifferentialPrivacy(result, clip_norm, dp_epsilon, dp_delta, client_map.size());
    }

    std::filesystem::path checkpoint_root(checkpoint_dir);
    std::filesystem::path scoped_dir = checkpoint_root / NormalizeTenant(tenant_id) / NormalizeModel(model_id);
    std::filesystem::create_directories(scoped_dir);
    std::string checkpoint_path = (scoped_dir / ("global_model_round_" + std::to_string(round) + ".bin")).string();

    try {
        std::string plaintext(reinterpret_cast<const char*>(result.data()), result.size() * sizeof(float));
        std::string encrypted_blob = crypto.EncryptBlob(plaintext);
        std::ofstream ofs(checkpoint_path, std::ios::binary);
        if (!ofs.is_open()) {
            Logger::Error("Failed to open checkpoint path for writing: " + checkpoint_path);
            rollback_submission();
            outcome.message = "Failed to persist encrypted checkpoint";
            return outcome;
        }
        ofs.write(encrypted_blob.data(), encrypted_blob.size());
        ofs.flush();
        if (!ofs.good()) {
            ofs.close();
            std::filesystem::remove(checkpoint_path);
            Logger::Error("Failed to fully write checkpoint: " + checkpoint_path);
            rollback_submission();
            outcome.message = "Failed to persist encrypted checkpoint";
            return outcome;
        }
        Logger::Info("Saved encrypted model checkpoint: " + checkpoint_path);
    } catch (const std::exception& e) {
        std::filesystem::remove(checkpoint_path);
        Logger::Error("Aggregation persistence failed for " + scope + ": " + e.what());
        rollback_submission();
        outcome.message = "Aggregation persistence failed: " + std::string(e.what());
        return outcome;
    }

    outcome.model_version = state_store.RecordAggregatedModel(
        tenant_id,
        model_id,
        round,
        checkpoint_path,
        aggregation_strategy,
        crypto.GetActiveKeyVersion(),
        static_cast<int>(client_map.size()),
        static_cast<int>(result.size())
    );

    latest_models_[scope] = {round, outcome.model_version, result};
    completed_rounds_.insert(round_key);
    aggregated_weights = result;
    outcome.accepted = true;
    outcome.aggregated = true;
    outcome.message = "Aggregation completed for round " + std::to_string(round);
    return outcome;
}

bool WeightBuffer::IsRoundCompleted(const std::string& tenant_id,
                                    const std::string& model_id,
                                    int round) {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_rounds_.count(RoundKey(tenant_id, model_id, round)) > 0;
}

bool WeightBuffer::HasClientSubmission(const std::string& tenant_id,
                                       const std::string& model_id,
                                       int round,
                                       const std::string& client_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = round_weights_.find(RoundKey(tenant_id, model_id, round));
    return it != round_weights_.end() && it->second.count(client_id) > 0;
}

size_t WeightBuffer::GetNumClients(const std::string& tenant_id,
                                   const std::string& model_id,
                                   int round) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = round_weights_.find(RoundKey(tenant_id, model_id, round));
    return it == round_weights_.end() ? 0 : it->second.size();
}

LatestModelSnapshot WeightBuffer::GetLatestGlobalModel(const std::string& tenant_id,
                                                       const std::string& model_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = latest_models_.find(ScopeKey(tenant_id, model_id));
    if (it == latest_models_.end()) {
        return {};
    }
    return it->second;
}

void WeightBuffer::RestoreLatestGlobalModel(const std::string& tenant_id,
                                            const std::string& model_id,
                                            int round,
                                            int model_version,
                                            const std::vector<float>& weights) {
    std::lock_guard<std::mutex> lock(mutex_);
    latest_models_[ScopeKey(tenant_id, model_id)] = {round, model_version, weights};
}

FederatedServiceImpl::FederatedServiceImpl(CryptoEngine& crypto,
                                           StateStore& state_store,
                                           const std::string& private_key_path)
    : crypto_(crypto),
      state_store_(state_store),
      private_key_(nullptr) {
    FILE* key_file = fopen(private_key_path.c_str(), "r");
    if (key_file) {
        private_key_ = PEM_read_PrivateKey(key_file, NULL, NULL, NULL);
        fclose(key_file);
    }
    if (!private_key_) {
        Logger::Error("Failed to load server private key for model signing");
    }

    for (const auto& client : state_store_.ListClients()) {
        registry_.Register({
            client.tenant_id,
            client.model_id,
            client.client_id,
            client.peer_identity,
            client.local_dataset_size,
            client.hardware_info,
            client.dataset_fingerprint,
            client.registered_at,
            client.last_active_round
        });
    }

    RestorePersistedModels();
}

FederatedServiceImpl::~FederatedServiceImpl() {
    if (cq_) {
        cq_->Shutdown();
    }
    if (private_key_) {
        EVP_PKEY_free(private_key_);
    }
}

void FederatedServiceImpl::RestorePersistedModels() {
    for (const auto& model : state_store_.GetActiveModels()) {
        std::ifstream file(model.checkpoint_path, std::ios::binary);
        if (!file.is_open()) {
            Logger::Warn("Skipped persisted model restore, checkpoint missing: " + model.checkpoint_path);
            continue;
        }

        std::string blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        try {
            std::string plaintext = crypto_.DecryptBlob(blob);
            if (plaintext.size() % sizeof(float) != 0) {
                Logger::Warn("Skipped persisted model restore due to invalid tensor size: " + model.checkpoint_path);
                continue;
            }

            size_t float_count = plaintext.size() / sizeof(float);
            std::vector<float> weights(float_count);
            std::memcpy(weights.data(), plaintext.data(), plaintext.size());
            buffer_.RestoreLatestGlobalModel(
                model.tenant_id,
                model.model_id,
                model.latest_round,
                model.version,
                weights
            );
        } catch (const std::exception& e) {
            Logger::Warn("Failed to restore persisted model from " + model.checkpoint_path +
                         ": " + e.what());
        }
    }
}

std::vector<unsigned char> FederatedServiceImpl::SignData(const std::string& data) {
    if (!private_key_) {
        return {};
    }

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        return {};
    }

    size_t siglen = 0;
    std::vector<unsigned char> sig;
    if (EVP_DigestSignInit(mdctx, NULL, NULL, NULL, private_key_) <= 0 ||
        EVP_DigestSign(mdctx, NULL, &siglen,
                       reinterpret_cast<const unsigned char*>(data.data()), data.size()) <= 0) {
        EVP_MD_CTX_free(mdctx);
        return {};
    }

    sig.resize(siglen);
    if (EVP_DigestSign(mdctx, sig.data(), &siglen,
                       reinterpret_cast<const unsigned char*>(data.data()), data.size()) <= 0) {
        EVP_MD_CTX_free(mdctx);
        return {};
    }

    sig.resize(siglen);
    EVP_MD_CTX_free(mdctx);
    return sig;
}

void FederatedServiceImpl::Run(grpc::ServerBuilder& builder) {
    builder.RegisterService(&service_);
    cq_ = builder.AddCompletionQueue();
}

void FederatedServiceImpl::HandleRpcs() {
    auto signer = [this](const std::string& payload) { return SignData(payload); };

    new RegisterClientCall(&service_, cq_.get(), registry_, state_store_, buffer_);
    new GetTrainingConfigCall(&service_, cq_.get(), crypto_, registry_, state_store_);
    new SubmitWeightsCall(&service_, cq_.get(), crypto_, registry_, state_store_, buffer_);
    new StreamGlobalModelCall(&service_, cq_.get(), crypto_, registry_, state_store_, buffer_, signer);

    void* tag = nullptr;
    bool ok = false;
    while (cq_->Next(&tag, &ok)) {
        CallData* call = static_cast<CallData*>(tag);
        if (ok) {
            call->Proceed();
        } else {
            delete call;
        }
    }
}

FederatedServiceImpl::RegisterClientCall::RegisterClientCall(
    pqfl::FederatedLearning::AsyncService* service,
    grpc::ServerCompletionQueue* cq,
    ClientRegistry& registry,
    StateStore& state_store,
    WeightBuffer& buffer)
    : service_(service),
      cq_(cq),
      responder_(&ctx_),
      status_(CREATE),
      registry_(registry),
      state_store_(state_store),
      buffer_(buffer) {
    Proceed();
}

void FederatedServiceImpl::RegisterClientCall::Proceed() {
    if (status_ == CREATE) {
        status_ = PROCESS;
        service_->RequestRegisterClient(&ctx_, &request_, &responder_, cq_, cq_, this);
        return;
    }

    if (status_ == PROCESS) {
        new RegisterClientCall(service_, cq_, registry_, state_store_, buffer_);

        std::string validation_error = ValidateFederatedRequestIds(
            request_.tenant_id(),
            request_.model_id(),
            request_.client_id());
        if (!validation_error.empty()) {
            response_.set_accepted(false);
            response_.set_assigned_id("");
            response_.set_current_round(-1);
            response_.set_active_model_version(0);
            Logger::Warn("Rejected RegisterClient request: " + validation_error, "Federated");
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        if (!IsPeerAuthenticated(&ctx_)) {
            response_.set_accepted(false);
            response_.set_assigned_id("");
            response_.set_current_round(-1);
            response_.set_active_model_version(0);
            Logger::Warn("Rejected RegisterClient request from unauthenticated peer", "Federated");
            status_ = FINISH;
            responder_.Finish(response_,
                              grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                           "Authenticated client certificate required"),
                              this);
            return;
        }

        std::string peer_identity = NormalizePeerIdentity(GetPeerIdentity(&ctx_));
        if (peer_identity.empty()) {
            response_.set_accepted(false);
            response_.set_assigned_id("");
            response_.set_current_round(-1);
            response_.set_active_model_version(0);
            Logger::Warn("Rejected RegisterClient request because peer identity was empty", "Federated");
            status_ = FINISH;
            responder_.Finish(response_,
                              grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                           "Peer identity is required"),
                              this);
            return;
        }

        ClientInfo info;
        info.tenant_id = NormalizeTenant(request_.tenant_id());
        info.model_id = NormalizeModel(request_.model_id());
        info.client_id = request_.client_id();
        info.peer_identity = peer_identity;
        info.local_dataset_size = std::max(1, request_.local_dataset_size());
        info.hardware_info = request_.hardware_info();
        info.dataset_fingerprint = request_.dataset_fingerprint();
        info.registered_at = CurrentTimestamp();

        bool accepted = registry_.Register(info);
        response_.set_accepted(accepted);
        response_.set_assigned_id(request_.client_id());

        auto latest = buffer_.GetLatestGlobalModel(info.tenant_id, info.model_id);
        response_.set_current_round(latest.round);
        response_.set_active_model_version(state_store_.GetActiveModelVersion(info.tenant_id, info.model_id));

        state_store_.RecordClientRegistration({
            info.tenant_id,
            info.model_id,
            info.client_id,
            info.peer_identity,
            info.local_dataset_size,
            info.hardware_info,
            info.dataset_fingerprint,
            info.last_active_round,
            info.registered_at
        });
        state_store_.RecordAuditEvent({
            CurrentTimestamp(),
            accepted ? "client_registered" : "client_register_duplicate",
            info.tenant_id,
            info.model_id,
            info.client_id,
            "Dataset size " + std::to_string(info.local_dataset_size)
        });

        status_ = FINISH;
        responder_.Finish(response_, grpc::Status::OK, this);
        return;
    }

    delete this;
}

FederatedServiceImpl::GetTrainingConfigCall::GetTrainingConfigCall(
    pqfl::FederatedLearning::AsyncService* service,
    grpc::ServerCompletionQueue* cq,
    CryptoEngine& crypto,
    ClientRegistry& registry,
    StateStore& state_store)
    : service_(service),
      cq_(cq),
      responder_(&ctx_),
      status_(CREATE),
      crypto_(crypto),
      registry_(registry),
      state_store_(state_store) {
    Proceed();
}

void FederatedServiceImpl::GetTrainingConfigCall::Proceed() {
    if (status_ == CREATE) {
        status_ = PROCESS;
        service_->RequestGetTrainingConfig(&ctx_, &request_, &responder_, cq_, cq_, this);
        return;
    }

    if (status_ == PROCESS) {
        new GetTrainingConfigCall(service_, cq_, crypto_, registry_, state_store_);

        std::string tenant_id = NormalizeTenant(request_.tenant_id());
        std::string model_id = NormalizeModel(request_.model_id());
        std::string validation_error = ValidateFederatedRequestIds(
            request_.tenant_id(),
            request_.model_id(),
            request_.client_id());
        if (!validation_error.empty()) {
            Logger::Warn("Rejected GetTrainingConfig request: " + validation_error, "Federated");
            status_ = FINISH;
            responder_.Finish(response_,
                              grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error),
                              this);
            return;
        }

        if (!IsPeerAuthenticated(&ctx_)) {
            Logger::Warn("Rejected GetTrainingConfig request from unauthenticated peer", "Federated");
            status_ = FINISH;
            responder_.Finish(response_,
                              grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                           "Authenticated client certificate required"),
                              this);
            return;
        }

        std::string peer_identity = NormalizePeerIdentity(GetPeerIdentity(&ctx_));
        if (!registry_.IsRegisteredForPeer(tenant_id, model_id, request_.client_id(), peer_identity)) {
            status_ = FINISH;
            responder_.Finish(response_,
                              grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                           "Client must register with the same certificate identity before requesting config"),
                              this);
            return;
        }

        const auto& cfg = Config::Instance().Get();
        response_.set_target_rounds(cfg.target_rounds);
        response_.set_learning_rate(cfg.learning_rate);
        response_.set_batch_size(cfg.batch_size);
        response_.set_required_clients(cfg.required_clients);
        response_.set_clip_norm(cfg.clip_norm);
        response_.set_aggregation_strategy(cfg.aggregation_strategy);
        response_.set_tenant_id(tenant_id);
        response_.set_model_id(model_id);
        response_.set_active_model_version(state_store_.GetActiveModelVersion(tenant_id, model_id));
        response_.set_active_key_version(crypto_.GetActiveKeyVersion());
        response_.set_server_momentum(cfg.server_momentum);
        response_.set_assignment_ready(false);
        response_.set_assignment_message("No active scheduled training job; ad-hoc participation only");

        PersistedAssignmentLease assignment;
        PersistedTrainingJobRecord job;
        PersistedDatasetRecord dataset;
        if (state_store_.LeaseTrainingAssignment(
                tenant_id,
                model_id,
                request_.client_id(),
                response_.active_model_version(),
                cfg.required_clients,
                cfg.default_assignment_ttl_seconds,
                &assignment,
                &job,
                &dataset)) {
            response_.set_target_rounds(job.target_rounds > 0 ? job.target_rounds : cfg.target_rounds);
            response_.set_required_clients(
                job.min_clients_per_round > 0 ? job.min_clients_per_round : cfg.required_clients);
            response_.set_job_id(job.job_id);
            response_.set_dataset_id(dataset.dataset_id);
            response_.set_dataset_fingerprint(dataset.fingerprint);
            response_.set_assigned_round_index(assignment.round_index);
            response_.set_assignment_id(assignment.assignment_id);
            response_.set_assignment_expires_at(assignment.expires_at);
            response_.set_base_model_version(assignment.base_model_version);
            response_.set_job_status(job.status);
            response_.set_assignment_ready(true);
            response_.set_assignment_message("Training assignment leased for this client");
        }

        status_ = FINISH;
        responder_.Finish(response_, grpc::Status::OK, this);
        return;
    }

    delete this;
}

std::vector<unsigned char> FederatedServiceImpl::SubmitWeightsCall::DeriveSessionKey() {
    std::vector<std::string> context_parts = {
        GetPeerIdentity(&ctx_),
        NormalizeTenant(request_.tenant_id()),
        NormalizeModel(request_.model_id()),
        request_.client_id(),
        std::to_string(request_.round_index()),
        "submit"
    };
    if (!request_.client_nonce().empty()) {
        context_parts.push_back(request_.client_nonce());
    }
    return crypto_.DeriveSessionKey(context_parts);
}

FederatedServiceImpl::SubmitWeightsCall::SubmitWeightsCall(
    pqfl::FederatedLearning::AsyncService* service,
    grpc::ServerCompletionQueue* cq,
    CryptoEngine& crypto,
    ClientRegistry& registry,
    StateStore& state_store,
    WeightBuffer& buffer)
    : service_(service),
      cq_(cq),
      responder_(&ctx_),
      status_(CREATE),
      crypto_(crypto),
      registry_(registry),
      state_store_(state_store),
      buffer_(buffer) {
    Proceed();
}

void FederatedServiceImpl::SubmitWeightsCall::Proceed() {
    if (status_ == CREATE) {
        status_ = PROCESS;
        service_->RequestSubmitWeights(&ctx_, &request_, &responder_, cq_, cq_, this);
        return;
    }

    if (status_ == PROCESS) {
        new SubmitWeightsCall(service_, cq_, crypto_, registry_, state_store_, buffer_);

        const auto& cfg = Config::Instance().Get();
        std::string tenant_id = NormalizeTenant(request_.tenant_id());
        std::string model_id = NormalizeModel(request_.model_id());

        std::string validation_error = ValidateFederatedRequestIds(
            request_.tenant_id(), request_.model_id(), request_.client_id());
        if (!validation_error.empty()) {
            response_.set_accepted(false);
            response_.set_message(validation_error);
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }
        if (request_.round_index() <= 0) {
            response_.set_accepted(false);
            response_.set_message("round_index must be >= 1");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }
        if (request_.local_dataset_size() <= 0) {
            response_.set_accepted(false);
            response_.set_message("local_dataset_size must be >= 1");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }
        if (request_.encrypted_weights().empty()) {
            response_.set_accepted(false);
            response_.set_message("encrypted_weights must not be empty");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }
        if (request_.iv().size() != 12 || request_.auth_tag().size() != 16) {
            response_.set_accepted(false);
            response_.set_message("iv/auth_tag length mismatch");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        if (!IsPeerAuthenticated(&ctx_)) {
            response_.set_accepted(false);
            response_.set_message("Authenticated client certificate required");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        std::string peer_identity = NormalizePeerIdentity(GetPeerIdentity(&ctx_));
        if (!registry_.IsRegisteredForPeer(tenant_id, model_id, request_.client_id(), peer_identity)) {
            response_.set_accepted(false);
            response_.set_message("Client must register with the same certificate identity before submitting weights");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        int current_model_version = state_store_.GetActiveModelVersion(tenant_id, model_id);
        if (request_.base_model_version() > 0 &&
            current_model_version > 0 &&
            request_.base_model_version() != current_model_version) {
            response_.set_accepted(false);
            response_.set_message("Submission is based on a stale global model version");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            response_.set_active_model_version(current_model_version);
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        PersistedTrainingJobRecord active_job{};
        bool has_active_job = state_store_.GetActiveTrainingJob(tenant_id, model_id, &active_job);
        if (has_active_job) {
            std::string assignment_error;
            if (!state_store_.ValidateTrainingAssignmentSubmission(
                    tenant_id,
                    model_id,
                    request_.client_id(),
                    request_.job_id(),
                    request_.assignment_id(),
                    request_.round_index(),
                    request_.base_model_version(),
                    &assignment_error)) {
                response_.set_accepted(false);
                response_.set_message(assignment_error);
                response_.set_active_key_version(crypto_.GetActiveKeyVersion());
                response_.set_active_model_version(current_model_version);
                status_ = FINISH;
                responder_.Finish(response_, grpc::Status::OK, this);
                return;
            }
        }

        if (buffer_.IsRoundCompleted(tenant_id, model_id, request_.round_index())) {
            response_.set_accepted(false);
            response_.set_message("Round already aggregated");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            response_.set_active_model_version(current_model_version);
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }
        if (buffer_.HasClientSubmission(tenant_id, model_id, request_.round_index(), request_.client_id())) {
            response_.set_accepted(false);
            response_.set_message("Client already submitted weights for this round");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            response_.set_active_model_version(current_model_version);
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        std::vector<unsigned char> session_key = DeriveSessionKey();
        std::vector<unsigned char> iv(request_.iv().begin(), request_.iv().end());
        std::vector<unsigned char> tag(request_.auth_tag().begin(), request_.auth_tag().end());

        std::string plaintext;
        try {
            plaintext = CryptoEngine::DecryptWithKey(session_key.data(),
                                                     request_.encrypted_weights(),
                                                     iv,
                                                     tag);
        } catch (const std::exception& e) {
            response_.set_accepted(false);
            response_.set_message("Decryption failed: " + std::string(e.what()));
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        if (plaintext.size() % sizeof(float) != 0) {
            response_.set_accepted(false);
            response_.set_message("Invalid weight dimensions");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        size_t num_floats = plaintext.size() / sizeof(float);
        std::vector<float> weights(num_floats);
        std::memcpy(weights.data(), plaintext.data(), plaintext.size());

        registry_.UpdateLastActiveRound(tenant_id, model_id, request_.client_id(), request_.round_index());
        state_store_.UpdateClientLastRound(tenant_id, model_id, request_.client_id(), request_.round_index());
        state_store_.RecordWeightSubmission(tenant_id, model_id, request_.client_id(), request_.round_index());

        std::vector<float> aggregated_weights;
        size_t required_clients = static_cast<size_t>(cfg.required_clients);
        if (has_active_job && active_job.min_clients_per_round > 0) {
            required_clients = static_cast<size_t>(active_job.min_clients_per_round);
        }
        AggregationAttemptResult aggregation_result = buffer_.AddWeightAndCheckAggregate(
            tenant_id,
            model_id,
            request_.client_id(),
            request_.round_index(),
            std::max(1, request_.local_dataset_size()),
            weights,
            crypto_,
            state_store_,
            required_clients,
            cfg.aggregation_strategy,
            cfg.clip_norm,
            cfg.dp_epsilon,
            cfg.dp_delta,
            cfg.trimmed_mean_beta,
            cfg.server_momentum,
            cfg.checkpoint_dir,
            aggregated_weights
        );

        if (!aggregation_result.accepted) {
            response_.set_accepted(false);
            response_.set_clients_received(buffer_.GetNumClients(tenant_id, model_id, request_.round_index()));
            response_.set_clients_required(static_cast<int>(required_clients));
            response_.set_aggregation_triggered(false);
            response_.set_active_model_version(aggregation_result.model_version);
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            response_.set_message(aggregation_result.message);
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, this);
            return;
        }

        if (has_active_job) {
            std::string assignment_error;
            if (!state_store_.AcceptTrainingAssignmentSubmission(
                    tenant_id,
                    model_id,
                    request_.client_id(),
                    request_.job_id(),
                    request_.assignment_id(),
                    request_.round_index(),
                    request_.base_model_version(),
                    &assignment_error)) {
                response_.set_accepted(false);
                response_.set_clients_received(buffer_.GetNumClients(tenant_id, model_id, request_.round_index()));
                response_.set_clients_required(static_cast<int>(required_clients));
                response_.set_aggregation_triggered(false);
                response_.set_active_model_version(aggregation_result.model_version);
                response_.set_active_key_version(crypto_.GetActiveKeyVersion());
                response_.set_message(assignment_error);
                status_ = FINISH;
                responder_.Finish(response_, grpc::Status::OK, this);
                return;
            }
        }

        response_.set_accepted(true);
        response_.set_clients_received(buffer_.GetNumClients(tenant_id, model_id, request_.round_index()));
        response_.set_clients_required(static_cast<int>(required_clients));
        response_.set_aggregation_triggered(aggregation_result.aggregated);
        response_.set_active_model_version(aggregation_result.model_version);
        response_.set_active_key_version(crypto_.GetActiveKeyVersion());
        response_.set_message(aggregation_result.message);

        status_ = FINISH;
        responder_.Finish(response_, grpc::Status::OK, this);
        return;
    }

    delete this;
}

std::vector<unsigned char> FederatedServiceImpl::StreamGlobalModelCall::DeriveSessionKey() {
    return crypto_.DeriveSessionKey({
        GetPeerIdentity(&ctx_),
        NormalizeTenant(request_.tenant_id()),
        NormalizeModel(request_.model_id()),
        request_.client_id(),
        "stream"
    });
}

FederatedServiceImpl::StreamGlobalModelCall::StreamGlobalModelCall(
    pqfl::FederatedLearning::AsyncService* service,
    grpc::ServerCompletionQueue* cq,
    CryptoEngine& crypto,
    ClientRegistry& registry,
    StateStore& state_store,
    WeightBuffer& buffer,
    const std::function<std::vector<unsigned char>(const std::string&)>& signer)
    : service_(service),
      cq_(cq),
      responder_(&ctx_),
      status_(CREATE),
      crypto_(crypto),
      registry_(registry),
      state_store_(state_store),
      buffer_(buffer),
      signer_(signer) {
    Proceed();
}

void FederatedServiceImpl::StreamGlobalModelCall::Proceed() {
    if (status_ == CREATE) {
        status_ = PROCESS;
        service_->RequestStreamGlobalModel(&ctx_, &request_, &responder_, cq_, cq_, this);
        return;
    }

    if (status_ == PROCESS || status_ == STREAM) {
        std::string tenant_id = NormalizeTenant(request_.tenant_id());
        std::string model_id = NormalizeModel(request_.model_id());

        if (status_ == PROCESS) {
            new StreamGlobalModelCall(service_, cq_, crypto_, registry_, state_store_, buffer_, signer_);

            std::string validation_error = ValidateFederatedRequestIds(
                request_.tenant_id(),
                request_.model_id(),
                request_.client_id());
            if (!validation_error.empty()) {
                status_ = FINISH;
                responder_.Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error),
                                  this);
                return;
            }

            if (!IsPeerAuthenticated(&ctx_)) {
                status_ = FINISH;
                responder_.Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                               "Authenticated client certificate required"),
                                  this);
                return;
            }

            std::string peer_identity = NormalizePeerIdentity(GetPeerIdentity(&ctx_));
            if (!registry_.IsRegisteredForPeer(tenant_id, model_id, request_.client_id(), peer_identity)) {
                status_ = FINISH;
                responder_.Finish(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                               "Client must register with the same certificate identity before streaming models"),
                                  this);
                return;
            }

            status_ = STREAM;
        }

        auto latest = buffer_.GetLatestGlobalModel(tenant_id, model_id);
        if (latest.round == -1 || latest.weights.empty() || latest.round == last_sent_round_) {
            alarm_.Set(cq_,
                       gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                                    gpr_time_from_seconds(1, GPR_TIMESPAN)),
                       this);
            return;
        }

        last_sent_round_ = latest.round;
        std::string plaintext(reinterpret_cast<const char*>(latest.weights.data()),
                              latest.weights.size() * sizeof(float));

        std::vector<unsigned char> session_key = DeriveSessionKey();
        std::vector<unsigned char> iv_out;
        std::vector<unsigned char> tag_out;
        std::string ciphertext;
        try {
            ciphertext = CryptoEngine::EncryptWithKey(session_key.data(), plaintext, iv_out, tag_out);
        } catch (const std::exception&) {
            status_ = FINISH;
            responder_.Finish(grpc::Status(grpc::StatusCode::INTERNAL,
                                           "Failed to encrypt global model"),
                              this);
            return;
        }

        std::string server_nonce = CurrentTimestamp();
        std::string to_sign = NormalizeTenant(tenant_id) + "|" +
                              NormalizeModel(model_id) + "|" +
                              std::to_string(latest.round) + "|" +
                              std::to_string(latest.model_version) + "|" +
                              ciphertext;
        std::vector<unsigned char> sig = signer_(to_sign);

        pqfl::GlobalModelUpdate update;
        update.set_round_index(latest.round);
        update.set_encrypted_weights(ciphertext);
        update.set_iv(std::string(iv_out.begin(), iv_out.end()));
        update.set_auth_tag(std::string(tag_out.begin(), tag_out.end()));
        update.set_server_signature(std::string(sig.begin(), sig.end()));
        update.set_aggregation_method(Config::Instance().Get().aggregation_strategy);
        update.set_tenant_id(tenant_id);
        update.set_model_id(model_id);
        update.set_model_version(latest.model_version);
        update.set_key_version(crypto_.GetActiveKeyVersion());
        update.set_server_nonce(server_nonce);

        responder_.Write(update, this);
        return;
    }

    delete this;
}
