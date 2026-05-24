// PQ-FL server entry point.
// Bootstraps crypto, state, and the gRPC services.

#include <iostream>
#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <filesystem>
#include <unistd.h>
#include <linux/limits.h>

#include "admin.grpc.pb.h"
#include "admin_service.hpp"
#include "pqfl.grpc.pb.h"
#include "worker.grpc.pb.h"
#include "worker_service.hpp"
#include "health.grpc.pb.h"
#include "crypto_engine.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "federated_service.hpp"
#include "state_store.hpp"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::health::v1::Health;
using grpc::health::v1::HealthCheckRequest;
using grpc::health::v1::HealthCheckResponse;

// --- Global state ---
std::unique_ptr<CryptoEngine> crypto;
std::unique_ptr<StateStore> state_store;

std::string read_file(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "FATAL: Failed to open file: " << filepath << std::endl;
        exit(EXIT_FAILURE);
    }
    return std::string((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
}

std::string find_cert_dir() {
    char exe_path[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len == -1) return "";
    exe_path[len] = '\0';

    std::filesystem::path current_dir = std::filesystem::path(exe_path).parent_path();
    while (current_dir.has_parent_path() && current_dir != current_dir.parent_path()) {
        std::filesystem::path cert_path = current_dir / "certs" / "ca.crt";
        if (std::filesystem::exists(cert_path)) {
            return (current_dir / "certs").string();
        }
        current_dir = current_dir.parent_path();
    }
    return "";
}

// Health check (standard gRPC health protocol)

class HealthServiceImpl final : public Health::Service {
    Status Check(ServerContext* context, const HealthCheckRequest* request,
                 HealthCheckResponse* response) override {
        response->set_status(HealthCheckResponse::SERVING);
        return Status::OK;
    }

    Status Watch(ServerContext* context, const HealthCheckRequest* request,
                 grpc::ServerWriter<HealthCheckResponse>* writer) override {
        HealthCheckResponse response;
        response.set_status(HealthCheckResponse::SERVING);
        writer->Write(response);
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
        }
        return Status::OK;
    }
};



void RunServer(const std::string& cert_dir) {
    const auto& config = Config::Instance().Get();
    std::string server_address = config.address;

    HealthServiceImpl health_service;
    FederatedServiceImpl fl_service(*crypto, *state_store, cert_dir + "/server.key");
    AdminServiceImpl admin_service(*state_store, *crypto, fl_service.GetWeightBuffer());
    WorkerServiceImpl worker_service(*state_store, fl_service.GetClientRegistry(), *crypto);

    // Configure mTLS with PQ certificates
    grpc::SslServerCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = read_file(cert_dir + "/ca.crt");
    grpc::SslServerCredentialsOptions::PemKeyCertPair pkcp = {
        read_file(cert_dir + "/server.key"),
        read_file(cert_dir + "/server.crt")
    };
    ssl_opts.pem_key_cert_pairs.push_back(pkcp);
    ssl_opts.client_certificate_request = GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;

    auto server_credentials = grpc::SslServerCredentials(ssl_opts);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, server_credentials);
    builder.RegisterService(&health_service);
    builder.RegisterService(&admin_service);
    builder.RegisterService(&worker_service);

    // Register FL async service + get completion queue
    fl_service.Run(builder);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    if (server == nullptr) {
        Logger::Fatal("Failed to build or start the gRPC server.");
        exit(EXIT_FAILURE);
    }

    Logger::Info("=== PQ-FL Orchestration Server ===");
    Logger::Info("Listening on " + server_address);
    Logger::Info("Aggregation: " + config.aggregation_strategy);
    Logger::Info("Required clients: " + std::to_string(config.required_clients));
    Logger::Info("DP (ε=" + std::to_string(config.dp_epsilon) +
                 ", δ=" + std::to_string(config.dp_delta) + ")");
    Logger::Info("Gradient clip norm: " + std::to_string(config.clip_norm));

    fl_service.StartRpcs();

    // Spawn async handler threads
    unsigned int num_threads = std::max(2u, std::thread::hardware_concurrency());

    std::vector<std::thread> cq_threads;
    for (unsigned int i = 0; i < num_threads; ++i) {
        cq_threads.emplace_back([&fl_service]() {
            fl_service.HandleRpcs();
        });
    }

    server->Wait();

    for (auto& t : cq_threads) {
        if (t.joinable()) t.join();
    }
}



int main(int argc, char** argv) {
    // Find certificate directory
    std::string cert_dir = find_cert_dir();
    if (cert_dir.empty()) {
        std::cerr << "FATAL: Could not find certificate directory." << std::endl;
        exit(EXIT_FAILURE);
    }
    std::string root_dir = std::filesystem::path(cert_dir).parent_path().string();

    // Load configuration
    try {
        Config::Instance().Load(root_dir + "/config.json");
        Config::Instance().Validate();
    } catch (const std::exception& e) {
        Logger::Fatal("Configuration error: " + std::string(e.what()));
        return EXIT_FAILURE;
    }

    // Initialize OpenSSL
    Logger::Info("Initializing OpenSSL...");
    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, NULL);
    Logger::Info("OpenSSL version: " + std::string(OpenSSL_version(OPENSSL_VERSION)));
    Logger::Info("Using OQS-enabled OpenSSL (ML-KEM-1024 + ML-DSA-87)");
    Logger::Info("Found certificate directory: " + cert_dir);

    // Initialize crypto engine
    const auto& config = Config::Instance().Get();
    std::filesystem::create_directories("data");
    std::filesystem::create_directories(config.checkpoint_dir);

    Logger::Info("Initializing Crypto Engine...");
    crypto = std::make_unique<CryptoEngine>(config.key_path, config.payload_key_path);
    Logger::Info("Initializing State Store...");
    state_store = std::make_unique<StateStore>(config.state_store_path, config.audit_event_retention);
    state_store->Load();
    state_store->UpdateActiveKeyVersion(crypto->GetActiveKeyVersion());

    // Start server
    RunServer(cert_dir);

    return 0;
}
