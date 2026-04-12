#include "state_store.hpp"
#include "logger.hpp"
#include "utils.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

namespace {



bool MatchesFilter(const std::string& value, const std::string& filter) {
    return filter.empty() || value == filter;
}

bool IsActiveJobStatus(const std::string& status) {
    return status == "planned" || status == "running";
}

#ifdef __linux__
void FsyncPathIfPossible(const std::filesystem::path& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
}

void FsyncDirectoryIfPossible(const std::filesystem::path& path) {
    std::filesystem::path directory = path.parent_path();
    if (directory.empty()) {
        directory = ".";
    }

    int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        return;
    }
    ::fsync(fd);
    ::close(fd);
}
#else
void FsyncPathIfPossible(const std::filesystem::path&) {}
void FsyncDirectoryIfPossible(const std::filesystem::path&) {}
#endif

} // namespace

StateStore::StateStore(const std::string& path, size_t audit_retention)
    : path_(path), audit_retention_(std::max<size_t>(1, audit_retention)) {}

std::string StateStore::NormalizeTenant(const std::string& tenant_id) {
    return tenant_id.empty() ? "default-tenant" : tenant_id;
}

std::string StateStore::NormalizeModel(const std::string& model_id) {
    return model_id.empty() ? "default-model" : model_id;
}

void StateStore::TrimAuditEventsLocked() {
    if (audit_events_.size() > audit_retention_) {
        audit_events_.erase(audit_events_.begin(),
                            audit_events_.begin() + (audit_events_.size() - audit_retention_));
    }
}

void StateStore::ExpireAssignmentsLocked(const std::string& now) {
    bool mutated = false;
    for (auto& assignment : assignments_) {
        if (assignment.status == "leased" && !assignment.expires_at.empty() && assignment.expires_at <= now) {
            assignment.status = "expired";
            assignment.updated_at = now;
            assignment.detail = "Lease expired before submission";
            mutated = true;
            audit_events_.push_back({
                now, "assignment_expired", assignment.tenant_id, assignment.model_id, assignment.client_id,
                "Assignment " + assignment.assignment_id + " expired"
            });
        }
    }
    if (mutated) {
        TrimAuditEventsLocked();
    }
}

void StateStore::Load() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::ifstream file(path_);
    if (!file.is_open()) {
        std::filesystem::create_directories(std::filesystem::path(path_).parent_path());
        SaveLocked();
        Logger::Info("Initialized empty state store at " + path_, "StateStore");
        return;
    }

    try {
        json j;
        file >> j;
        clients_.clear();
        models_.clear();
        datasets_.clear();
        training_jobs_.clear();
        assignments_.clear();
        workers_.clear();
        worker_tasks_.clear();
        audit_events_.clear();
        completed_rounds_ = j.value("completed_rounds", 0);
        total_submissions_ = j.value("total_submissions", 0);
        active_key_version_ = j.value("active_key_version", 1);

        if (j.contains("clients")) {
            for (const auto& client : j["clients"]) {
                clients_.push_back({
                    NormalizeTenant(client.value("tenant_id", "")),
                    NormalizeModel(client.value("model_id", "")),
                    client.value("client_id", ""),
                    client.value("peer_identity", ""),
                    client.value("local_dataset_size", 0),
                    client.value("hardware_info", ""),
                    client.value("dataset_fingerprint", ""),
                    client.value("last_active_round", -1),
                    client.value("registered_at", CurrentTimestamp())
                });
            }
        }
        if (j.contains("models")) {
            for (const auto& model : j["models"]) {
                models_.push_back({
                    NormalizeTenant(model.value("tenant_id", "")),
                    NormalizeModel(model.value("model_id", "")),
                    model.value("version", 0),
                    model.value("latest_round", -1),
                    model.value("checkpoint_path", ""),
                    model.value("aggregation_strategy", "fedavg"),
                    model.value("key_version", 1),
                    model.value("client_count", 0),
                    model.value("weight_dimension", 0),
                    model.value("active", false),
                    model.value("created_at", CurrentTimestamp())
                });
            }
        }
        if (j.contains("datasets")) {
            for (const auto& dataset : j["datasets"]) {
                datasets_.push_back({
                    NormalizeTenant(dataset.value("tenant_id", "")),
                    NormalizeModel(dataset.value("model_id", "")),
                    dataset.value("dataset_id", ""),
                    dataset.value("fingerprint", ""),
                    dataset.value("storage_uri", ""),
                    dataset.value("format", ""),
                    dataset.value("sample_count", 0),
                    dataset.value("shard_count", 0),
                    dataset.value("active", true),
                    dataset.value("created_at", CurrentTimestamp())
                });
            }
        }
        if (j.contains("training_jobs")) {
            for (const auto& job : j["training_jobs"]) {
                training_jobs_.push_back({
                    job.value("job_id", ""),
                    NormalizeTenant(job.value("tenant_id", "")),
                    NormalizeModel(job.value("model_id", "")),
                    job.value("dataset_id", ""),
                    job.value("target_rounds", 0),
                    job.value("planned_clients", 0),
                    job.value("min_clients_per_round", 0),
                    job.value("assignment_ttl_seconds", 120),
                    job.value("status", "planned"),
                    job.value("current_round", 1),
                    job.value("completed_rounds", 0),
                    job.value("active_model_version", 0),
                    job.value("created_at", CurrentTimestamp()),
                    job.value("activated_at", ""),
                    job.value("notes", "")
                });
            }
        }
        if (j.contains("assignments")) {
            for (const auto& assignment : j["assignments"]) {
                assignments_.push_back({
                    assignment.value("assignment_id", ""),
                    assignment.value("job_id", ""),
                    NormalizeTenant(assignment.value("tenant_id", "")),
                    NormalizeModel(assignment.value("model_id", "")),
                    assignment.value("dataset_id", ""),
                    assignment.value("client_id", ""),
                    assignment.value("round_index", 0),
                    assignment.value("base_model_version", 0),
                    assignment.value("status", "leased"),
                    assignment.value("issued_at", ""),
                    assignment.value("expires_at", ""),
                    assignment.value("updated_at", ""),
                    assignment.value("detail", "")
                });
            }
        }
        if (j.contains("workers")) {
            for (const auto& worker : j["workers"]) {
                PersistedWorkerRecord record;
                record.worker_id = worker.value("worker_id", "");
                record.trainer_client_id = worker.value("trainer_client_id", "");
                record.tenant_id = NormalizeTenant(worker.value("tenant_id", ""));
                record.model_id = NormalizeModel(worker.value("model_id", ""));
                record.hostname = worker.value("hostname", "");
                record.launcher = worker.value("launcher", "");
                record.max_parallel_tasks = worker.value("max_parallel_tasks", 1);
                if (worker.contains("labels")) {
                    record.labels = worker["labels"].get<std::vector<std::string>>();
                }
                record.status = worker.value("status", "idle");
                record.registered_at = worker.value("registered_at", CurrentTimestamp());
                record.last_heartbeat_at = worker.value("last_heartbeat_at", "");
                record.active_task_id = worker.value("active_task_id", "");
                workers_.push_back(record);
            }
        }
        if (j.contains("worker_tasks")) {
            for (const auto& task : j["worker_tasks"]) {
                worker_tasks_.push_back({
                    task.value("task_id", ""),
                    task.value("worker_id", ""),
                    task.value("trainer_client_id", ""),
                    task.value("job_id", ""),
                    task.value("assignment_id", ""),
                    NormalizeTenant(task.value("tenant_id", "")),
                    NormalizeModel(task.value("model_id", "")),
                    task.value("dataset_id", ""),
                    task.value("round_index", 0),
                    task.value("base_model_version", 0),
                    task.value("status", "leased"),
                    task.value("detail", ""),
                    task.value("progress_percent", 0),
                    task.value("issued_at", ""),
                    task.value("started_at", ""),
                    task.value("completed_at", ""),
                    task.value("updated_at", "")
                });
            }
        }
        if (j.contains("audit_events")) {
            for (const auto& event : j["audit_events"]) {
                audit_events_.push_back({
                    event.value("timestamp", CurrentTimestamp()),
                    event.value("action", ""),
                    NormalizeTenant(event.value("tenant_id", "")),
                    NormalizeModel(event.value("model_id", "")),
                    event.value("client_id", ""),
                    event.value("detail", "")
                });
            }
        }

        ExpireAssignmentsLocked(CurrentTimestamp());
        TrimAuditEventsLocked();
        SaveLocked();
        Logger::Info("Loaded state store from " + path_, "StateStore");
    } catch (const std::exception& e) {
        Logger::Error("Failed to load state store: " + std::string(e.what()), "StateStore");
        clients_.clear();
        models_.clear();
        datasets_.clear();
        training_jobs_.clear();
        assignments_.clear();
        workers_.clear();
        worker_tasks_.clear();
        audit_events_.clear();
        completed_rounds_ = 0;
        total_submissions_ = 0;
        active_key_version_ = 1;
        SaveLocked();
    }
}

