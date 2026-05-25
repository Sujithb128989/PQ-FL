#include "federated_service.hpp"
#include "config.hpp"
#include "identity_utils.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include "federated_helpers.hpp"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <sstream>

using namespace pqfl_helpers;

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

void FederatedServiceImpl::StartRpcs() {
    auto signer = [this](const std::string& payload) { return SignData(payload); };

    new RegisterClientCall(&service_, cq_.get(), registry_, state_store_, buffer_);
    new GetTrainingConfigCall(&service_, cq_.get(), crypto_, registry_, state_store_);
    new SubmitWeightsCall(&service_, cq_.get(), crypto_, registry_, state_store_, buffer_);
    new StreamGlobalModelCall(&service_, cq_.get(), crypto_, registry_, state_store_, buffer_, signer);
}

void FederatedServiceImpl::HandleRpcs() {
    void* tag = nullptr;
    bool ok = false;
    while (cq_->Next(&tag, &ok)) {
        auto* event_tag = static_cast<CallData::Tag*>(tag);
        CallData* call = event_tag->call;
        CallData::Event event = event_tag->event;
        delete event_tag;
        std::lock_guard<std::mutex> lock(proceed_mutex_);
        call->Proceed(ok, event);
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
    Proceed(true, Event::START);
}

void FederatedServiceImpl::RegisterClientCall::Proceed(bool ok, Event event) {
    if (event == Event::FINISH || !ok) {
        delete this;
        return;
    }

    if (status_ == CREATE) {
        status_ = PROCESS;
        service_->RequestRegisterClient(&ctx_, &request_, &responder_, cq_, cq_, MakeTag(Event::REQUEST));
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
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
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
                              MakeTag(Event::FINISH));
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
                              MakeTag(Event::FINISH));
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
        responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
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
    Proceed(true, Event::START);
}

void FederatedServiceImpl::GetTrainingConfigCall::Proceed(bool ok, Event event) {
    if (event == Event::FINISH || !ok) {
        delete this;
        return;
    }

    if (status_ == CREATE) {
        status_ = PROCESS;
        service_->RequestGetTrainingConfig(&ctx_, &request_, &responder_, cq_, cq_, MakeTag(Event::REQUEST));
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
                              MakeTag(Event::FINISH));
            return;
        }

        if (!IsPeerAuthenticated(&ctx_)) {
            Logger::Warn("Rejected GetTrainingConfig request from unauthenticated peer", "Federated");
            status_ = FINISH;
            responder_.Finish(response_,
                              grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                           "Authenticated client certificate required"),
                              MakeTag(Event::FINISH));
            return;
        }

        std::string peer_identity = NormalizePeerIdentity(GetPeerIdentity(&ctx_));
        if (!registry_.IsRegisteredForPeer(tenant_id, model_id, request_.client_id(), peer_identity)) {
            status_ = FINISH;
            responder_.Finish(response_,
                              grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                           "Client must register with the same certificate identity before requesting config"),
                              MakeTag(Event::FINISH));
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
        std::vector<unsigned char> kem_public_key = crypto_.GetKemPublicKey();
        response_.set_payload_kem_public_key(
            std::string(kem_public_key.begin(), kem_public_key.end()));
        response_.set_payload_kem_algorithm(crypto_.GetKemAlgorithm());

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
        responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
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
    if (!request_.kem_ciphertext().empty()) {
        std::vector<unsigned char> kem_ciphertext(
            request_.kem_ciphertext().begin(),
            request_.kem_ciphertext().end());
        Logger::Info("SubmitWeights deriving AES-256-GCM payload key via ML-KEM-1024 for client " +
                     request_.client_id() +
                     ", ciphertext_bytes=" + std::to_string(kem_ciphertext.size()));
        return crypto_.DeriveKemSessionKey(context_parts, kem_ciphertext);
    }
    Logger::Info("SubmitWeights deriving AES-256-GCM payload key via shared secret for client " +
                 request_.client_id());
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
    Proceed(true, Event::START);
}

