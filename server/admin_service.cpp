#include "admin_service.hpp"
#include "config.hpp"
#include "identity_utils.hpp"
#include "logger.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>

namespace {

void PopulateModelHead(const PersistedModelRecord& model, pqfl::ModelHead* out) {
    out->set_tenant_id(model.tenant_id);
    out->set_model_id(model.model_id);
    out->set_active_version(model.version);
    out->set_latest_round(model.latest_round);
    out->set_checkpoint_path(model.checkpoint_path);
    out->set_aggregation_strategy(model.aggregation_strategy);
    out->set_key_version(model.key_version);
}

void PopulateDatasetRecord(const PersistedDatasetRecord& dataset, pqfl::DatasetRecord* out) {
    out->set_tenant_id(dataset.tenant_id);
    out->set_model_id(dataset.model_id);
    out->set_dataset_id(dataset.dataset_id);
    out->set_fingerprint(dataset.fingerprint);
    out->set_storage_uri(dataset.storage_uri);
    out->set_format(dataset.format);
    out->set_sample_count(dataset.sample_count);
    out->set_shard_count(dataset.shard_count);
    out->set_active(dataset.active);
    out->set_created_at(dataset.created_at);
}

void PopulateTrainingJob(const PersistedTrainingJobRecord& job, pqfl::TrainingJob* out) {
    out->set_job_id(job.job_id);
    out->set_tenant_id(job.tenant_id);
    out->set_model_id(job.model_id);
    out->set_dataset_id(job.dataset_id);
    out->set_target_rounds(job.target_rounds);
    out->set_planned_clients(job.planned_clients);
    out->set_min_clients_per_round(job.min_clients_per_round);
    out->set_assignment_ttl_seconds(job.assignment_ttl_seconds);
    out->set_status(job.status);
    out->set_current_round(job.current_round);
    out->set_completed_rounds(job.completed_rounds);
    out->set_active_model_version(job.active_model_version);
    out->set_created_at(job.created_at);
    out->set_activated_at(job.activated_at);
    out->set_notes(job.notes);
}

void PopulateWorkerInfo(const PersistedWorkerRecord& worker, pqfl::RegisteredWorkerInfo* out) {
    out->set_worker_id(worker.worker_id);
    out->set_trainer_client_id(worker.trainer_client_id);
    out->set_tenant_id(worker.tenant_id);
    out->set_model_id(worker.model_id);
    out->set_hostname(worker.hostname);
    out->set_launcher(worker.launcher);
    out->set_max_parallel_tasks(worker.max_parallel_tasks);
    for (const auto& label : worker.labels) {
        out->add_labels(label);
    }
    out->set_status(worker.status);
    out->set_registered_at(worker.registered_at);
    out->set_last_heartbeat_at(worker.last_heartbeat_at);
    out->set_active_task_id(worker.active_task_id);
}

void PopulateWorkerTask(const PersistedWorkerTaskRecord& task, pqfl::WorkerTaskInfo* out) {
    out->set_task_id(task.task_id);
    out->set_worker_id(task.worker_id);
    out->set_trainer_client_id(task.trainer_client_id);
    out->set_job_id(task.job_id);
    out->set_assignment_id(task.assignment_id);
    out->set_tenant_id(task.tenant_id);
    out->set_model_id(task.model_id);
    out->set_dataset_id(task.dataset_id);
    out->set_round_index(task.round_index);
    out->set_base_model_version(task.base_model_version);
    out->set_status(task.status);
    out->set_detail(task.detail);
    out->set_progress_percent(task.progress_percent);
    out->set_issued_at(task.issued_at);
    out->set_started_at(task.started_at);
    out->set_completed_at(task.completed_at);
    out->set_updated_at(task.updated_at);
}

std::string ValidateAdminScope(const std::string& tenant_id, const std::string& model_id) {
    std::string error = ValidateIdentifier(tenant_id, "tenant_id", true);
    if (!error.empty()) return error;
    return ValidateIdentifier(model_id, "model_id", true);
}

bool IsAuthorizedAdmin(grpc::ServerContext* context) {
    return HasAllowedPeerIdentity(context, Config::Instance().Get().admin_peer_identities);
}

} // namespace

AdminServiceImpl::AdminServiceImpl(StateStore& state_store,
                                   CryptoEngine& crypto,
                                   WeightBuffer& buffer)
    : state_store_(state_store), crypto_(crypto), buffer_(buffer) {}