void StateStore::SaveLocked() const {
    json j;
    j["completed_rounds"] = completed_rounds_;
    j["total_submissions"] = total_submissions_;
    j["active_key_version"] = active_key_version_;
    j["clients"] = json::array();
    j["models"] = json::array();
    j["datasets"] = json::array();
    j["training_jobs"] = json::array();
    j["assignments"] = json::array();
    j["workers"] = json::array();
    j["worker_tasks"] = json::array();
    j["audit_events"] = json::array();

    for (const auto& client : clients_) {
        j["clients"].push_back({
            {"tenant_id", client.tenant_id}, {"model_id", client.model_id}, {"client_id", client.client_id},
            {"peer_identity", client.peer_identity},
            {"local_dataset_size", client.local_dataset_size}, {"hardware_info", client.hardware_info},
            {"dataset_fingerprint", client.dataset_fingerprint}, {"last_active_round", client.last_active_round},
            {"registered_at", client.registered_at}
        });
    }
    for (const auto& model : models_) {
        j["models"].push_back({
            {"tenant_id", model.tenant_id}, {"model_id", model.model_id}, {"version", model.version},
            {"latest_round", model.latest_round}, {"checkpoint_path", model.checkpoint_path},
            {"aggregation_strategy", model.aggregation_strategy}, {"key_version", model.key_version},
            {"client_count", model.client_count}, {"weight_dimension", model.weight_dimension},
            {"active", model.active}, {"created_at", model.created_at}
        });
    }
    for (const auto& dataset : datasets_) {
        j["datasets"].push_back({
            {"tenant_id", dataset.tenant_id}, {"model_id", dataset.model_id}, {"dataset_id", dataset.dataset_id},
            {"fingerprint", dataset.fingerprint}, {"storage_uri", dataset.storage_uri}, {"format", dataset.format},
            {"sample_count", dataset.sample_count}, {"shard_count", dataset.shard_count},
            {"active", dataset.active}, {"created_at", dataset.created_at}
        });
    }
    for (const auto& job : training_jobs_) {
        j["training_jobs"].push_back({
            {"job_id", job.job_id}, {"tenant_id", job.tenant_id}, {"model_id", job.model_id},
            {"dataset_id", job.dataset_id}, {"target_rounds", job.target_rounds},
            {"planned_clients", job.planned_clients}, {"min_clients_per_round", job.min_clients_per_round},
            {"assignment_ttl_seconds", job.assignment_ttl_seconds}, {"status", job.status},
            {"current_round", job.current_round}, {"completed_rounds", job.completed_rounds},
            {"active_model_version", job.active_model_version}, {"created_at", job.created_at},
            {"activated_at", job.activated_at}, {"notes", job.notes}
        });
    }
    for (const auto& assignment : assignments_) {
        j["assignments"].push_back({
            {"assignment_id", assignment.assignment_id}, {"job_id", assignment.job_id},
            {"tenant_id", assignment.tenant_id}, {"model_id", assignment.model_id},
            {"dataset_id", assignment.dataset_id}, {"client_id", assignment.client_id},
            {"round_index", assignment.round_index}, {"base_model_version", assignment.base_model_version},
            {"status", assignment.status}, {"issued_at", assignment.issued_at},
            {"expires_at", assignment.expires_at}, {"updated_at", assignment.updated_at},
            {"detail", assignment.detail}
        });
    }
    for (const auto& worker : workers_) {
        j["workers"].push_back({
            {"worker_id", worker.worker_id},
            {"trainer_client_id", worker.trainer_client_id},
            {"tenant_id", worker.tenant_id},
            {"model_id", worker.model_id},
            {"hostname", worker.hostname},
            {"launcher", worker.launcher},
            {"max_parallel_tasks", worker.max_parallel_tasks},
            {"labels", worker.labels},
            {"status", worker.status},
            {"registered_at", worker.registered_at},
            {"last_heartbeat_at", worker.last_heartbeat_at},
            {"active_task_id", worker.active_task_id}
        });
    }
    for (const auto& task : worker_tasks_) {
        j["worker_tasks"].push_back({
            {"task_id", task.task_id},
            {"worker_id", task.worker_id},
            {"trainer_client_id", task.trainer_client_id},
            {"job_id", task.job_id},
            {"assignment_id", task.assignment_id},
            {"tenant_id", task.tenant_id},
            {"model_id", task.model_id},
            {"dataset_id", task.dataset_id},
            {"round_index", task.round_index},
            {"base_model_version", task.base_model_version},
            {"status", task.status},
            {"detail", task.detail},
            {"progress_percent", task.progress_percent},
            {"issued_at", task.issued_at},
            {"started_at", task.started_at},
            {"completed_at", task.completed_at},
            {"updated_at", task.updated_at}
        });
    }
    for (const auto& event : audit_events_) {
        j["audit_events"].push_back({
            {"timestamp", event.timestamp}, {"action", event.action}, {"tenant_id", event.tenant_id},
            {"model_id", event.model_id}, {"client_id", event.client_id}, {"detail", event.detail}
        });
    }

    std::filesystem::path output_path(path_);
    if (!output_path.parent_path().empty()) {
        std::filesystem::create_directories(output_path.parent_path());
    }

    std::filesystem::path temp_path = output_path;
    temp_path += ".tmp";

    {
        std::ofstream file(temp_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            throw std::runtime_error("Failed to open temporary state store for writing: " +
                                     temp_path.string());
        }
        file << j.dump(2);
        file.flush();
        if (!file.good()) {
            throw std::runtime_error("Failed to flush temporary state store: " + temp_path.string());
        }
    }

    FsyncPathIfPossible(temp_path);
    if (std::rename(temp_path.string().c_str(), output_path.string().c_str()) != 0) {
        std::filesystem::remove(temp_path);
        throw std::runtime_error("Failed to atomically replace state store at " + output_path.string());
    }
    FsyncDirectoryIfPossible(output_path);
}

