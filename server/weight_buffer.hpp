#ifndef WEIGHT_BUFFER_HPP
#define WEIGHT_BUFFER_HPP

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "crypto_engine.hpp"
#include "state_store.hpp"

/**
 * @struct LatestModelSnapshot
 * @brief Represents the latest aggregated global model weights.
 */
struct LatestModelSnapshot {
    int round = -1;
    int model_version = 0;
    std::vector<float> weights;
};

/**
 * @struct AggregationAttemptResult
 * @brief Result of an attempt to aggregate client weights.
 */
struct AggregationAttemptResult {
    bool accepted = false;
    bool aggregated = false;
    int model_version = 0;
    std::string message;
};

/**
 * @class WeightBuffer
 * @brief Buffer for accumulating client model weights and executing federated aggregation.
 */
class WeightBuffer {
public:
    /**
     * @brief Adds a client's weights to the buffer and attempts aggregation if the threshold is met.
     */
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

    /**
     * @brief Checks if a specific round has been successfully completed.
     */
    bool IsRoundCompleted(const std::string& tenant_id,
                          const std::string& model_id,
                          int round);

    /**
     * @brief Checks if a specific client has submitted weights for a round.
     */
    bool HasClientSubmission(const std::string& tenant_id,
                             const std::string& model_id,
                             int round,
                             const std::string& client_id);

    /**
     * @brief Returns the number of client submissions received for a round.
     */
    size_t GetNumClients(const std::string& tenant_id,
                         const std::string& model_id,
                         int round);

    /**
     * @brief Retrieves the latest aggregated global model snapshot.
     */
    LatestModelSnapshot GetLatestGlobalModel(const std::string& tenant_id,
                                             const std::string& model_id);

    /**
     * @brief Restores a previously aggregated global model into the buffer (e.g., on restart).
     */
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

#endif // WEIGHT_BUFFER_HPP
