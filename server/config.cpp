#include "config.hpp"
#include "logger.hpp"
#include <fstream>
#include <iostream>
#include <unordered_set>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

Config& Config::Instance() {
    static Config instance;
    return instance;
}

void Config::Load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::Warn("Config file not found at " + path + ". Using defaults.", "Config");
        return;
    }

    try {
        nlohmann::json j;
        file >> j;

        // --- Network ---
        if (j.contains("address")) config_.address = j["address"];
        if (j.contains("key_path")) config_.key_path = j["key_path"];
        if (j.contains("payload_key_path")) config_.payload_key_path = j["payload_key_path"];
        if (j.contains("log_level")) config_.log_level = j["log_level"];

        // --- Federated Learning ---
        if (j.contains("fl")) {
            auto& fl = j["fl"];
            if (fl.contains("required_clients")) config_.required_clients = fl["required_clients"];
            if (fl.contains("target_rounds")) config_.target_rounds = fl["target_rounds"];
            if (fl.contains("learning_rate")) config_.learning_rate = fl["learning_rate"];
            if (fl.contains("batch_size")) config_.batch_size = fl["batch_size"];
            if (fl.contains("aggregation_strategy")) config_.aggregation_strategy = fl["aggregation_strategy"];
            if (fl.contains("server_momentum")) config_.server_momentum = fl["server_momentum"];
            if (fl.contains("default_assignment_ttl_seconds")) {
                config_.default_assignment_ttl_seconds = fl["default_assignment_ttl_seconds"];
            }
        }

        // --- Privacy & Robustness ---
        if (j.contains("privacy")) {
            auto& priv = j["privacy"];
            if (priv.contains("dp_epsilon")) config_.dp_epsilon = priv["dp_epsilon"];
            if (priv.contains("dp_delta")) config_.dp_delta = priv["dp_delta"];
            if (priv.contains("clip_norm")) config_.clip_norm = priv["clip_norm"];
            if (priv.contains("trimmed_mean_beta")) config_.trimmed_mean_beta = priv["trimmed_mean_beta"];
        }

        // --- Model Persistence ---
        if (j.contains("checkpoint_dir")) config_.checkpoint_dir = j["checkpoint_dir"];
        if (j.contains("state_store_path")) config_.state_store_path = j["state_store_path"];
        if (j.contains("audit_event_retention")) config_.audit_event_retention = j["audit_event_retention"];
        if (j.contains("admin_peer_identities")) config_.admin_peer_identities = j["admin_peer_identities"].get<std::vector<std::string>>();
        if (j.contains("worker_peer_identities")) config_.worker_peer_identities = j["worker_peer_identities"].get<std::vector<std::string>>();

        Validate();

        Logger::Info("Configuration loaded from " + path, "Config");
    } catch (const std::exception& e) {
        Logger::Error("Failed to parse config file: " + std::string(e.what()), "Config");
        throw;
    }
}

const Config::ServerConfig& Config::Get() const {
    return config_;
}

void Config::Validate() const {
    static const std::unordered_set<std::string> allowed_aggregations = {
        "fedavg", "krum", "trimmed_mean"
    };

    Require(!config_.address.empty(), "config.address must not be empty");
    Require(!config_.key_path.empty(), "config.key_path must not be empty");
    Require(!config_.payload_key_path.empty(), "config.payload_key_path must not be empty");
    Require(!config_.checkpoint_dir.empty(), "config.checkpoint_dir must not be empty");
    Require(!config_.state_store_path.empty(), "config.state_store_path must not be empty");
    Require(config_.required_clients >= 1, "config.fl.required_clients must be >= 1");
    Require(config_.target_rounds >= 1, "config.fl.target_rounds must be >= 1");
    Require(config_.learning_rate > 0.0f, "config.fl.learning_rate must be > 0");
    Require(config_.batch_size >= 1, "config.fl.batch_size must be >= 1");
    Require(config_.default_assignment_ttl_seconds >= 30,
            "config.fl.default_assignment_ttl_seconds must be >= 30");
    Require(allowed_aggregations.count(config_.aggregation_strategy) > 0,
            "config.fl.aggregation_strategy must be one of fedavg, krum, trimmed_mean");
    Require(config_.clip_norm >= 0.0f, "config.privacy.clip_norm must be >= 0");
    Require(config_.server_momentum >= 0.0f, "config.fl.server_momentum must be >= 0");
    Require(config_.trimmed_mean_beta >= 0.0f && config_.trimmed_mean_beta < 0.5f,
            "config.privacy.trimmed_mean_beta must be in [0, 0.5)");
    Require(config_.dp_epsilon > 0.0f, "config.privacy.dp_epsilon must be > 0");
    Require(config_.dp_delta > 0.0f && config_.dp_delta < 1.0f,
            "config.privacy.dp_delta must be in (0, 1)");
    Require(!config_.admin_peer_identities.empty(),
            "config.admin_peer_identities must contain at least one identity");
    Require(!config_.worker_peer_identities.empty(),
            "config.worker_peer_identities must contain at least one identity");
}
