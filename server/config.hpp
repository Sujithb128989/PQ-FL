#ifndef CONFIG_HPP
#define CONFIG_HPP

#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

// ---------------------------------------------------------------------------
// Config: Federated Learning Server Configuration
// ---------------------------------------------------------------------------

class Config {
public:
    struct ServerConfig {
        // --- Network ---
        std::string address = "0.0.0.0:50051";
        std::string key_path = "data/master.key";
        std::string payload_key_path = "data/payload.key";
        std::string log_level = "INFO";

        // --- Federated Learning ---
        int required_clients = 2;          // Min clients per aggregation round
        int target_rounds = 100;           // Total FL rounds
        float learning_rate = 0.01f;
        int batch_size = 32;
        std::string aggregation_strategy = "fedavg"; // "fedavg" | "krum" | "trimmed_mean"
        float server_momentum = 0.0f;      // Optional server-side optimizer momentum
        int default_assignment_ttl_seconds = 120;

        // --- Privacy & Robustness ---
        float dp_epsilon = 1.0f;           // Differential privacy budget
        float dp_delta = 1e-5f;            // Differential privacy failure probability
        float clip_norm = 1.0f;            // L2 gradient clipping bound
        float trimmed_mean_beta = 0.1f;    // Fraction to trim (top & bottom) for trimmed_mean

        // --- Persistence & Operations ---
        std::string checkpoint_dir = "models"; // Where to save global model checkpoints
        std::string state_store_path = "data/state.json";
        int audit_event_retention = 2000;
        std::vector<std::string> admin_peer_identities = {"pqfl-admin"};
        std::vector<std::string> worker_peer_identities = {"pqfl-worker"};
    };

    static Config& Instance();

    void Load(const std::string& path);
    const ServerConfig& Get() const;
    void Validate() const;

private:
    Config() = default;
    ServerConfig config_;
};

#endif // CONFIG_HPP
