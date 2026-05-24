#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <string>
#include <thread>
#include <vector>
#include "pqfl.grpc.pb.h"
#include "worker.grpc.pb.h"
#include "crypto_engine.hpp"

namespace {

constexpr char kDefaultAddress[] = "localhost:50051";
constexpr char kWorkerCertIdentity[] = "pqfl-worker";
constexpr char kTenantId[] = "demo-tenant";
constexpr char kModelId[] = "demo-model";
constexpr char kDefaultPayloadSecretPath[] = "/app/data/payload.key";
constexpr char kDefaultServerCertPath[] = "/app/certs/server.crt";

std::string ReadFile(const std::string& filepath) {
    std::ifstream file(filepath);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::vector<float> SyntheticTrain(const std::vector<float>& base, const std::string& worker_id) {
    std::vector<float> weights = base.empty() ? std::vector<float>{0.1f, 0.2f, -0.1f, 0.4f} : base;
    float offset = static_cast<float>((worker_id.size() % 7) + 1) * 0.01f;
    for (size_t i = 0; i < weights.size(); ++i) {
        weights[i] += offset + static_cast<float>(i) * 0.005f;
    }
    return weights;
}

EVP_PKEY* LoadPublicKeyFromCertificate(const std::string& cert_path) {
    FILE* cert_file = fopen(cert_path.c_str(), "r");
    if (!cert_file) {
        return nullptr;
    }

    X509* cert = PEM_read_X509(cert_file, NULL, NULL, NULL);
    fclose(cert_file);
    if (!cert) {
        return nullptr;
    }

    EVP_PKEY* public_key = X509_get_pubkey(cert);
    X509_free(cert);
    return public_key;
}

bool VerifySignature(EVP_PKEY* public_key, const std::string& data, const std::string& signature) {
    if (!public_key) {
        return false;
    }

    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    if (!mdctx) {
        return false;
    }

    bool verified = false;
    if (EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, public_key) > 0 &&
        EVP_DigestVerify(mdctx,
                         reinterpret_cast<const unsigned char*>(signature.data()),
                         signature.size(),
                         reinterpret_cast<const unsigned char*>(data.data()),
                         data.size()) == 1) {
        verified = true;
    }

    EVP_MD_CTX_free(mdctx);
    return verified;
}

} // namespace

class WorkerDemoClient {
public:
    WorkerDemoClient(std::shared_ptr<grpc::Channel> channel,
                     std::vector<unsigned char> payload_secret,
                     std::string server_cert_path,
                     std::string worker_id,
                     std::string trainer_client_id)
        : worker_stub_(pqfl::WorkerCoordinator::NewStub(channel)),
          fl_stub_(pqfl::FederatedLearning::NewStub(channel)),
          payload_secret_(std::move(payload_secret)),
          server_cert_path_(std::move(server_cert_path)),
          worker_id_(std::move(worker_id)),
          trainer_client_id_(std::move(trainer_client_id)) {}

    bool Run() {
        if (!RegisterWorker()) {
            return false;
        }

        pqfl::WorkerTaskLeaseResponse task;
        if (!LeaseTask(&task)) {
            return false;
        }
        if (!task.task_ready()) {
            std::cout << "[" << worker_id_ << "] " << task.message() << std::endl;
            return true;
        }

        ReportStatus(task.task_id(), "running", "worker bootstrapped trainer", 20);
        std::vector<float> base_weights;
        if (!FetchBaseModelIfAvailable(task, &base_weights)) {
            ReportStatus(task.task_id(), "failed", "failed to verify streamed base model", 100);
            return false;
        }
        std::vector<float> trained_weights = SyntheticTrain(base_weights, worker_id_);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        Heartbeat(task.task_id(), "synthetic trainer still running");
        ReportStatus(task.task_id(), "completed", "synthetic trainer finished", 100);

        if (!SubmitWeights(task, trained_weights)) {
            return false;
        }

        std::cout << "[" << worker_id_ << "] Completed task " << task.task_id()
                  << " for round " << task.round_index() << std::endl;
        return true;
    }

private:
    bool RegisterWorker() {
        pqfl::WorkerRegistration req;
        req.set_worker_id(worker_id_);
        req.set_trainer_client_id(trainer_client_id_);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);
        req.set_hostname(worker_id_ + ".local");
        req.set_launcher("reference-worker-demo");
        req.set_max_parallel_tasks(1);
        req.add_labels("reference");
        req.add_labels("cpu");
        req.set_advertised_dataset_size(100);