void StateStore::RecordClientRegistration(const PersistedClientRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto tenant_id = NormalizeTenant(record.tenant_id);
    auto model_id = NormalizeModel(record.model_id);
    auto it = std::find_if(clients_.begin(), clients_.end(), [&](const PersistedClientRecord& existing) {
        return existing.tenant_id == tenant_id && existing.model_id == model_id && existing.client_id == record.client_id;
    });
    if (it == clients_.end()) {
        PersistedClientRecord saved = record;
        saved.tenant_id = tenant_id;
        saved.model_id = model_id;
        saved.registered_at = saved.registered_at.empty() ? CurrentTimestamp() : saved.registered_at;
        clients_.push_back(saved);
    } else {
        if (it->peer_identity.empty() && !record.peer_identity.empty()) {
            it->peer_identity = record.peer_identity;
        }
        it->local_dataset_size = record.local_dataset_size;
        it->hardware_info = record.hardware_info;
        it->dataset_fingerprint = record.dataset_fingerprint;
    }
    SaveLocked();
}

void StateStore::UpdateClientLastRound(const std::string& tenant_id,
                                       const std::string& model_id,
                                       const std::string& client_id,
                                       int round) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    for (auto& client : clients_) {
        if (client.tenant_id == normalized_tenant && client.model_id == normalized_model && client.client_id == client_id) {
            client.last_active_round = std::max(client.last_active_round, round);
            break;
        }
    }
    SaveLocked();
}

void StateStore::RecordWeightSubmission(const std::string& tenant_id,
                                        const std::string& model_id,
                                        const std::string& client_id,
                                        int round) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++total_submissions_;
    audit_events_.push_back({
        CurrentTimestamp(), "weight_submission", NormalizeTenant(tenant_id), NormalizeModel(model_id), client_id,
        "Accepted submission for round " + std::to_string(round)
    });
    TrimAuditEventsLocked();
    SaveLocked();
}

int StateStore::RecordAggregatedModel(const std::string& tenant_id,
                                      const std::string& model_id,
                                      int round,
                                      const std::string& checkpoint_path,
                                      const std::string& aggregation_strategy,
                                      int key_version,
                                      int client_count,
                                      int weight_dimension) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    int next_version = 1;
    for (const auto& model : models_) {
        if (model.tenant_id == normalized_tenant && model.model_id == normalized_model) {
            next_version = std::max(next_version, model.version + 1);
        }
    }
    for (auto& model : models_) {
        if (model.tenant_id == normalized_tenant && model.model_id == normalized_model) {
            model.active = false;
        }
    }
    models_.push_back({
        normalized_tenant, normalized_model, next_version, round, checkpoint_path, aggregation_strategy, key_version,
        client_count, weight_dimension, true, CurrentTimestamp()
    });
    ++completed_rounds_;
    audit_events_.push_back({
        CurrentTimestamp(), "model_aggregated", normalized_tenant, normalized_model, "",
        "Stored model version " + std::to_string(next_version) + " for round " + std::to_string(round)
    });
    TrimAuditEventsLocked();
    SaveLocked();
    return next_version;
}