void FederatedServiceImpl::SubmitWeightsCall::Proceed(bool ok, Event event) {
    if (event == Event::FINISH || !ok) {
        delete this;
        return;
    }

    if (status_ == CREATE) {
        status_ = PROCESS;
        service_->RequestSubmitWeights(&ctx_, &request_, &responder_, cq_, cq_, MakeTag(Event::REQUEST));
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
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }
        if (request_.round_index() <= 0) {
            response_.set_accepted(false);
            response_.set_message("round_index must be >= 1");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }
        if (request_.local_dataset_size() <= 0) {
            response_.set_accepted(false);
            response_.set_message("local_dataset_size must be >= 1");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }
        if (request_.encrypted_weights().empty()) {
            response_.set_accepted(false);
            response_.set_message("encrypted_weights must not be empty");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }
        if (request_.iv().size() != 12 || request_.auth_tag().size() != 16) {
            response_.set_accepted(false);
            response_.set_message("iv/auth_tag length mismatch");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }
        if (request_.key_agreement() == "ml-kem-1024" && request_.kem_ciphertext().empty()) {
            response_.set_accepted(false);
            response_.set_message("ML-KEM-1024 key agreement requires kem_ciphertext");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }

        if (!IsPeerAuthenticated(&ctx_)) {
            response_.set_accepted(false);
            response_.set_message("Authenticated client certificate required");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }

        std::string peer_identity = NormalizePeerIdentity(GetPeerIdentity(&ctx_));
        if (!registry_.IsRegisteredForPeer(tenant_id, model_id, request_.client_id(), peer_identity)) {
            response_.set_accepted(false);
            response_.set_message("Client must register with the same certificate identity before submitting weights");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
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
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
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
                responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
                return;
            }
        }

        if (buffer_.IsRoundCompleted(tenant_id, model_id, request_.round_index())) {
            response_.set_accepted(false);
            response_.set_message("Round already aggregated");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            response_.set_active_model_version(current_model_version);
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }
        if (buffer_.HasClientSubmission(tenant_id, model_id, request_.round_index(), request_.client_id())) {
            response_.set_accepted(false);
            response_.set_message("Client already submitted weights for this round");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            response_.set_active_model_version(current_model_version);
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }

        std::vector<unsigned char> iv(request_.iv().begin(), request_.iv().end());
        std::vector<unsigned char> tag(request_.auth_tag().begin(), request_.auth_tag().end());

        std::string plaintext;
        try {
            std::vector<unsigned char> session_key = DeriveSessionKey();
            plaintext = CryptoEngine::DecryptWithKey(session_key.data(),
                                                     request_.encrypted_weights(),
                                                     iv,
                                                     tag);
        } catch (const std::exception& e) {
            response_.set_accepted(false);
            response_.set_message("Decryption failed: " + std::string(e.what()));
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
            return;
        }

        if (plaintext.size() % sizeof(float) != 0) {
            response_.set_accepted(false);
            response_.set_message("Invalid weight dimensions");
            response_.set_active_key_version(crypto_.GetActiveKeyVersion());
            status_ = FINISH;
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
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
            responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
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
                responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
                return;
            }

            if (aggregation_result.aggregated) {
                state_store_.AdvanceTrainingJobRound(
                    tenant_id,
                    model_id,
                    request_.round_index(),
                    aggregation_result.model_version
                );
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
        responder_.Finish(response_, grpc::Status::OK, MakeTag(Event::FINISH));
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
    Proceed(true, Event::START);
}

