#ifndef FEDERATED_SERVICE_HPP
#define FEDERATED_SERVICE_HPP

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <grpcpp/alarm.h>
#include <grpcpp/grpcpp.h>
#include "pqfl.grpc.pb.h"
#include "crypto_engine.hpp"
#include "state_store.hpp"

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

class ClientRegistry {
public:
    bool Register(const ClientInfo& info);
    bool IsRegistered(const std::string& tenant_id,
                      const std::string& model_id,
                      const std::string& client_id) const;
    bool IsRegisteredForPeer(const std::string& tenant_id,
                             const std::string& model_id,
                             const std::string& client_id,
                             const std::string& peer_identity) const;
    bool BindPeerIdentityIfMissing(const std::string& tenant_id,
                                   const std::string& model_id,
                                   const std::string& client_id,
                                   const std::string& peer_identity);
    int GetDatasetSize(const std::string& tenant_id,
                       const std::string& model_id,
                       const std::string& client_id) const;
    void UpdateLastActiveRound(const std::string& tenant_id,
                               const std::string& model_id,
                               const std::string& client_id,
                               int round);
    size_t GetClientCount() const;
    std::vector<ClientInfo> GetAllClients() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ClientInfo> clients_;
};

struct LatestModelSnapshot {
    int round = -1;
    int model_version = 0;
    std::vector<float> weights;
};

struct AggregationAttemptResult {
    bool accepted = false;
    bool aggregated = false;
    int model_version = 0;
    std::string message;
};

class WeightBuffer {
public:
    AggregationAttemptResult AddWeightAndCheckAggregate(
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
        std::vector<float>& aggregated_weights);

    bool IsRoundCompleted(const std::string& tenant_id,
                          const std::string& model_id,
                          int round);
    bool HasClientSubmission(const std::string& tenant_id,
                             const std::string& model_id,
                             int round,
                             const std::string& client_id);
    size_t GetNumClients(const std::string& tenant_id,
                         const std::string& model_id,
                         int round);
    LatestModelSnapshot GetLatestGlobalModel(const std::string& tenant_id,
                                             const std::string& model_id);
    void RestoreLatestGlobalModel(const std::string& tenant_id,
                                  const std::string& model_id,
                                  int round,
                                  int model_version,
                                  const std::vector<float>& weights);

private:
    std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_map<std::string, std::vector<float>>> round_weights_;
    std::unordered_map<std::string, std::unordered_map<std::string, int>> round_dataset_sizes_;
    std::unordered_set<std::string> completed_rounds_;
    std::unordered_map<std::string, LatestModelSnapshot> latest_models_;
    std::unordered_map<std::string, std::vector<float>> optimizer_velocity_;

    std::vector<float> AggFedAvg(
        const std::unordered_map<std::string, std::vector<float>>& weights,
        const std::unordered_map<std::string, int>& dataset_sizes);
    std::vector<float> AggKrum(
        const std::unordered_map<std::string, std::vector<float>>& weights);
    std::vector<float> AggTrimmedMean(
        const std::unordered_map<std::string, std::vector<float>>& weights,
        float beta);
    void ClipWeights(std::vector<float>& weights, float clip_norm);
    void ApplyDifferentialPrivacy(std::vector<float>& weights,
                                  float clip_norm,
                                  float epsilon,
                                  float delta,
                                  size_t num_clients);
    bool ValidateWeights(const std::unordered_map<std::string, std::vector<float>>& weights,
                         std::string& reason);
    int GetLatestModelVersionLocked(const std::string& scope) const;
    void RemoveSubmissionLocked(const std::string& round_key,
                                const std::string& client_id);
};

class FederatedServiceImpl {
public:
    FederatedServiceImpl(CryptoEngine& crypto,
                         StateStore& state_store,
                         const std::string& private_key_path);
    ~FederatedServiceImpl();

    void Run(grpc::ServerBuilder& builder);
    void HandleRpcs();

    ClientRegistry& GetClientRegistry() { return registry_; }
    WeightBuffer& GetWeightBuffer() { return buffer_; }

private:
    class CallData {
    public:
        virtual ~CallData() = default;
        virtual void Proceed() = 0;
    };