bool StateStore::RollbackModel(const std::string& tenant_id,
                               const std::string& model_id,
                               int target_version,
                               PersistedModelRecord* restored_model) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    PersistedModelRecord* target = nullptr;
    for (auto& model : models_) {
        if (model.tenant_id == normalized_tenant &&
            model.model_id == normalized_model &&
            model.version == target_version) {
            target = &model;
            break;
        }
    }
    if (!target) {
        return false;
    }
    for (auto& model : models_) {
        if (model.tenant_id == normalized_tenant && model.model_id == normalized_model) {
            model.active = false;
        }
    }
    target->active = true;
    if (restored_model != nullptr) {
        *restored_model = *target;
    }
    audit_events_.push_back({
        CurrentTimestamp(), "model_rollback", normalized_tenant, normalized_model, "",
        "Activated model version " + std::to_string(target_version)
    });
    TrimAuditEventsLocked();
    SaveLocked();
    return true;
}

void StateStore::UpdateActiveKeyVersion(int active_key_version) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_key_version_ == active_key_version) {
        return;
    }
    active_key_version_ = active_key_version;
    audit_events_.push_back({
        CurrentTimestamp(), "key_rotation", "default-tenant", "default-model", "",
        "Active at-rest key version is now " + std::to_string(active_key_version)
    });
    TrimAuditEventsLocked();
    SaveLocked();
}

void StateStore::RecordAuditEvent(const PersistedAuditEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);
    PersistedAuditEvent saved = event;
    if (saved.timestamp.empty()) {
        saved.timestamp = CurrentTimestamp();
    }
    saved.tenant_id = NormalizeTenant(saved.tenant_id);
    saved.model_id = NormalizeModel(saved.model_id);
    audit_events_.push_back(saved);
    TrimAuditEventsLocked();
    SaveLocked();
}

PersistedDatasetRecord StateStore::UpsertDataset(const PersistedDatasetRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    PersistedDatasetRecord saved = record;
    saved.tenant_id = NormalizeTenant(saved.tenant_id);
    saved.model_id = NormalizeModel(saved.model_id);
    saved.created_at = saved.created_at.empty() ? CurrentTimestamp() : saved.created_at;
    auto it = std::find_if(datasets_.begin(), datasets_.end(), [&](const PersistedDatasetRecord& existing) {
        return existing.tenant_id == saved.tenant_id &&
               existing.model_id == saved.model_id &&
               existing.dataset_id == saved.dataset_id;
    });
    if (it == datasets_.end()) {
        datasets_.push_back(saved);
    } else {
        saved.created_at = it->created_at;
        *it = saved;
    }
    audit_events_.push_back({
        CurrentTimestamp(), "dataset_upserted", saved.tenant_id, saved.model_id, "",
        "Dataset " + saved.dataset_id + " now points to " + saved.storage_uri
    });
    TrimAuditEventsLocked();
    SaveLocked();
    return saved;
}

std::vector<PersistedDatasetRecord> StateStore::ListDatasets(const std::string& tenant_filter,
                                                             const std::string& model_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedDatasetRecord> result;
    auto normalized_tenant = tenant_filter.empty() ? "" : NormalizeTenant(tenant_filter);
    auto normalized_model = model_filter.empty() ? "" : NormalizeModel(model_filter);
    for (const auto& dataset : datasets_) {
        if (MatchesFilter(dataset.tenant_id, normalized_tenant) &&
            MatchesFilter(dataset.model_id, normalized_model)) {
            result.push_back(dataset);
        }
    }
    return result;
}

bool StateStore::GetDataset(const std::string& tenant_id,
                            const std::string& model_id,
                            const std::string& dataset_id,
                            PersistedDatasetRecord* dataset) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    for (const auto& candidate : datasets_) {
        if (candidate.tenant_id == normalized_tenant &&
            candidate.model_id == normalized_model &&
            candidate.dataset_id == dataset_id) {
            if (dataset != nullptr) {
                *dataset = candidate;
            }
            return true;
        }
    }
    return false;
}

PersistedTrainingJobRecord StateStore::CreateTrainingJob(const PersistedTrainingJobRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    PersistedTrainingJobRecord saved = record;
    saved.tenant_id = NormalizeTenant(saved.tenant_id);
    saved.model_id = NormalizeModel(saved.model_id);
    saved.created_at = CurrentTimestamp();
    saved.status = saved.status.empty() ? "planned" : saved.status;
    saved.current_round = std::max(1, saved.current_round);
    saved.assignment_ttl_seconds = std::max(30, saved.assignment_ttl_seconds);
    saved.job_id = saved.job_id.empty()
        ? saved.tenant_id + "::" + saved.model_id + "::" + saved.dataset_id + "::" + saved.created_at
        : saved.job_id;
    saved.active_model_version = 0;
    for (const auto& model : models_) {
        if (model.tenant_id == saved.tenant_id && model.model_id == saved.model_id && model.active) {
            saved.active_model_version = model.version;
            break;
        }
    }
    for (auto& job : training_jobs_) {
        if (job.tenant_id == saved.tenant_id && job.model_id == saved.model_id && IsActiveJobStatus(job.status)) {
            job.status = "superseded";
        }
    }
    training_jobs_.push_back(saved);
    audit_events_.push_back({
        CurrentTimestamp(), "training_job_created", saved.tenant_id, saved.model_id, "",
        "Created job " + saved.job_id + " for dataset " + saved.dataset_id
    });
    TrimAuditEventsLocked();
    SaveLocked();
    return saved;
}

std::vector<PersistedTrainingJobRecord> StateStore::ListTrainingJobs(const std::string& tenant_filter,
                                                                     const std::string& model_filter,
                                                                     const std::string& status_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedTrainingJobRecord> result;
    auto normalized_tenant = tenant_filter.empty() ? "" : NormalizeTenant(tenant_filter);
    auto normalized_model = model_filter.empty() ? "" : NormalizeModel(model_filter);
    for (const auto& job : training_jobs_) {
        if (MatchesFilter(job.tenant_id, normalized_tenant) &&
            MatchesFilter(job.model_id, normalized_model) &&
            MatchesFilter(job.status, status_filter)) {
            result.push_back(job);
        }
    }
    return result;
}