        pqfl::WorkerRegistrationResponse res;
        grpc::ClientContext ctx;
        grpc::Status status = worker_stub_->RegisterWorker(&ctx, req, &res);
        if (!status.ok()) {
            std::cout << "[" << worker_id_ << "] RegisterWorker failed: "
                      << status.error_message() << std::endl;
            return false;
        }

        trainer_client_id_ = res.trainer_client_id();
        return true;
    }

    bool LeaseTask(pqfl::WorkerTaskLeaseResponse* task) {
        pqfl::WorkerTaskLeaseRequest req;
        req.set_worker_id(worker_id_);
        req.set_trainer_client_id(trainer_client_id_);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);

        grpc::ClientContext ctx;
        grpc::Status status = worker_stub_->LeaseTrainingTask(&ctx, req, task);
        if (!status.ok()) {
            std::cout << "[" << worker_id_ << "] LeaseTrainingTask failed: "
                      << status.error_message() << std::endl;
            return false;
        }
        return true;
    }

    bool FetchBaseModelIfAvailable(const pqfl::WorkerTaskLeaseResponse& task,
                                   std::vector<float>* weights_out) {
        if (task.active_model_version() <= 0 || task.round_index() <= 1) {
            weights_out->clear();
            return true;
        }

        std::vector<unsigned char> stream_public_key;
        std::vector<unsigned char> stream_secret_key;
        if (!CryptoEngine::GenerateKemKeypair(stream_public_key, stream_secret_key)) {
            std::cout << "[" << worker_id_ << "] Failed to generate ML-KEM-1024 stream keypair" << std::endl;
            return false;
        }

        pqfl::ModelRequest req;
        req.set_client_id(trainer_client_id_);
        req.set_last_received_round(std::max(0, task.round_index() - 2));
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);
        req.set_client_kem_public_key(std::string(stream_public_key.begin(), stream_public_key.end()));

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        auto reader = fl_stub_->StreamGlobalModel(&ctx, req);

        pqfl::GlobalModelUpdate update;
        if (!reader->Read(&update)) {
            reader->Finish();
            std::cout << "[" << worker_id_ << "] Failed to stream the base model for round "
                      << task.round_index() << std::endl;
            return false;
        }

        std::vector<unsigned char> session_key;
        if (update.key_agreement() == "ml-kem-1024") {
            std::vector<unsigned char> kem_ciphertext(update.kem_ciphertext().begin(),
                                                      update.kem_ciphertext().end());
            std::vector<unsigned char> shared_secret;
            if (!CryptoEngine::DecapsulateKem(stream_secret_key, kem_ciphertext, shared_secret)) {
                std::cout << "[" << worker_id_ << "] ML-KEM-1024 stream decapsulation failed" << std::endl;
                reader->Finish();
                return false;
            }
            session_key = CryptoEngine::DeriveSessionKeyFromKemSecret(
                shared_secret,
                {kWorkerCertIdentity, kTenantId, kModelId, trainer_client_id_, "stream"});
            std::cout << "[" << worker_id_ << "] ML-KEM-1024 stream decapsulation succeeded, ciphertext_bytes="
                      << kem_ciphertext.size() << std::endl;
        } else {
            session_key = CryptoEngine::DeriveSessionKeyWithSecret(
                payload_secret_,
                {kWorkerCertIdentity, kTenantId, kModelId, trainer_client_id_, "stream"});
            std::cout << "[" << worker_id_ << "] StreamGlobalModel used shared-secret key agreement" << std::endl;
        }
        std::vector<unsigned char> iv(update.iv().begin(), update.iv().end());
        std::vector<unsigned char> tag(update.auth_tag().begin(), update.auth_tag().end());
        std::string plaintext = CryptoEngine::DecryptWithKey(
            session_key.data(),
            update.encrypted_weights(),
            iv,
            tag
        );

        EVP_PKEY* public_key = LoadPublicKeyFromCertificate(server_cert_path_);
        std::string signed_payload = update.tenant_id() + "|" + update.model_id() + "|" +
                                     std::to_string(update.round_index()) + "|" +
                                     std::to_string(update.model_version()) + "|" +
                                     update.encrypted_weights();
        bool signature_ok = VerifySignature(public_key, signed_payload, update.server_signature());
        if (public_key) {
            EVP_PKEY_free(public_key);
        }
        reader->Finish();

        if (!signature_ok) {
            std::cout << "[" << worker_id_ << "] Global model signature verification failed" << std::endl;
            return false;
        }
        if (plaintext.size() % sizeof(float) != 0) {
            std::cout << "[" << worker_id_ << "] Base model had invalid tensor dimensions" << std::endl;
            return false;
        }
        std::vector<float> weights(plaintext.size() / sizeof(float));
        if (!plaintext.empty()) {
            std::memcpy(weights.data(), plaintext.data(), plaintext.size());
        }
        *weights_out = std::move(weights);
        std::cout << "[" << worker_id_ << "] Fetched base model round "
                  << update.round_index() << " via StreamGlobalModel RPC, key_agreement="
                  << update.key_agreement() << std::endl;
        return true;
    }

    void ReportStatus(const std::string& task_id,
                      const std::string& status_value,
                      const std::string& detail,
                      int progress_percent) {
        pqfl::WorkerTaskStatusUpdate req;
        req.set_worker_id(worker_id_);
        req.set_task_id(task_id);
        req.set_status(status_value);
        req.set_detail(detail);
        req.set_progress_percent(progress_percent);
        pqfl::WorkerTaskStatusAck res;
        grpc::ClientContext ctx;
        worker_stub_->ReportTaskStatus(&ctx, req, &res);
    }

    void Heartbeat(const std::string& task_id, const std::string& detail) {
        pqfl::WorkerHeartbeat req;
        req.set_worker_id(worker_id_);
        req.set_trainer_client_id(trainer_client_id_);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);
        req.set_active_task_id(task_id);
        req.set_detail(detail);
        pqfl::WorkerHeartbeatAck res;
        grpc::ClientContext ctx;
        worker_stub_->Heartbeat(&ctx, req, &res);
    }

    bool SubmitWeights(const pqfl::WorkerTaskLeaseResponse& task, const std::vector<float>& weights) {
        pqfl::WeightPayload req;
        req.set_client_id(trainer_client_id_);
        req.set_round_index(task.round_index());
        req.set_local_dataset_size(100);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);
        req.set_base_model_version(task.base_model_version());
        req.set_job_id(task.job_id());
        req.set_assignment_id(task.assignment_id());
        req.set_client_nonce("worker-demo-" + worker_id_ + "-round-" + std::to_string(task.round_index()));

        std::vector<std::string> context = {
            kWorkerCertIdentity,
            kTenantId,
            kModelId,
            trainer_client_id_,
            std::to_string(req.round_index()),
            "submit",
            req.client_nonce()
        };
        std::vector<unsigned char> session_key;
        if (!task.payload_kem_public_key().empty()) {
            std::vector<unsigned char> public_key(task.payload_kem_public_key().begin(),
                                                  task.payload_kem_public_key().end());
            std::vector<unsigned char> kem_ciphertext;
            std::vector<unsigned char> shared_secret;
            if (!CryptoEngine::EncapsulateKem(public_key, kem_ciphertext, shared_secret)) {
                std::cout << "[" << worker_id_ << "] ML-KEM-1024 encapsulation failed" << std::endl;
                return false;
            }
            req.set_kem_ciphertext(std::string(kem_ciphertext.begin(), kem_ciphertext.end()));
            req.set_key_agreement("ml-kem-1024");
            session_key = CryptoEngine::DeriveSessionKeyFromKemSecret(shared_secret, context);
            std::cout << "[" << worker_id_ << "] ML-KEM-1024 encapsulated payload key, ciphertext_bytes="
                      << kem_ciphertext.size() << std::endl;
        } else {
            req.set_key_agreement("shared-secret");
            session_key = CryptoEngine::DeriveSessionKeyWithSecret(payload_secret_, context);
            std::cout << "[" << worker_id_ << "] Using shared-secret payload key" << std::endl;
        }
        std::string plaintext(reinterpret_cast<const char*>(weights.data()), weights.size() * sizeof(float));
        std::vector<unsigned char> iv;
        std::vector<unsigned char> tag;
        std::string ciphertext = CryptoEngine::EncryptWithKey(session_key.data(), plaintext, iv, tag);

        req.set_encrypted_weights(ciphertext);
        req.set_iv(std::string(iv.begin(), iv.end()));
        req.set_auth_tag(std::string(tag.begin(), tag.end()));

        pqfl::AggregationResponse res;
        grpc::ClientContext ctx;
        grpc::Status status = fl_stub_->SubmitWeights(&ctx, req, &res);
        if (!status.ok()) {
            std::cout << "[" << worker_id_ << "] SubmitWeights failed: "
                      << status.error_message() << std::endl;
            return false;
        }

        if (!res.accepted()) {
            std::cout << "[" << worker_id_ << "] SubmitWeights rejected: "
                      << res.message() << std::endl;
            return false;
        }

        std::cout << "[" << worker_id_ << "] " << res.message() << std::endl;
        return true;
    }

    std::unique_ptr<pqfl::WorkerCoordinator::Stub> worker_stub_;
    std::unique_ptr<pqfl::FederatedLearning::Stub> fl_stub_;
    std::vector<unsigned char> payload_secret_;
    std::string server_cert_path_;
    std::string worker_id_;
    std::string trainer_client_id_;
};