grpc::Status AdminServiceImpl::GetSystemStatus(grpc::ServerContext* context,
                                               const pqfl::AdminEmpty* request,
                                               pqfl::SystemStatus* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    auto status = state_store_.GetSystemStatus();
    response->set_registered_clients(status.registered_clients);
    response->set_completed_rounds(status.completed_rounds);
    response->set_total_submissions(status.total_submissions);
    response->set_active_key_version(status.active_key_version);
    response->set_state_path(status.state_path);
    response->set_registered_datasets(status.registered_datasets);
    response->set_active_training_jobs(status.active_training_jobs);
    response->set_registered_workers(status.registered_workers);
    response->set_active_worker_tasks(status.active_worker_tasks);
    response->set_stored_models(status.stored_models);
    response->set_pending_assignments(status.pending_assignments);
    response->set_leased_assignments(status.leased_assignments);
    response->set_submitted_assignments(status.submitted_assignments);
    response->set_busy_workers(status.busy_workers);
    response->set_failed_worker_tasks(status.failed_worker_tasks);
    for (const auto& model : status.active_models) {
        PopulateModelHead(model, response->add_active_models());
    }
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::ListModels(grpc::ServerContext* context,
                                          const pqfl::ListModelsRequest* request,
                                          pqfl::ModelRegistrySnapshot* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    auto models = state_store_.ListModels(request->tenant_id(), request->model_id());
    for (const auto& model : models) {
        PopulateModelHead(model, response->add_models());
    }
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::ListClients(grpc::ServerContext* context,
                                           const pqfl::ListClientsRequest* request,
                                           pqfl::RegisteredClients* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    auto clients = state_store_.ListClients(request->tenant_id(), request->model_id());
    for (const auto& client : clients) {
        auto* out = response->add_clients();
        out->set_tenant_id(client.tenant_id);
        out->set_model_id(client.model_id);
        out->set_client_id(client.client_id);
        out->set_local_dataset_size(client.local_dataset_size);
        out->set_hardware_info(client.hardware_info);
        out->set_last_active_round(client.last_active_round);
        out->set_registered_at(client.registered_at);
        out->set_dataset_fingerprint(client.dataset_fingerprint);
    }
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::ListWorkers(grpc::ServerContext* context,
                                           const pqfl::ListWorkersRequest* request,
                                           pqfl::RegisteredWorkers* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    auto workers = state_store_.ListWorkers(request->tenant_id(), request->model_id());
    for (const auto& worker : workers) {
        PopulateWorkerInfo(worker, response->add_workers());
    }
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::ListWorkerTasks(grpc::ServerContext* context,
                                               const pqfl::ListWorkerTasksRequest* request,
                                               pqfl::WorkerTasksResponse* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    auto tasks = state_store_.ListWorkerTasks(
        request->tenant_id(), request->model_id(), request->worker_id(), request->status());
    for (const auto& task : tasks) {
        PopulateWorkerTask(task, response->add_tasks());
    }
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::UpsertDataset(grpc::ServerContext* context,
                                             const pqfl::UpsertDatasetRequest* request,
                                             pqfl::DatasetRecord* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    std::string validation_error = ValidateAdminScope(request->tenant_id(), request->model_id());
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    validation_error = ValidateIdentifier(request->dataset_id(), "dataset_id");
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    if (request->storage_uri().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "storage_uri is required");
    }
    if (request->format().empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "format is required");
    }
    if (request->sample_count() < 0 || request->shard_count() < 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "sample_count and shard_count must be >= 0");
    }
    PersistedDatasetRecord saved = state_store_.UpsertDataset({
        request->tenant_id(),
        request->model_id(),
        request->dataset_id(),
        request->fingerprint(),
        request->storage_uri(),
        request->format(),
        request->sample_count(),
        request->shard_count(),
        request->active(),
        ""
    });
    PopulateDatasetRecord(saved, response);
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::ListDatasets(grpc::ServerContext* context,
                                            const pqfl::ListDatasetsRequest* request,
                                            pqfl::DatasetCatalog* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    auto datasets = state_store_.ListDatasets(request->tenant_id(), request->model_id());
    for (const auto& dataset : datasets) {
        PopulateDatasetRecord(dataset, response->add_datasets());
    }
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::CreateTrainingJob(grpc::ServerContext* context,
                                                 const pqfl::CreateTrainingJobRequest* request,
                                                 pqfl::TrainingJob* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    std::string validation_error = ValidateAdminScope(request->tenant_id(), request->model_id());
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    validation_error = ValidateIdentifier(request->dataset_id(), "dataset_id");
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    if (request->target_rounds() <= 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "target_rounds must be >= 1");
    }
    if (request->planned_clients() <= 0 || request->min_clients_per_round() <= 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "planned_clients and min_clients_per_round must be >= 1");
    }
    if (request->planned_clients() < request->min_clients_per_round()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "planned_clients must be >= min_clients_per_round");
    }
    if (request->assignment_ttl_seconds() < 30) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "assignment_ttl_seconds must be >= 30");
    }

    PersistedDatasetRecord dataset;
    if (!state_store_.GetDataset(request->tenant_id(), request->model_id(), request->dataset_id(), &dataset)) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Dataset must be registered before creating a training job");
    }

    PersistedTrainingJobRecord job = state_store_.CreateTrainingJob({
        "",
        request->tenant_id(),
        request->model_id(),
        request->dataset_id(),
        std::max(1, request->target_rounds()),
        std::max(1, request->planned_clients()),
        std::max(1, request->min_clients_per_round()),
        std::max(30, request->assignment_ttl_seconds()),
        "planned",
        1,
        0,
        0,
        "",
        "",
        request->notes()
    });
    PopulateTrainingJob(job, response);
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::ListTrainingJobs(grpc::ServerContext* context,
                                                const pqfl::ListTrainingJobsRequest* request,
                                                pqfl::TrainingJobsResponse* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    auto jobs = state_store_.ListTrainingJobs(request->tenant_id(), request->model_id(), request->status());
    for (const auto& job : jobs) {
        PopulateTrainingJob(job, response->add_jobs());
    }
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::RollbackModel(grpc::ServerContext* context,
                                             const pqfl::RollbackModelRequest* request,
                                             pqfl::RollbackModelResponse* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    std::string validation_error = ValidateAdminScope(request->tenant_id(), request->model_id());
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    if (request->target_version() <= 0) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "target_version must be >= 1");
    }
    PersistedModelRecord restored_model;
    bool ok = state_store_.RollbackModel(
        request->tenant_id(),
        request->model_id(),
        request->target_version(),
        &restored_model
    );

    if (!ok) {
        response->set_success(false);
        response->set_message("Requested model version not found");
        Logger::Warn("Rollback request failed because target version was not found", "Admin");
        return grpc::Status::OK;
    }

    std::ifstream file(restored_model.checkpoint_path, std::ios::binary);
    if (!file.is_open()) {
        response->set_success(false);
        response->set_message("Checkpoint file not found for requested rollback");
        return grpc::Status::OK;
    }

    std::string blob((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string plaintext;
    try {
        plaintext = crypto_.DecryptBlob(blob);
    } catch (const std::exception& e) {
        response->set_success(false);
        response->set_message("Failed to decrypt rollback checkpoint: " + std::string(e.what()));
        return grpc::Status::OK;
    }

    if (plaintext.size() % sizeof(float) != 0) {
        response->set_success(false);
        response->set_message("Rollback checkpoint had invalid tensor data");
        return grpc::Status::OK;
    }

    size_t float_count = plaintext.size() / sizeof(float);
    std::vector<float> weights(float_count);
    std::memcpy(weights.data(), plaintext.data(), plaintext.size());
    buffer_.RestoreLatestGlobalModel(
        restored_model.tenant_id,
        restored_model.model_id,
        restored_model.latest_round,
        restored_model.version,
        weights
    );

    response->set_success(true);
    response->set_message("Rollback activated model version " + std::to_string(restored_model.version));
    PopulateModelHead(restored_model, response->mutable_active_model());
    Logger::Warn("Admin rollback activated " + restored_model.tenant_id + "/" + restored_model.model_id +
                 " version " + std::to_string(restored_model.version), "Admin");
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::RotateAtRestKey(grpc::ServerContext* context,
                                               const pqfl::RotateKeyRequest* request,
                                               pqfl::RotateKeyResponse* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    int key_version = crypto_.RotateMasterKey();
    state_store_.UpdateActiveKeyVersion(key_version);
    response->set_active_key_version(key_version);
    response->set_message("Rotated at-rest encryption key; existing checkpoints remain readable through versioned keys");
    Logger::Warn("Admin rotated at-rest encryption key to version " + std::to_string(key_version), "Admin");
    return grpc::Status::OK;
}

grpc::Status AdminServiceImpl::ListAuditEvents(grpc::ServerContext* context,
                                               const pqfl::AuditEventsRequest* request,
                                               pqfl::AuditEventsResponse* response) {
    if (!IsAuthorizedAdmin(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Admin certificate required");
    }
    int limit = request->limit() > 0 ? request->limit() : 20;
    auto events = state_store_.ListAuditEvents(static_cast<size_t>(limit));
    for (const auto& event : events) {
        auto* out = response->add_events();
        out->set_timestamp(event.timestamp);
        out->set_action(event.action);
        out->set_tenant_id(event.tenant_id);
        out->set_model_id(event.model_id);
        out->set_client_id(event.client_id);
        out->set_detail(event.detail);
    }
    return grpc::Status::OK;
}