bool StateStore::GetActiveTrainingJob(const std::string& tenant_id,
                                      const std::string& model_id,
                                      PersistedTrainingJobRecord* job) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    for (auto it = training_jobs_.rbegin(); it != training_jobs_.rend(); ++it) {
        if (it->tenant_id == normalized_tenant && it->model_id == normalized_model && IsActiveJobStatus(it->status)) {
            if (job != nullptr) {
                *job = *it;
            }
            return true;
        }
    }
    return false;
}

bool StateStore::LeaseTrainingAssignment(const std::string& tenant_id,
                                         const std::string& model_id,
                                         const std::string& client_id,
                                         int active_model_version,
                                         int default_min_clients,
                                         int default_assignment_ttl_seconds,
                                         PersistedAssignmentLease* assignment_out,
                                         PersistedTrainingJobRecord* job_out,
                                         PersistedDatasetRecord* dataset_out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    std::string now = CurrentTimestamp();
    ExpireAssignmentsLocked(now);

    PersistedTrainingJobRecord* job = nullptr;
    for (auto it = training_jobs_.rbegin(); it != training_jobs_.rend(); ++it) {
        if (it->tenant_id == normalized_tenant && it->model_id == normalized_model && IsActiveJobStatus(it->status)) {
            job = &(*it);
            break;
        }
    }
    if (!job) {
        return false;
    }
    if (job->target_rounds > 0 && job->completed_rounds >= job->target_rounds) {
        job->status = "completed";
        SaveLocked();
        return false;
    }
    if (job->status == "planned") {
        job->status = "running";
        job->activated_at = now;
    }

    int round_index = std::max(1, job->current_round);
    for (auto& assignment : assignments_) {
        if (assignment.job_id == job->job_id &&
            assignment.client_id == client_id &&
            assignment.round_index == round_index &&
            assignment.status == "leased" &&
            assignment.expires_at > now) {
            if (assignment_out != nullptr) *assignment_out = assignment;
            if (job_out != nullptr) *job_out = *job;
            if (dataset_out != nullptr) {
                for (const auto& dataset : datasets_) {
                    if (dataset.tenant_id == normalized_tenant &&
                        dataset.model_id == normalized_model &&
                        dataset.dataset_id == job->dataset_id) {
                        *dataset_out = dataset;
                        break;
                    }
                }
            }
            SaveLocked();
            return true;
        }
    }

    int round_capacity = job->planned_clients > 0
        ? job->planned_clients
        : std::max(1, job->min_clients_per_round > 0 ? job->min_clients_per_round : default_min_clients);
    int leased_count = 0;
    for (const auto& assignment : assignments_) {
        if (assignment.job_id == job->job_id &&
            assignment.round_index == round_index &&
            assignment.status == "leased" &&
            assignment.expires_at > now) {
            ++leased_count;
        }
    }
    if (leased_count >= round_capacity) {
        SaveLocked();
        return false;
    }

    PersistedAssignmentLease assignment;
    assignment.assignment_id = job->job_id + "::round:" + std::to_string(round_index) +
                               "::client:" + client_id + "::" + now;
    assignment.job_id = job->job_id;
    assignment.tenant_id = normalized_tenant;
    assignment.model_id = normalized_model;
    assignment.dataset_id = job->dataset_id;
    assignment.client_id = client_id;
    assignment.round_index = round_index;
    assignment.base_model_version = active_model_version;
    assignment.status = "leased";
    assignment.issued_at = now;
    assignment.expires_at = TimestampAfterSeconds(
        job->assignment_ttl_seconds > 0 ? job->assignment_ttl_seconds : default_assignment_ttl_seconds);
    assignment.updated_at = now;
    assignment.detail = "Lease issued";
    assignments_.push_back(assignment);

    audit_events_.push_back({
        now, "assignment_leased", normalized_tenant, normalized_model, client_id,
        "Issued assignment " + assignment.assignment_id + " for round " + std::to_string(round_index)
    });
    TrimAuditEventsLocked();
    SaveLocked();

    if (assignment_out != nullptr) *assignment_out = assignment;
    if (job_out != nullptr) *job_out = *job;
    if (dataset_out != nullptr) {
        for (const auto& dataset : datasets_) {
            if (dataset.tenant_id == normalized_tenant &&
                dataset.model_id == normalized_model &&
                dataset.dataset_id == job->dataset_id) {
                *dataset_out = dataset;
                break;
            }
        }
    }
    return true;
}

bool StateStore::ValidateTrainingAssignmentSubmission(const std::string& tenant_id,
                                                      const std::string& model_id,
                                                      const std::string& client_id,
                                                      const std::string& job_id,
                                                      const std::string& assignment_id,
                                                      int round,
                                                      int base_model_version,
                                                      std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    std::string now = CurrentTimestamp();
    ExpireAssignmentsLocked(now);

    PersistedTrainingJobRecord* active_job = nullptr;
    for (auto it = training_jobs_.rbegin(); it != training_jobs_.rend(); ++it) {
        if (it->tenant_id == normalized_tenant && it->model_id == normalized_model && IsActiveJobStatus(it->status)) {
            active_job = &(*it);
            break;
        }
    }
    if (!active_job) {
        return true;
    }
    if (job_id.empty() || assignment_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "assignment_id and job_id are required while a scheduled training job is active";
        }
        return false;
    }
    if (job_id != active_job->job_id) {
        if (error_message != nullptr) {
            *error_message = "submission does not match the active training job";
        }
        return false;
    }

    for (auto& assignment : assignments_) {
        if (assignment.assignment_id != assignment_id) continue;
        if (assignment.client_id != client_id ||
            assignment.tenant_id != normalized_tenant ||
            assignment.model_id != normalized_model) {
            if (error_message != nullptr) *error_message = "assignment belongs to a different scope or client";
            return false;
        }
        if (assignment.status != "leased") {
            if (error_message != nullptr) *error_message = "assignment is not in a leasable state";
            return false;
        }
        if (!assignment.expires_at.empty() && assignment.expires_at <= now) {
            assignment.status = "expired";
            assignment.updated_at = now;
            assignment.detail = "Expired at submission time";
            if (error_message != nullptr) *error_message = "assignment lease expired";
            SaveLocked();
            return false;
        }
        if (assignment.round_index != round) {
            if (error_message != nullptr) *error_message = "submission round does not match leased round";
            return false;
        }
        if (base_model_version > 0 &&
            assignment.base_model_version > 0 &&
            assignment.base_model_version != base_model_version) {
            if (error_message != nullptr) {
                *error_message = "submission base model version does not match assigned model version";
            }
            return false;
        }
        return true;
    }

    if (error_message != nullptr) *error_message = "assignment not found";
    return false;
}

