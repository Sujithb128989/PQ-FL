#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <grpcpp/grpcpp.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include "pqfl.grpc.pb.h"
#include "crypto_engine.hpp"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using pqfl::AggregationResponse;
using pqfl::ClientRegistration;
using pqfl::ConfigRequest;
using pqfl::FederatedLearning;
using pqfl::GlobalModelUpdate;
using pqfl::ModelRequest;
using pqfl::RegistrationResponse;
using pqfl::TrainingConfig;
using pqfl::WeightPayload;

namespace {

constexpr char kTestCertIdentity[] = "pqfl-edge-client";
constexpr char kTenantId[] = "demo-tenant";
constexpr char kModelId[] = "demo-model";
constexpr char kDefaultAddress[] = "localhost:50051";
constexpr char kDefaultPayloadSecretPath[] = "/app/data/payload.key";

std::string ReadFile(const std::string& filepath) {
    std::ifstream file(filepath);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
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

class FLTestClient {
public:
    FLTestClient(std::shared_ptr<Channel> channel, std::vector<unsigned char> payload_secret)
        : stub_(FederatedLearning::NewStub(channel)),
          payload_secret_(std::move(payload_secret)) {}

    std::string Register(const std::string& cid) {
        ClientRegistration req;
        req.set_client_id(cid);
        req.set_local_dataset_size(100);
        req.set_hardware_info("Test-Edge-Node");
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);
        req.set_dataset_fingerprint("synthetic-demo-v1");

        RegistrationResponse res;
        ClientContext context;

        Status status = stub_->RegisterClient(&context, req, &res);
        if (status.ok()) {
            std::cout << "[Node " << cid << "] Registered. Server round: "
                      << res.current_round() << ", model version: "
                      << res.active_model_version() << std::endl;
            return res.assigned_id();
        }

        std::cout << "[Node " << cid << "] Registration failed: "
                  << status.error_message() << std::endl;
        return "";
    }

    TrainingConfig FetchConfig(const std::string& cid) {
        ConfigRequest req;
        req.set_client_id(cid);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);

        TrainingConfig res;
        ClientContext context;
        Status status = stub_->GetTrainingConfig(&context, req, &res);
        if (!status.ok()) {
            std::cout << "[Node " << cid << "] GetTrainingConfig failed: "
                      << status.error_message() << std::endl;
        } else {
            std::cout << "[Node " << cid << "] Config received. assignment_ready="
                      << (res.assignment_ready() ? "true" : "false")
                      << ", active_model_version=" << res.active_model_version()
                      << ", job_status=" << (res.job_status().empty() ? "idle" : res.job_status())
                      << std::endl;
        }
        return res;
    }

    void SendWeights(const std::string& cid, const std::vector<float>& weights, const TrainingConfig& config) {
        WeightPayload req;
        req.set_client_id(cid);
        req.set_round_index(config.assigned_round_index() > 0 ? config.assigned_round_index() : 1);
        req.set_local_dataset_size(100);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);
        req.set_base_model_version(config.base_model_version());
        req.set_job_id(config.job_id());
        req.set_assignment_id(config.assignment_id());
        req.set_client_nonce("nonce-" + cid + "-round-" + std::to_string(req.round_index()));

        std::vector<std::string> key_context = {
            kTestCertIdentity,
            kTenantId,
            kModelId,
            cid,
            std::to_string(req.round_index()),
            "submit",
            req.client_nonce()
        };
        std::vector<unsigned char> session_key;
        if (!config.payload_kem_public_key().empty()) {
            std::vector<unsigned char> public_key(config.payload_kem_public_key().begin(),
                                                  config.payload_kem_public_key().end());
            std::vector<unsigned char> kem_ciphertext;
            std::vector<unsigned char> shared_secret;
            if (!CryptoEngine::EncapsulateKem(public_key, kem_ciphertext, shared_secret)) {
                std::cout << "[Node " << cid << "] ML-KEM-1024 encapsulation failed" << std::endl;
                return;
            }
            req.set_kem_ciphertext(std::string(kem_ciphertext.begin(), kem_ciphertext.end()));
            req.set_key_agreement("ml-kem-1024");
            session_key = CryptoEngine::DeriveSessionKeyFromKemSecret(shared_secret, key_context);
            std::cout << "[Node " << cid << "] ML-KEM-1024 payload key encapsulated, ciphertext_bytes="
                      << kem_ciphertext.size() << std::endl;
        } else {
            req.set_key_agreement("shared-secret");
            session_key = CryptoEngine::DeriveSessionKeyWithSecret(payload_secret_, key_context);
        }
        std::string plaintext(reinterpret_cast<const char*>(weights.data()),
                              weights.size() * sizeof(float));
        std::vector<unsigned char> iv, tag;
        std::string ciphertext = CryptoEngine::EncryptWithKey(session_key.data(), plaintext, iv, tag);

        req.set_encrypted_weights(ciphertext);
        req.set_iv(std::string(iv.begin(), iv.end()));
        req.set_auth_tag(std::string(tag.begin(), tag.end()));

