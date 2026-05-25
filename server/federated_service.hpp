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

#include "client_registry.hpp"
#include "weight_buffer.hpp"

class FederatedServiceImpl {
public:
    FederatedServiceImpl(CryptoEngine& crypto,
                         StateStore& state_store,
                         const std::string& private_key_path);
    ~FederatedServiceImpl();

    void Run(grpc::ServerBuilder& builder);
    void StartRpcs();
    void HandleRpcs();

    ClientRegistry& GetClientRegistry() { return registry_; }
    WeightBuffer& GetWeightBuffer() { return buffer_; }

private:
    class CallData {
    public:
        enum class Event { START, REQUEST, WRITE, ALARM, FINISH, DONE };
        struct Tag {
            CallData* call;
            Event event;
        };
        virtual ~CallData() = default;
        virtual void Proceed(bool ok, Event event) = 0;
        Tag* MakeTag(Event event) { return new Tag{this, event}; }
    };

    class RegisterClientCall : public CallData {
    public:
        RegisterClientCall(pqfl::FederatedLearning::AsyncService* service,
                           grpc::ServerCompletionQueue* cq,
                           ClientRegistry& registry,
                           StateStore& state_store,
                           WeightBuffer& buffer);
        void Proceed(bool ok, Event event) override;
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
        void Proceed(bool ok, Event event) override;
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
        void Proceed(bool ok, Event event) override;
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
        void Proceed(bool ok, Event event) override;
    private:
        pqfl::FederatedLearning::AsyncService* service_;
        grpc::ServerCompletionQueue* cq_;
        grpc::ServerContext ctx_;
        pqfl::ModelRequest request_;
        pqfl::GlobalModelUpdate pending_update_;
        grpc::ServerAsyncWriter<pqfl::GlobalModelUpdate> responder_;
        enum CallStatus { CREATE, PROCESS, STREAM, FINISH };
        CallStatus status_;
        int last_sent_round_ = -1;
        grpc::Alarm alarm_;
        bool done_notify_in_flight_ = false;
        bool done_received_ = false;
        bool write_in_flight_ = false;
        bool alarm_in_flight_ = false;
        bool finish_in_flight_ = false;
        CryptoEngine& crypto_;
        ClientRegistry& registry_;
        StateStore& state_store_;
        WeightBuffer& buffer_;
        std::function<std::vector<unsigned char>(const std::string&)> signer_;

        std::vector<unsigned char> DeriveSessionKey();
        void FinishWithStatus(const grpc::Status& status);
        void RequestDoneNotification();
        bool MaybeDeleteAfterTerminalEvent();
    };

    CryptoEngine& crypto_;
    StateStore& state_store_;
    ClientRegistry registry_;
    WeightBuffer buffer_;
    EVP_PKEY* private_key_;

    pqfl::FederatedLearning::AsyncService service_;
    std::unique_ptr<grpc::ServerCompletionQueue> cq_;
    std::mutex proceed_mutex_;

    std::vector<unsigned char> SignData(const std::string& data);
    void RestorePersistedModels();
};

#endif // FEDERATED_SERVICE_HPP