bool StateStore::AcceptTrainingAssignmentSubmission(const std::string& tenant_id,
                                                    const std::string& model_id,
                                                    const std::string& client_id,
                                                    const std::string& job_id,
                                                    const std::string& assignment_id,
                                                    int round,
                                                    int base_model_version,
                                                    std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    std::string now = CurrentTimestamp();
    ExpireAssignmentsLocked(now);

    PersistedTrainingJobRecord* active_job = nullptr;
    for (auto it = training_jobs_.rbegin(); it != training_jobs_.rend(); ++it) {
        if (it->tenant_id == normalized_tenant && it->model_id == normalized_model && IsActiveJobStatus(it->status)) {
            active_job = &(*it);
            break;
        }
    }
    if (!active_job) {
        return true;
    }
    if (job_id.empty() || assignment_id.empty()) {
        if (error_message != nullptr) {
            *error_message = "assignment_id and job_id are required while a scheduled training job is active";
        }
        return false;
    }
    if (job_id != active_job->job_id) {
        if (error_message != nullptr) {
            *error_message = "submission does not match the active training job";
        }
        return false;
    }

    for (auto& assignment : assignments_) {
        if (assignment.assignment_id != assignment_id) continue;
        if (assignment.client_id != client_id ||
            assignment.tenant_id != normalized_tenant ||
            assignment.model_id != normalized_model) {
            if (error_message != nullptr) *error_message = "assignment belongs to a different scope or client";
            return false;
        }
        if (assignment.status != "leased") {
            if (error_message != nullptr) *error_message = "assignment is not in a leasable state";
            return false;
        }
        if (!assignment.expires_at.empty() && assignment.expires_at <= now) {
            assignment.status = "expired";
            assignment.updated_at = now;
            assignment.detail = "Expired at submission time";
            if (error_message != nullptr) *error_message = "assignment lease expired";
            SaveLocked();
            return false;
        }
        if (assignment.round_index != round) {
            if (error_message != nullptr) *error_message = "submission round does not match leased round";
            return false;
        }
        if (base_model_version > 0 &&
            assignment.base_model_version > 0 &&
            assignment.base_model_version != base_model_version) {
            if (error_message != nullptr) {
                *error_message = "submission base model version does not match assigned model version";
            }
            return false;
        }
        assignment.status = "submitted";
        assignment.updated_at = now;
        assignment.detail = "Client submitted weights";
        for (auto& task : worker_tasks_) {
            if (task.assignment_id == assignment.assignment_id) {
                task.status = "submitted";
                task.detail = "Weights submitted to control plane";
                task.progress_percent = 100;
                task.updated_at = now;
                if (task.completed_at.empty()) {
                    task.completed_at = now;
                }
                for (auto& worker : workers_) {
                    if (worker.worker_id == task.worker_id) {
                        worker.status = "idle";
                        if (worker.active_task_id == task.task_id) {
                            worker.active_task_id.clear();
                        }
                        worker.last_heartbeat_at = now;
                        break;
                    }
                }
            }
        }
        audit_events_.push_back({
            now, "assignment_submitted", normalized_tenant, normalized_model, client_id,
            "Accepted assignment " + assignment_id
        });
        TrimAuditEventsLocked();
        SaveLocked();
        return true;
    }

    if (error_message != nullptr) *error_message = "assignment not found";
    return false;
}

void StateStore::AdvanceTrainingJobRound(const std::string& tenant_id,
                                         const std::string& model_id,
                                         int round,
                                         int active_model_version) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    std::string now = CurrentTimestamp();

    for (auto it = training_jobs_.rbegin(); it != training_jobs_.rend(); ++it) {
        if (it->tenant_id != normalized_tenant || it->model_id != normalized_model || !IsActiveJobStatus(it->status)) {
            continue;
        }
        for (auto& assignment : assignments_) {
            if (assignment.job_id == it->job_id && assignment.round_index == round) {
                if (assignment.status == "submitted") assignment.status = "aggregated";
                else if (assignment.status == "leased") assignment.status = "missed";
                assignment.updated_at = now;
            }
        }
        for (auto& task : worker_tasks_) {
            if (task.job_id == it->job_id && task.round_index == round) {
                if (task.status == "submitted" || task.status == "completed") {
                    task.status = "aggregated";
                } else if (task.status == "leased" || task.status == "running") {
                    task.status = "missed";
                }
                task.updated_at = now;
                if (task.completed_at.empty() &&
                    (task.status == "aggregated" || task.status == "missed")) {
                    task.completed_at = now;
                }
                for (auto& worker : workers_) {
                    if (worker.worker_id == task.worker_id &&
                        worker.active_task_id == task.task_id) {
                        worker.active_task_id.clear();
                        worker.status = "idle";
                        worker.last_heartbeat_at = now;
                    }
                }
            }
        }
        it->status = "running";
        if (it->activated_at.empty()) it->activated_at = now;
        ++it->completed_rounds;
        it->current_round = round + 1;
        it->active_model_version = active_model_version;
        if (it->target_rounds > 0 && it->completed_rounds >= it->target_rounds) {
            it->status = "completed";
        }
        audit_events_.push_back({
            now, "training_round_advanced", normalized_tenant, normalized_model, "",
            "Job " + it->job_id + " advanced after round " + std::to_string(round)
        });
        TrimAuditEventsLocked();
        SaveLocked();
        return;
    }
}