        AggregationResponse res;
        ClientContext context;
        Status status = stub_->SubmitWeights(&context, req, &res);
        if (status.ok()) {
            std::cout << "[Node " << cid << "] " << res.message() << std::endl;
            return;
        }

        std::cout << "[Node " << cid << "] SendWeights failed: "
                  << status.error_message() << std::endl;
    }

    void ReceiveOneGlobalModel(const std::string& cid, const std::string& server_cert_path) {
        ModelRequest req;
        req.set_client_id(cid);
        req.set_last_received_round(0);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);
        std::vector<unsigned char> stream_public_key;
        std::vector<unsigned char> stream_secret_key;
        if (CryptoEngine::GenerateKemKeypair(stream_public_key, stream_secret_key)) {
            req.set_client_kem_public_key(std::string(stream_public_key.begin(), stream_public_key.end()));
        }

        ClientContext context;
        auto reader = stub_->StreamGlobalModel(&context, req);

        GlobalModelUpdate update;
        if (!reader->Read(&update)) {
            Status status = reader->Finish();
            std::cout << "[Node " << cid << "] StreamGlobalModel failed: "
                      << status.error_message() << std::endl;
            return;
        }

        std::vector<unsigned char> session_key;
        if (update.key_agreement() == "ml-kem-1024") {
            std::vector<unsigned char> kem_ciphertext(update.kem_ciphertext().begin(),
                                                      update.kem_ciphertext().end());
            std::vector<unsigned char> shared_secret;
            if (!CryptoEngine::DecapsulateKem(stream_secret_key, kem_ciphertext, shared_secret)) {
                std::cout << "[Node " << cid << "] Stream ML-KEM-1024 decapsulation failed" << std::endl;
                return;
            }
            session_key = CryptoEngine::DeriveSessionKeyFromKemSecret(
                shared_secret,
                {kTestCertIdentity, kTenantId, kModelId, cid, "stream"});
            std::cout << "[Node " << cid << "] Stream ML-KEM-1024 decapsulation succeeded, ciphertext_bytes="
                      << kem_ciphertext.size() << std::endl;
        } else {
            session_key = CryptoEngine::DeriveSessionKeyWithSecret(
                payload_secret_,
                {kTestCertIdentity, kTenantId, kModelId, cid, "stream"});
        }
        std::vector<unsigned char> iv(update.iv().begin(), update.iv().end());
        std::vector<unsigned char> tag(update.auth_tag().begin(), update.auth_tag().end());
        std::string plaintext = CryptoEngine::DecryptWithKey(
            session_key.data(),
            update.encrypted_weights(),
            iv,
            tag
        );

        EVP_PKEY* public_key = LoadPublicKeyFromCertificate(server_cert_path);
        std::string signed_payload = update.tenant_id() + "|" + update.model_id() + "|" +
                                     std::to_string(update.round_index()) + "|" +
                                     std::to_string(update.model_version()) + "|" +
                                     update.encrypted_weights();
        bool signature_ok = VerifySignature(public_key, signed_payload, update.server_signature());
        if (public_key) {
            EVP_PKEY_free(public_key);
        }

        size_t float_count = plaintext.size() / sizeof(float);
        std::vector<float> weights(float_count);
        if (!plaintext.empty()) {
            std::memcpy(weights.data(), plaintext.data(), plaintext.size());
        }

        context.TryCancel();
        Status finish_status = reader->Finish();
        if (!finish_status.ok() && finish_status.error_code() != grpc::StatusCode::CANCELLED) {
            std::cout << "[Node " << cid << "] Stream closed with: "
                      << finish_status.error_message() << std::endl;
        }

        std::cout << "[Node " << cid << "] Received global model round "
                  << update.round_index() << " via " << update.aggregation_method()
                  << ", version=" << update.model_version()
                  << ", key_agreement=" << update.key_agreement()
                  << ", signature verified=" << (signature_ok ? "true" : "false")
                  << ", params=" << float_count << std::endl;
    }

    bool StreamCancellationTest(const std::string& cid) {
        Register(cid);
        ModelRequest req;
        req.set_client_id(cid);
        req.set_last_received_round(0);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);

        ClientContext context;
        auto reader = stub_->StreamGlobalModel(&context, req);
        context.TryCancel();
        GlobalModelUpdate update;
        (void)reader->Read(&update);
        Status status = reader->Finish();
        bool passed = status.error_code() == grpc::StatusCode::CANCELLED;
        std::cout << "[StreamTest] cancellation_mid_stream=" << (passed ? "PASS" : "FAIL")
                  << " code=" << status.error_code()
                  << " message=" << status.error_message() << std::endl;
        return passed;
    }

    bool SlowConsumerBackpressureTest(const std::string& cid) {
        Register(cid);
        ModelRequest req;
        req.set_client_id(cid);
        req.set_last_received_round(0);
        req.set_tenant_id(kTenantId);
        req.set_model_id(kModelId);

        ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        auto reader = stub_->StreamGlobalModel(&context, req);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        GlobalModelUpdate update;
        bool read_ok = reader->Read(&update);
        context.TryCancel();
        Status status = reader->Finish();
        bool passed = read_ok && update.round_index() > 0 &&
                      (!status.ok() || status.error_code() == grpc::StatusCode::CANCELLED);
        std::cout << "[StreamTest] slow_consumer_backpressure=" << (passed ? "PASS" : "FAIL")
                  << " read_ok=" << (read_ok ? "true" : "false")
                  << " round=" << update.round_index()
                  << " finish_code=" << status.error_code() << std::endl;
        return passed;
    }

    bool ConcurrentStreamTest() {
        std::vector<std::thread> threads;
        std::vector<bool> results(3, false);
        for (int i = 0; i < 3; ++i) {
            threads.emplace_back([this, i, &results]() {
                std::string cid = "stream-concurrent-" + std::to_string(i + 1);
                Register(cid);
                ModelRequest req;
                req.set_client_id(cid);
                req.set_last_received_round(0);
                req.set_tenant_id(kTenantId);
                req.set_model_id(kModelId);
                ClientContext context;
                context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
                auto reader = stub_->StreamGlobalModel(&context, req);
                GlobalModelUpdate update;
                bool read_ok = reader->Read(&update);
                context.TryCancel();
                Status status = reader->Finish();
                results[i] = read_ok && update.round_index() > 0 &&
                             (!status.ok() || status.error_code() == grpc::StatusCode::CANCELLED);
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }
        bool passed = true;
        for (bool result : results) {
            passed = passed && result;
        }
        std::cout << "[StreamTest] concurrent_worker_connections=" << (passed ? "PASS" : "FAIL")
                  << " clients=" << results.size() << std::endl;
        return passed;
    }

private:
    std::unique_ptr<FederatedLearning::Stub> stub_;
    std::vector<unsigned char> payload_secret_;
};

int main(int argc, char** argv) {
    std::cout << "\n>>> ALLOCATING PQ-mTLS SECURE CHANNELS...\n";
    bool stream_tests_only = argc > 1 && std::string(argv[1]) == "--stream-tests";
    const char* env_address = std::getenv("PQFL_ADDRESS");
    std::string cert_dir = "/app/certs";
    const char* env_payload_path = std::getenv("PQFL_PAYLOAD_KEY_PATH");
    std::string address = env_address ? env_address : kDefaultAddress;
    std::string payload_path = env_payload_path ? env_payload_path : kDefaultPayloadSecretPath;
    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = ReadFile(cert_dir + "/ca.crt");
    ssl_opts.pem_cert_chain = ReadFile(cert_dir + "/client.crt");
    ssl_opts.pem_private_key = ReadFile(cert_dir + "/client.key");

    auto creds = grpc::SslCredentials(ssl_opts);
    std::vector<unsigned char> payload_secret = CryptoEngine::LoadSecretFile(payload_path);
    FLTestClient client1(grpc::CreateChannel(address, creds), payload_secret);
    FLTestClient client2(grpc::CreateChannel(address, creds), payload_secret);

    if (stream_tests_only) {
        client1.Register("stream-seed-1");
        client2.Register("stream-seed-2");
        TrainingConfig seed_cfg1 = client1.FetchConfig("stream-seed-1");
        TrainingConfig seed_cfg2 = client2.FetchConfig("stream-seed-2");
        client1.SendWeights("stream-seed-1", {0.1f, 0.2f, -0.1f, 0.4f}, seed_cfg1);
        client2.SendWeights("stream-seed-2", {0.2f, 0.1f, -0.2f, 0.3f}, seed_cfg2);

        bool ok = true;
        ok = client1.StreamCancellationTest("stream-cancel-client") && ok;
        ok = client1.SlowConsumerBackpressureTest("stream-slow-client") && ok;
        ok = client1.ConcurrentStreamTest() && ok;
        std::cout << "[StreamTest] async_cq_stream_tests=" << (ok ? "PASS" : "FAIL") << std::endl;
        return ok ? 0 : 1;
    }

    std::cout << ">>> EDGE CLIENTS CONNECTING...\n\n";
    client1.Register("edge-client-1");
    client2.Register("edge-client-2");
    TrainingConfig cfg1 = client1.FetchConfig("edge-client-1");
    TrainingConfig cfg2 = client2.FetchConfig("edge-client-2");

    std::vector<float> w1 = {0.1f, 0.5f, -0.2f, 0.8f};
    std::vector<float> w2 = {0.2f, 0.4f, -0.1f, 0.9f};

    std::cout << "\n>>> TRANSMITTING ENCRYPTED WEIGHT TENSORS...\n";
    client1.SendWeights("edge-client-1", w1, cfg1);
    client2.SendWeights("edge-client-2", w2, cfg2);

    std::cout << "\n>>> RECEIVING AND VERIFYING SIGNED GLOBAL MODEL...\n";
    client1.ReceiveOneGlobalModel("edge-client-1", cert_dir + "/server.crt");

    return 0;
}