    class RegisterClientCall : public CallData {
    public:
        RegisterClientCall(pqfl::FederatedLearning::AsyncService* service,
                           grpc::ServerCompletionQueue* cq,
                           ClientRegistry& registry,
                           StateStore& state_store,
                           WeightBuffer& buffer);
        void Proceed() override;
    private:
        pqfl::FederatedLearning::AsyncService* service_;
        grpc::ServerCompletionQueue* cq_;
        grpc::ServerContext ctx_;
        pqfl::ClientRegistration request_;
        pqfl::RegistrationResponse response_;
        grpc::ServerAsyncResponseWriter<pqfl::RegistrationResponse> responder_;
        enum CallStatus { CREATE, PROCESS, FINISH };
        CallStatus status_;
        ClientRegistry& registry_;
        StateStore& state_store_;
        WeightBuffer& buffer_;
    };

    class GetTrainingConfigCall : public CallData {
    public:
        GetTrainingConfigCall(pqfl::FederatedLearning::AsyncService* service,
                              grpc::ServerCompletionQueue* cq,
                              CryptoEngine& crypto,
                              ClientRegistry& registry,
                              StateStore& state_store);
        void Proceed() override;
    private:
        pqfl::FederatedLearning::AsyncService* service_;
        grpc::ServerCompletionQueue* cq_;
        grpc::ServerContext ctx_;
        pqfl::ConfigRequest request_;
        pqfl::TrainingConfig response_;
        grpc::ServerAsyncResponseWriter<pqfl::TrainingConfig> responder_;
        enum CallStatus { CREATE, PROCESS, FINISH };
        CallStatus status_;
        CryptoEngine& crypto_;
        ClientRegistry& registry_;
        StateStore& state_store_;
    };

    class SubmitWeightsCall : public CallData {
    public:
        SubmitWeightsCall(pqfl::FederatedLearning::AsyncService* service,
                          grpc::ServerCompletionQueue* cq,
                          CryptoEngine& crypto,
                          ClientRegistry& registry,
                          StateStore& state_store,
                          WeightBuffer& buffer);
        void Proceed() override;
    private:
        pqfl::FederatedLearning::AsyncService* service_;
        grpc::ServerCompletionQueue* cq_;
        grpc::ServerContext ctx_;
        pqfl::WeightPayload request_;
        pqfl::AggregationResponse response_;
        grpc::ServerAsyncResponseWriter<pqfl::AggregationResponse> responder_;
        enum CallStatus { CREATE, PROCESS, FINISH };
        CallStatus status_;
        CryptoEngine& crypto_;
        ClientRegistry& registry_;
        StateStore& state_store_;
        WeightBuffer& buffer_;

        std::vector<unsigned char> DeriveSessionKey();
    };

    class StreamGlobalModelCall : public CallData {
    public:
        StreamGlobalModelCall(pqfl::FederatedLearning::AsyncService* service,
                              grpc::ServerCompletionQueue* cq,
                              CryptoEngine& crypto,
                              ClientRegistry& registry,
                              StateStore& state_store,
                              WeightBuffer& buffer,
                              const std::function<std::vector<unsigned char>(const std::string&)>& signer);
        void Proceed() override;
    private:
        pqfl::FederatedLearning::AsyncService* service_;
        grpc::ServerCompletionQueue* cq_;
        grpc::ServerContext ctx_;
        pqfl::ModelRequest request_;
        grpc::ServerAsyncWriter<pqfl::GlobalModelUpdate> responder_;
        enum CallStatus { CREATE, PROCESS, STREAM, FINISH };
        CallStatus status_;
        int last_sent_round_ = -1;
        grpc::Alarm alarm_;
        CryptoEngine& crypto_;
        ClientRegistry& registry_;
        StateStore& state_store_;
        WeightBuffer& buffer_;
        std::function<std::vector<unsigned char>(const std::string&)> signer_;

        std::vector<unsigned char> DeriveSessionKey();
    };

    CryptoEngine& crypto_;
    StateStore& state_store_;
    ClientRegistry registry_;
    WeightBuffer buffer_;
    EVP_PKEY* private_key_;

    pqfl::FederatedLearning::AsyncService service_;
    std::unique_ptr<grpc::ServerCompletionQueue> cq_;

    std::vector<unsigned char> SignData(const std::string& data);
    void RestorePersistedModels();
};

#endif // FEDERATED_SERVICE_HPP