PersistedWorkerRecord StateStore::UpsertWorker(const PersistedWorkerRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    PersistedWorkerRecord saved = record;
    saved.tenant_id = NormalizeTenant(saved.tenant_id);
    saved.model_id = NormalizeModel(saved.model_id);
    saved.max_parallel_tasks = std::max(1, saved.max_parallel_tasks);
    saved.status = saved.status.empty() ? "idle" : saved.status;
    saved.registered_at = saved.registered_at.empty() ? CurrentTimestamp() : saved.registered_at;
    saved.last_heartbeat_at = saved.last_heartbeat_at.empty() ? saved.registered_at : saved.last_heartbeat_at;

    auto it = std::find_if(workers_.begin(), workers_.end(), [&](const PersistedWorkerRecord& existing) {
        return existing.worker_id == saved.worker_id;
    });
    if (it == workers_.end()) {
        workers_.push_back(saved);
    } else {
        saved.registered_at = it->registered_at;
        if (saved.active_task_id.empty()) {
            saved.active_task_id = it->active_task_id;
        }
        *it = saved;
    }

    audit_events_.push_back({
        CurrentTimestamp(), "worker_upserted", saved.tenant_id, saved.model_id, saved.trainer_client_id,
        "Worker " + saved.worker_id + " registered launcher " + saved.launcher
    });
    TrimAuditEventsLocked();
    SaveLocked();
    return saved;
}

bool StateStore::GetWorker(const std::string& worker_id, PersistedWorkerRecord* worker) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& candidate : workers_) {
        if (candidate.worker_id == worker_id) {
            if (worker != nullptr) {
                *worker = candidate;
            }
            return true;
        }
    }
    return false;
}

std::vector<PersistedWorkerRecord> StateStore::ListWorkers(const std::string& tenant_filter,
                                                           const std::string& model_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedWorkerRecord> result;
    auto normalized_tenant = tenant_filter.empty() ? "" : NormalizeTenant(tenant_filter);
    auto normalized_model = model_filter.empty() ? "" : NormalizeModel(model_filter);
    for (const auto& worker : workers_) {
        if (MatchesFilter(worker.tenant_id, normalized_tenant) &&
            MatchesFilter(worker.model_id, normalized_model)) {
            result.push_back(worker);
        }
    }
    return result;
}

PersistedWorkerTaskRecord StateStore::LeaseWorkerTask(const PersistedWorkerRecord& worker,
                                                      const PersistedAssignmentLease& assignment,
                                                      const PersistedTrainingJobRecord& job,
                                                      const PersistedDatasetRecord& dataset) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string now = CurrentTimestamp();

    for (auto& task : worker_tasks_) {
        if (task.assignment_id == assignment.assignment_id && task.worker_id == worker.worker_id) {
            task.updated_at = now;
            for (auto& saved_worker : workers_) {
                if (saved_worker.worker_id == worker.worker_id) {
                    saved_worker.status = "leased";
                    saved_worker.last_heartbeat_at = now;
                    saved_worker.active_task_id = task.task_id;
                }
            }
            SaveLocked();
            return task;
        }
    }

    PersistedWorkerTaskRecord task;
    task.task_id = assignment.assignment_id + "::worker:" + worker.worker_id;
    task.worker_id = worker.worker_id;
    task.trainer_client_id = worker.trainer_client_id;
    task.job_id = job.job_id;
    task.assignment_id = assignment.assignment_id;
    task.tenant_id = assignment.tenant_id;
    task.model_id = assignment.model_id;
    task.dataset_id = dataset.dataset_id;
    task.round_index = assignment.round_index;
    task.base_model_version = assignment.base_model_version;
    task.status = "leased";
    task.detail = "Worker task leased";
    task.progress_percent = 0;
    task.issued_at = now;
    task.updated_at = now;
    worker_tasks_.push_back(task);

    for (auto& saved_worker : workers_) {
        if (saved_worker.worker_id == worker.worker_id) {
            saved_worker.status = "leased";
            saved_worker.last_heartbeat_at = now;
            saved_worker.active_task_id = task.task_id;
            break;
        }
    }

    audit_events_.push_back({
        now, "worker_task_leased", assignment.tenant_id, assignment.model_id, worker.trainer_client_id,
        "Leased worker task " + task.task_id + " to " + worker.worker_id
    });
    TrimAuditEventsLocked();
    SaveLocked();
    return task;
}

bool StateStore::UpdateWorkerTaskStatus(const std::string& worker_id,
                                        const std::string& task_id,
                                        const std::string& status,
                                        const std::string& detail,
                                        int progress_percent,
                                        std::string* error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string now = CurrentTimestamp();

    for (auto& task : worker_tasks_) {
        if (task.task_id != task_id) {
            continue;
        }
        if (task.worker_id != worker_id) {
            if (error_message != nullptr) {
                *error_message = "task belongs to a different worker";
            }
            return false;
        }

        task.status = status.empty() ? task.status : status;
        task.detail = detail;
        task.progress_percent = std::max(0, std::min(100, progress_percent));
        task.updated_at = now;
        if (task.status == "running" && task.started_at.empty()) {
            task.started_at = now;
        }
        if ((task.status == "completed" || task.status == "failed" ||
             task.status == "submitted" || task.status == "aggregated") &&
            task.completed_at.empty()) {
            task.completed_at = now;
        }

        for (auto& worker : workers_) {
            if (worker.worker_id != worker_id) {
                continue;
            }
            worker.last_heartbeat_at = now;
            if (task.status == "running") {
                worker.status = "busy";
                worker.active_task_id = task.task_id;
            } else if (task.status == "leased") {
                worker.status = "leased";
                worker.active_task_id = task.task_id;
            } else if (task.status == "completed" || task.status == "failed" ||
                       task.status == "submitted" || task.status == "aggregated") {
                worker.status = "idle";
                if (worker.active_task_id == task.task_id) {
                    worker.active_task_id.clear();
                }
            }
            break;
        }

        audit_events_.push_back({
            now, "worker_task_status", task.tenant_id, task.model_id, task.trainer_client_id,
            "Worker " + worker_id + " set task " + task_id + " to " + task.status
        });
        TrimAuditEventsLocked();
        SaveLocked();
        return true;
    }

    if (error_message != nullptr) {
        *error_message = "worker task not found";
    }
    return false;
}

