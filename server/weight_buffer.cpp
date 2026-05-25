#include "weight_buffer.hpp"
#include "federated_helpers.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

using namespace pqfl_helpers;

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
    ClipWeights(weights, clip_norm);

    Logger::Info("Applied DP noise (sigma=" + std::to_string(sigma) +
                 ", epsilon=" + std::to_string(epsilon) +
                 ", post_noise_clip_norm=" + std::to_string(clip_norm) + ")");
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