void FederatedServiceImpl::StreamGlobalModelCall::Proceed(bool ok, Event event) {
    if (event == Event::FINISH) {
        finish_in_flight_ = false;
        if (MaybeDeleteAfterTerminalEvent()) {
            return;
        }
        return;
    }
    if (event == Event::DONE) {
        done_notify_in_flight_ = false;
        done_received_ = true;
        Logger::Info("StreamGlobalModel cancellation/done notification for client " +
                     request_.client_id());
        if (alarm_in_flight_) {
            alarm_.Cancel();
        }
        MaybeDeleteAfterTerminalEvent();
        return;
    }
    if (event == Event::WRITE) {
        write_in_flight_ = false;
        if (!ok || done_received_) {
            Logger::Info("StreamGlobalModel write completed after stream ended for client " +
                         request_.client_id());
            MaybeDeleteAfterTerminalEvent();
            return;
        }
    }
    if (event == Event::ALARM) {
        alarm_in_flight_ = false;
        if (!ok || done_received_) {
            Logger::Info("StreamGlobalModel alarm completed after stream ended for client " +
                         request_.client_id());
            MaybeDeleteAfterTerminalEvent();
            return;
        }
    }
    if (!ok) {
        delete this;
        return;
    }

    if (status_ == CREATE) {
        status_ = PROCESS;
        RequestDoneNotification();
        service_->RequestStreamGlobalModel(&ctx_, &request_, &responder_, cq_, cq_, MakeTag(Event::REQUEST));
        return;
    }

    if (status_ == PROCESS || status_ == STREAM) {
        std::string tenant_id = NormalizeTenant(request_.tenant_id());
        std::string model_id = NormalizeModel(request_.model_id());

        if (status_ == PROCESS) {
            Logger::Info("Accepted StreamGlobalModel request for client " + request_.client_id() +
                         ", tenant " + tenant_id +
                         ", model " + model_id +
                         ", last_received_round=" + std::to_string(request_.last_received_round()));
            new StreamGlobalModelCall(service_, cq_, crypto_, registry_, state_store_, buffer_, signer_);
            Logger::Info("Spawned next StreamGlobalModel listener");

            std::string validation_error = ValidateFederatedRequestIds(
                request_.tenant_id(),
                request_.model_id(),
                request_.client_id());
            if (!validation_error.empty()) {
                FinishWithStatus(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error));
                return;
            }

            if (!IsPeerAuthenticated(&ctx_)) {
                FinishWithStatus(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                               "Authenticated client certificate required"));
                return;
            }

            std::string peer_identity = NormalizePeerIdentity(GetPeerIdentity(&ctx_));
            Logger::Info("StreamGlobalModel peer identity " + peer_identity);
            if (!registry_.IsRegisteredForPeer(tenant_id, model_id, request_.client_id(), peer_identity)) {
                FinishWithStatus(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                                               "Client must register with the same certificate identity before streaming models"));
                return;
            }

            status_ = STREAM;
            Logger::Info("StreamGlobalModel request passed validation");
        }

        auto latest = buffer_.GetLatestGlobalModel(tenant_id, model_id);
        Logger::Info("Latest global model lookup for stream returned round " +
                     std::to_string(latest.round));
        if (latest.round == -1 ||
            latest.weights.empty() ||
            latest.round == last_sent_round_ ||
            latest.round <= request_.last_received_round()) {
            if (done_received_) {
                MaybeDeleteAfterTerminalEvent();
                return;
            }
            alarm_in_flight_ = true;
            alarm_.Set(cq_,
                       gpr_time_add(gpr_now(GPR_CLOCK_MONOTONIC),
                                    gpr_time_from_seconds(1, GPR_TIMESPAN)),
                       MakeTag(Event::ALARM));
            return;
        }

        last_sent_round_ = latest.round;
        Logger::Info("Streaming global model round " + std::to_string(latest.round) +
                     " to client " + request_.client_id());
        std::string plaintext(reinterpret_cast<const char*>(latest.weights.data()),
                              latest.weights.size() * sizeof(float));

        std::vector<unsigned char> kem_ciphertext;
        std::string key_agreement = "shared-secret";
        std::vector<unsigned char> session_key;
        if (!request_.client_kem_public_key().empty()) {
            std::vector<unsigned char> client_public_key(
                request_.client_kem_public_key().begin(),
                request_.client_kem_public_key().end());
            std::vector<unsigned char> shared_secret;
            if (!CryptoEngine::EncapsulateKem(client_public_key, kem_ciphertext, shared_secret)) {
                FinishWithStatus(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                               "Invalid ML-KEM-1024 client public key"));
                return;
            }
            session_key = CryptoEngine::DeriveSessionKeyFromKemSecret(shared_secret, {
                GetPeerIdentity(&ctx_),
                NormalizeTenant(request_.tenant_id()),
                NormalizeModel(request_.model_id()),
                request_.client_id(),
                "stream"
            });
            key_agreement = "ml-kem-1024";
            Logger::Info("StreamGlobalModel ML-KEM-1024 encapsulation complete for client " +
                         request_.client_id() +
                         ", ciphertext_bytes=" + std::to_string(kem_ciphertext.size()));
        } else {
            session_key = DeriveSessionKey();
            Logger::Info("StreamGlobalModel using shared-secret stream key for client " +
                         request_.client_id());
        }
        std::vector<unsigned char> iv_out;
        std::vector<unsigned char> tag_out;
        std::string ciphertext;
        try {
            ciphertext = CryptoEngine::EncryptWithKey(session_key.data(), plaintext, iv_out, tag_out);
        } catch (const std::exception&) {
            FinishWithStatus(grpc::Status(grpc::StatusCode::INTERNAL,
                                           "Failed to encrypt global model"));
            return;
        }

        std::string server_nonce = CurrentTimestamp();
        std::string to_sign = NormalizeTenant(tenant_id) + "|" +
                              NormalizeModel(model_id) + "|" +
                              std::to_string(latest.round) + "|" +
                              std::to_string(latest.model_version) + "|" +
                              ciphertext;
        Logger::Info("Signing streamed global model round " + std::to_string(latest.round));
        std::vector<unsigned char> sig = signer_(to_sign);
        Logger::Info("Writing streamed global model round " + std::to_string(latest.round));

        pending_update_.Clear();
        pending_update_.set_round_index(latest.round);
        pending_update_.set_encrypted_weights(ciphertext);
        pending_update_.set_iv(std::string(iv_out.begin(), iv_out.end()));
        pending_update_.set_auth_tag(std::string(tag_out.begin(), tag_out.end()));
        pending_update_.set_server_signature(std::string(sig.begin(), sig.end()));
        pending_update_.set_aggregation_method(Config::Instance().Get().aggregation_strategy);
        pending_update_.set_tenant_id(tenant_id);
        pending_update_.set_model_id(model_id);
        pending_update_.set_model_version(latest.model_version);
        pending_update_.set_key_version(crypto_.GetActiveKeyVersion());
        pending_update_.set_server_nonce(server_nonce);
        pending_update_.set_kem_ciphertext(std::string(kem_ciphertext.begin(), kem_ciphertext.end()));
        pending_update_.set_key_agreement(key_agreement);

        write_in_flight_ = true;
        responder_.Write(pending_update_, MakeTag(Event::WRITE));
        return;
    }

    delete this;
}

void FederatedServiceImpl::StreamGlobalModelCall::FinishWithStatus(const grpc::Status& status) {
    status_ = FINISH;
    finish_in_flight_ = true;
    responder_.Finish(status, MakeTag(Event::FINISH));
}

void FederatedServiceImpl::StreamGlobalModelCall::RequestDoneNotification() {
    if (!done_notify_in_flight_) {
        done_notify_in_flight_ = true;
        ctx_.AsyncNotifyWhenDone(MakeTag(Event::DONE));
    }
}

bool FederatedServiceImpl::StreamGlobalModelCall::MaybeDeleteAfterTerminalEvent() {
    if (write_in_flight_ || alarm_in_flight_ || finish_in_flight_ || done_notify_in_flight_) {
        return false;
    }
    if (done_received_ || status_ == FINISH) {
        delete this;
        return true;
    }
    return false;
}