bool StateStore::RecordWorkerHeartbeat(const std::string& worker_id,
                                       const std::string& active_task_id,
                                       const std::string& detail) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string now = CurrentTimestamp();
    for (auto& worker : workers_) {
        if (worker.worker_id != worker_id) {
            continue;
        }
        worker.last_heartbeat_at = now;
        if (!active_task_id.empty()) {
            worker.active_task_id = active_task_id;
            worker.status = "busy";
        } else if (worker.status != "leased") {
            worker.status = "idle";
        }
        if (!detail.empty()) {
            audit_events_.push_back({
                now, "worker_heartbeat", worker.tenant_id, worker.model_id, worker.trainer_client_id,
                "Worker " + worker_id + ": " + detail
            });
            TrimAuditEventsLocked();
        }
        SaveLocked();
        return true;
    }
    return false;
}

std::vector<PersistedWorkerTaskRecord> StateStore::ListWorkerTasks(const std::string& tenant_filter,
                                                                   const std::string& model_filter,
                                                                   const std::string& worker_filter,
                                                                   const std::string& status_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedWorkerTaskRecord> result;
    auto normalized_tenant = tenant_filter.empty() ? "" : NormalizeTenant(tenant_filter);
    auto normalized_model = model_filter.empty() ? "" : NormalizeModel(model_filter);
    for (const auto& task : worker_tasks_) {
        if (MatchesFilter(task.tenant_id, normalized_tenant) &&
            MatchesFilter(task.model_id, normalized_model) &&
            MatchesFilter(task.worker_id, worker_filter) &&
            MatchesFilter(task.status, status_filter)) {
            result.push_back(task);
        }
    }
    return result;
}

bool StateStore::GetActiveModelRecord(const std::string& tenant_id,
                                      const std::string& model_id,
                                      PersistedModelRecord* model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    for (const auto& candidate : models_) {
        if (candidate.tenant_id == normalized_tenant &&
            candidate.model_id == normalized_model &&
            candidate.active) {
            if (model != nullptr) {
                *model = candidate;
            }
            return true;
        }
    }
    return false;
}

PersistedSystemStatus StateStore::GetSystemStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PersistedSystemStatus status;
    status.registered_clients = static_cast<int>(clients_.size());
    status.completed_rounds = completed_rounds_;
    status.total_submissions = total_submissions_;
    status.active_key_version = active_key_version_;
    status.state_path = path_;
    status.registered_datasets = static_cast<int>(datasets_.size());
    status.stored_models = static_cast<int>(models_.size());
    status.active_training_jobs = 0;
    status.registered_workers = static_cast<int>(workers_.size());
    status.busy_workers = 0;
    status.active_worker_tasks = 0;
    status.pending_assignments = 0;
    status.leased_assignments = 0;
    status.submitted_assignments = 0;
    status.failed_worker_tasks = 0;
    for (const auto& model : models_) {
        if (model.active) status.active_models.push_back(model);
    }
    for (const auto& job : training_jobs_) {
        if (IsActiveJobStatus(job.status)) {
            ++status.active_training_jobs;
            int round_capacity = job.planned_clients > 0 ? job.planned_clients : job.min_clients_per_round;
            if (round_capacity > 0) {
                int active_assignments = 0;
                for (const auto& assignment : assignments_) {
                    if (assignment.job_id == job.job_id &&
                        assignment.round_index == job.current_round &&
                        (assignment.status == "leased" ||
                         assignment.status == "submitted" ||
                         assignment.status == "aggregated")) {
                        ++active_assignments;
                    }
                }
                status.pending_assignments += std::max(0, round_capacity - active_assignments);
            }
        }
    }
    for (const auto& assignment : assignments_) {
        if (assignment.status == "leased") ++status.leased_assignments;
        else if (assignment.status == "submitted") ++status.submitted_assignments;
    }
    for (const auto& worker : workers_) {
        if (worker.status == "busy" || worker.status == "leased") {
            ++status.busy_workers;
        }
    }
    for (const auto& task : worker_tasks_) {
        if (task.status == "leased" || task.status == "running" || task.status == "submitted") {
            ++status.active_worker_tasks;
        }
        if (task.status == "failed") {
            ++status.failed_worker_tasks;
        }
    }
    return status;
}

std::vector<PersistedModelRecord> StateStore::ListModels(const std::string& tenant_filter,
                                                         const std::string& model_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedModelRecord> result;
    auto normalized_tenant = tenant_filter.empty() ? "" : NormalizeTenant(tenant_filter);
    auto normalized_model = model_filter.empty() ? "" : NormalizeModel(model_filter);
    for (const auto& model : models_) {
        if (MatchesFilter(model.tenant_id, normalized_tenant) &&
            MatchesFilter(model.model_id, normalized_model)) {
            result.push_back(model);
        }
    }
    return result;
}

std::vector<PersistedClientRecord> StateStore::ListClients(const std::string& tenant_filter,
                                                           const std::string& model_filter) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedClientRecord> result;
    auto normalized_tenant = tenant_filter.empty() ? "" : NormalizeTenant(tenant_filter);
    auto normalized_model = model_filter.empty() ? "" : NormalizeModel(model_filter);
    for (const auto& client : clients_) {
        if (MatchesFilter(client.tenant_id, normalized_tenant) &&
            MatchesFilter(client.model_id, normalized_model)) {
            result.push_back(client);
        }
    }
    return result;
}

std::vector<PersistedAuditEvent> StateStore::ListAuditEvents(size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedAuditEvent> result;
    size_t count = std::min(limit, audit_events_.size());
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.push_back(audit_events_[audit_events_.size() - 1 - i]);
    }
    return result;
}

std::vector<PersistedModelRecord> StateStore::GetActiveModels() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PersistedModelRecord> result;
    for (const auto& model : models_) {
        if (model.active) result.push_back(model);
    }
    return result;
}

int StateStore::GetActiveModelVersion(const std::string& tenant_id, const std::string& model_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto normalized_tenant = NormalizeTenant(tenant_id);
    auto normalized_model = NormalizeModel(model_id);
    for (const auto& model : models_) {
        if (model.tenant_id == normalized_tenant && model.model_id == normalized_model && model.active) {
            return model.version;
        }
    }
    return 0;
}
