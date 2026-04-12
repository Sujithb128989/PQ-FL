#ifndef STATE_STORE_HPP
#define STATE_STORE_HPP

#include <mutex>
#include <string>
#include <vector>

struct PersistedClientRecord {
    std::string tenant_id;
    std::string model_id;
    std::string client_id;
    std::string peer_identity;
    int local_dataset_size = 0;
    std::string hardware_info;
    std::string dataset_fingerprint;
    int last_active_round = -1;
    std::string registered_at;
};

struct PersistedModelRecord {
    std::string tenant_id;
    std::string model_id;
    int version = 0;
    int latest_round = -1;
    std::string checkpoint_path;
    std::string aggregation_strategy;
    int key_version = 1;
    int client_count = 0;
    int weight_dimension = 0;
    bool active = false;
    std::string created_at;
};

struct PersistedDatasetRecord {
    std::string tenant_id;
    std::string model_id;
    std::string dataset_id;
    std::string fingerprint;
    std::string storage_uri;
    std::string format;
    int sample_count = 0;
    int shard_count = 0;
    bool active = true;
    std::string created_at;
};

struct PersistedTrainingJobRecord {
    std::string job_id;
    std::string tenant_id;
    std::string model_id;
    std::string dataset_id;
    int target_rounds = 0;
    int planned_clients = 0;
    int min_clients_per_round = 0;
    int assignment_ttl_seconds = 120;
    std::string status;
    int current_round = 1;
    int completed_rounds = 0;
    int active_model_version = 0;
    std::string created_at;
    std::string activated_at;
    std::string notes;
};

struct PersistedAssignmentLease {
    std::string assignment_id;
    std::string job_id;
    std::string tenant_id;
    std::string model_id;
    std::string dataset_id;
    std::string client_id;
    int round_index = 0;
    int base_model_version = 0;
    std::string status;
    std::string issued_at;
    std::string expires_at;
    std::string updated_at;
    std::string detail;
};

struct PersistedWorkerRecord {
    std::string worker_id;
    std::string trainer_client_id;
    std::string tenant_id;
    std::string model_id;
    std::string hostname;
    std::string launcher;
    int max_parallel_tasks = 1;
    std::vector<std::string> labels;
    std::string status;
    std::string registered_at;
    std::string last_heartbeat_at;
    std::string active_task_id;
};

struct PersistedWorkerTaskRecord {
    std::string task_id;
    std::string worker_id;
    std::string trainer_client_id;
    std::string job_id;
    std::string assignment_id;
    std::string tenant_id;
    std::string model_id;
    std::string dataset_id;
    int round_index = 0;
    int base_model_version = 0;
    std::string status;
    std::string detail;
    int progress_percent = 0;
    std::string issued_at;
    std::string started_at;
    std::string completed_at;
    std::string updated_at;
};

struct PersistedAuditEvent {
    std::string timestamp;
    std::string action;
    std::string tenant_id;
    std::string model_id;
    std::string client_id;
    std::string detail;
};

struct PersistedSystemStatus {
    int registered_clients = 0;
    int completed_rounds = 0;
    int total_submissions = 0;
    int active_key_version = 1;
    std::string state_path;
    std::vector<PersistedModelRecord> active_models;
    int registered_datasets = 0;
    int stored_models = 0;
    int active_training_jobs = 0;
    int pending_assignments = 0;
    int leased_assignments = 0;
    int submitted_assignments = 0;
    int registered_workers = 0;
    int busy_workers = 0;
    int active_worker_tasks = 0;
    int failed_worker_tasks = 0;
};

class StateStore {
public:
    explicit StateStore(const std::string& path, size_t audit_retention = 2000);

    void Load();

    void RecordClientRegistration(const PersistedClientRecord& record);
    void UpdateClientLastRound(const std::string& tenant_id,
                               const std::string& model_id,
                               const std::string& client_id,
                               int round);
    void RecordWeightSubmission(const std::string& tenant_id,
                                const std::string& model_id,
                                const std::string& client_id,
                                int round);
    int RecordAggregatedModel(const std::string& tenant_id,
                              const std::string& model_id,
                              int round,
                              const std::string& checkpoint_path,
                              const std::string& aggregation_strategy,
                              int key_version,
                              int client_count,
                              int weight_dimension);
    bool RollbackModel(const std::string& tenant_id,
                       const std::string& model_id,
                       int target_version,
                       PersistedModelRecord* restored_model);
    void UpdateActiveKeyVersion(int active_key_version);
    void RecordAuditEvent(const PersistedAuditEvent& event);

    PersistedDatasetRecord UpsertDataset(const PersistedDatasetRecord& record);
    std::vector<PersistedDatasetRecord> ListDatasets(const std::string& tenant_filter = "",
                                                     const std::string& model_filter = "") const;
    bool GetDataset(const std::string& tenant_id,
                    const std::string& model_id,
                    const std::string& dataset_id,
                    PersistedDatasetRecord* dataset) const;