int main(int argc, char** argv) {
    const char* env_address = std::getenv("PQFL_ADDRESS");
    const char* env_payload_path = std::getenv("PQFL_PAYLOAD_KEY_PATH");
    std::string address = env_address ? env_address : kDefaultAddress;
    std::string worker_id = argc > 1 ? argv[1] : "demo-worker-1";
    std::string trainer_client_id = argc > 2 ? argv[2] : worker_id + "-trainer";
    std::string payload_path = env_payload_path ? env_payload_path : kDefaultPayloadSecretPath;
    const char* env_server_cert_path = std::getenv("PQFL_SERVER_CERT_PATH");
    std::string server_cert_path = env_server_cert_path ? env_server_cert_path : kDefaultServerCertPath;

    std::string cert_dir = "/app/certs";
    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = ReadFile(cert_dir + "/ca.crt");
    ssl_opts.pem_cert_chain = ReadFile(cert_dir + "/worker.crt");
    ssl_opts.pem_private_key = ReadFile(cert_dir + "/worker.key");
    auto creds = grpc::SslCredentials(ssl_opts);

    std::vector<unsigned char> payload_secret = CryptoEngine::LoadSecretFile(payload_path);
    WorkerDemoClient client(grpc::CreateChannel(address, creds),
                            std::move(payload_secret),
                            std::move(server_cert_path),
                            worker_id,
                            trainer_client_id);
    return client.Run() ? 0 : 1;
}