    PersistedTrainingJobRecord CreateTrainingJob(const PersistedTrainingJobRecord& record);
    std::vector<PersistedTrainingJobRecord> ListTrainingJobs(const std::string& tenant_filter = "",
                                                             const std::string& model_filter = "",
                                                             const std::string& status_filter = "") const;
    bool GetActiveTrainingJob(const std::string& tenant_id,
                              const std::string& model_id,
                              PersistedTrainingJobRecord* job) const;
    bool LeaseTrainingAssignment(const std::string& tenant_id,
                                 const std::string& model_id,
                                 const std::string& client_id,
                                 int active_model_version,
                                 int default_min_clients,
                                 int default_assignment_ttl_seconds,
                                 PersistedAssignmentLease* assignment_out,
                                 PersistedTrainingJobRecord* job_out,
                                 PersistedDatasetRecord* dataset_out);
    bool AcceptTrainingAssignmentSubmission(const std::string& tenant_id,
                                            const std::string& model_id,
                                            const std::string& client_id,
                                            const std::string& job_id,
                                            const std::string& assignment_id,
                                            int round,
                                            int base_model_version,
                                            std::string* error_message);
    bool ValidateTrainingAssignmentSubmission(const std::string& tenant_id,
                                             const std::string& model_id,
                                             const std::string& client_id,
                                             const std::string& job_id,
                                             const std::string& assignment_id,
                                             int round,
                                             int base_model_version,
                                             std::string* error_message);
    void AdvanceTrainingJobRound(const std::string& tenant_id,
                                 const std::string& model_id,
                                 int round,
                                 int active_model_version);
    PersistedWorkerRecord UpsertWorker(const PersistedWorkerRecord& record);
    bool GetWorker(const std::string& worker_id, PersistedWorkerRecord* worker) const;
    std::vector<PersistedWorkerRecord> ListWorkers(const std::string& tenant_filter = "",
                                                   const std::string& model_filter = "") const;
    PersistedWorkerTaskRecord LeaseWorkerTask(const PersistedWorkerRecord& worker,
                                              const PersistedAssignmentLease& assignment,
                                              const PersistedTrainingJobRecord& job,
                                              const PersistedDatasetRecord& dataset);
    bool UpdateWorkerTaskStatus(const std::string& worker_id,
                                const std::string& task_id,
                                const std::string& status,
                                const std::string& detail,
                                int progress_percent,
                                std::string* error_message);
    bool RecordWorkerHeartbeat(const std::string& worker_id,
                               const std::string& active_task_id,
                               const std::string& detail);
    std::vector<PersistedWorkerTaskRecord> ListWorkerTasks(const std::string& tenant_filter = "",
                                                           const std::string& model_filter = "",
                                                           const std::string& worker_filter = "",
                                                           const std::string& status_filter = "") const;
    bool GetActiveModelRecord(const std::string& tenant_id,
                              const std::string& model_id,
                              PersistedModelRecord* model) const;

    PersistedSystemStatus GetSystemStatus() const;
    std::vector<PersistedModelRecord> ListModels(const std::string& tenant_filter = "",
                                                 const std::string& model_filter = "") const;
    std::vector<PersistedClientRecord> ListClients(const std::string& tenant_filter = "",
                                                   const std::string& model_filter = "") const;
    std::vector<PersistedAuditEvent> ListAuditEvents(size_t limit) const;
    std::vector<PersistedModelRecord> GetActiveModels() const;
    int GetActiveModelVersion(const std::string& tenant_id, const std::string& model_id) const;
    std::string GetStatePath() const { return path_; }

private:
    mutable std::mutex mutex_;
    std::string path_;
    size_t audit_retention_;
    std::vector<PersistedClientRecord> clients_;
    std::vector<PersistedModelRecord> models_;
    std::vector<PersistedDatasetRecord> datasets_;
    std::vector<PersistedTrainingJobRecord> training_jobs_;
    std::vector<PersistedAssignmentLease> assignments_;
    std::vector<PersistedWorkerRecord> workers_;
    std::vector<PersistedWorkerTaskRecord> worker_tasks_;
    std::vector<PersistedAuditEvent> audit_events_;
    int completed_rounds_ = 0;
    int total_submissions_ = 0;
    int active_key_version_ = 1;

    void SaveLocked() const;
    void TrimAuditEventsLocked();
    void ExpireAssignmentsLocked(const std::string& now);
    static std::string NormalizeTenant(const std::string& tenant_id);
    static std::string NormalizeModel(const std::string& model_id);
};

#endif // STATE_STORE_HPP
