#include "worker_service.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "identity_utils.hpp"
#include "logger.hpp"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace {



std::string NormalizeTenant(const std::string& tenant_id) {
    return tenant_id.empty() ? "default-tenant" : tenant_id;
}

std::string NormalizeModel(const std::string& model_id) {
    return model_id.empty() ? "default-model" : model_id;
}

bool IsAuthorizedWorker(grpc::ServerContext* context) {
    return HasAllowedPeerIdentity(context, Config::Instance().Get().worker_peer_identities);
}

bool IsAllowedTaskStatus(const std::string& status) {
    static const std::unordered_set<std::string> kAllowedStatuses = {
        "leased", "running", "completed", "failed", "submitted", "aggregated"
    };
    return !status.empty() && kAllowedStatuses.count(status) > 0;
}

} // namespace

WorkerServiceImpl::WorkerServiceImpl(StateStore& state_store,
                                     ClientRegistry& registry,
                                     CryptoEngine& crypto)
    : state_store_(state_store),
      registry_(registry),
      crypto_(crypto) {}

grpc::Status WorkerServiceImpl::RegisterWorker(grpc::ServerContext* context,
                                               const pqfl::WorkerRegistration* request,
                                               pqfl::WorkerRegistrationResponse* response) {
    if (!IsAuthorizedWorker(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Worker certificate required");
    }

    std::string tenant_id = NormalizeTenant(request->tenant_id());
    std::string model_id = NormalizeModel(request->model_id());
    std::string worker_id = request->worker_id().empty()
        ? request->trainer_client_id() + "-worker"
        : request->worker_id();
    std::string trainer_client_id = request->trainer_client_id().empty()
        ? worker_id + "-trainer"
        : request->trainer_client_id();
    std::string validation_error = ValidateIdentifier(request->tenant_id(), "tenant_id", true);
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    validation_error = ValidateIdentifier(request->model_id(), "model_id", true);
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    validation_error = ValidateIdentifier(worker_id, "worker_id");
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    validation_error = ValidateIdentifier(trainer_client_id, "trainer_client_id");
    if (!validation_error.empty()) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, validation_error);
    }
    std::vector<std::string> labels(request->labels().begin(), request->labels().end());

    PersistedWorkerRecord worker = state_store_.UpsertWorker({
        worker_id,
        trainer_client_id,
        tenant_id,
        model_id,
        request->hostname(),
        request->launcher(),
        std::max(1, request->max_parallel_tasks()),
        labels,
        "idle",
        "",
        CurrentTimestamp(),
        ""
    });

    std::string peer_identity = NormalizePeerIdentity(GetPeerIdentity(context));
    if (!registry_.IsRegistered(tenant_id, model_id, trainer_client_id)) {
        ClientInfo info;
        info.tenant_id = tenant_id;
        info.model_id = model_id;
        info.client_id = trainer_client_id;
        info.peer_identity = peer_identity;
        info.local_dataset_size = std::max(1, request->advertised_dataset_size());
        info.hardware_info = request->hostname() + " via " + request->launcher();
        info.dataset_fingerprint = "worker-sidecar";
        info.registered_at = CurrentTimestamp();
        registry_.Register(info);
        state_store_.RecordClientRegistration({
            tenant_id,
            model_id,
            trainer_client_id,
            info.peer_identity,
            info.local_dataset_size,
            info.hardware_info,
            info.dataset_fingerprint,
            -1,
            info.registered_at
        });
    } else if (registry_.BindPeerIdentityIfMissing(tenant_id, model_id, trainer_client_id, peer_identity)) {
        state_store_.RecordClientRegistration({
            tenant_id,
            model_id,
            trainer_client_id,
            peer_identity,
            std::max(1, request->advertised_dataset_size()),
            request->hostname() + " via " + request->launcher(),
            "worker-sidecar",
            -1,
            CurrentTimestamp()
        });
    }

    response->set_accepted(true);
    response->set_worker_id(worker.worker_id);
    response->set_trainer_client_id(worker.trainer_client_id);
    response->set_server_time(CurrentTimestamp());
    response->set_message("Worker registered and trainer client is ready");
    return grpc::Status::OK;
}

grpc::Status WorkerServiceImpl::LeaseTrainingTask(grpc::ServerContext* context,
                                                  const pqfl::WorkerTaskLeaseRequest* request,
                                                  pqfl::WorkerTaskLeaseResponse* response) {
    if (!IsAuthorizedWorker(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Worker certificate required");
    }

    PersistedWorkerRecord worker;
    std::string validation_error = ValidateIdentifier(request->worker_id(), "worker_id");
    if (!validation_error.empty()) {
        response->set_task_ready(false);
        response->set_message(validation_error);
        return grpc::Status::OK;
    }
    if (!state_store_.GetWorker(request->worker_id(), &worker)) {
        response->set_task_ready(false);
        response->set_message("Worker must register before leasing tasks");
        return grpc::Status::OK;
    }

    std::string tenant_id = NormalizeTenant(request->tenant_id().empty() ? worker.tenant_id : request->tenant_id());
    std::string model_id = NormalizeModel(request->model_id().empty() ? worker.model_id : request->model_id());
    int active_model_version = state_store_.GetActiveModelVersion(tenant_id, model_id);

    PersistedAssignmentLease assignment;
    PersistedTrainingJobRecord job;
    PersistedDatasetRecord dataset;
    if (!state_store_.LeaseTrainingAssignment(
            tenant_id,
            model_id,
            worker.trainer_client_id,
            active_model_version,
            Config::Instance().Get().required_clients,
            Config::Instance().Get().default_assignment_ttl_seconds,
            &assignment,
            &job,
            &dataset)) {
        response->set_task_ready(false);
        response->set_message("No runnable training assignment is available right now");
        return grpc::Status::OK;
    }

    PersistedWorkerTaskRecord task = state_store_.LeaseWorkerTask(worker, assignment, job, dataset);
    PersistedModelRecord active_model;
    bool has_model = state_store_.GetActiveModelRecord(tenant_id, model_id, &active_model);
    const auto& cfg = Config::Instance().Get();

    response->set_task_ready(true);
    response->set_message("Training task leased");
    response->set_task_id(task.task_id);
    response->set_worker_id(worker.worker_id);
    response->set_trainer_client_id(worker.trainer_client_id);
    response->set_job_id(job.job_id);
    response->set_assignment_id(assignment.assignment_id);
    response->set_dataset_id(dataset.dataset_id);
    response->set_dataset_storage_uri(dataset.storage_uri);
    response->set_dataset_fingerprint(dataset.fingerprint);
    response->set_dataset_format(dataset.format);
    response->set_round_index(assignment.round_index);
    response->set_base_model_version(assignment.base_model_version);
    response->set_active_model_version(has_model ? active_model.version : active_model_version);
    response->set_checkpoint_path(has_model ? active_model.checkpoint_path : "");
    response->set_target_rounds(job.target_rounds);
    response->set_learning_rate(cfg.learning_rate);
    response->set_batch_size(cfg.batch_size);
    response->set_required_clients(job.min_clients_per_round > 0 ? job.min_clients_per_round : cfg.required_clients);
    response->set_aggregation_strategy(cfg.aggregation_strategy);
    response->set_clip_norm(cfg.clip_norm);
    response->set_server_momentum(cfg.server_momentum);
    response->set_assignment_expires_at(assignment.expires_at);
    std::vector<unsigned char> kem_public_key = crypto_.GetKemPublicKey();
    response->set_payload_kem_public_key(std::string(kem_public_key.begin(), kem_public_key.end()));
    response->set_payload_kem_algorithm(crypto_.GetKemAlgorithm());
    return grpc::Status::OK;
}

grpc::Status WorkerServiceImpl::ReportTaskStatus(grpc::ServerContext* context,
                                                 const pqfl::WorkerTaskStatusUpdate* request,
                                                 pqfl::WorkerTaskStatusAck* response) {
    if (!IsAuthorizedWorker(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Worker certificate required");
    }
    std::string validation_error = ValidateIdentifier(request->worker_id(), "worker_id");
    if (!validation_error.empty()) {
        response->set_accepted(false);
        response->set_message(validation_error);
        return grpc::Status::OK;
    }
    validation_error = ValidateIdentifier(request->task_id(), "task_id");
    if (!validation_error.empty()) {
        response->set_accepted(false);
        response->set_message(validation_error);
        return grpc::Status::OK;
    }
    if (!IsAllowedTaskStatus(request->status())) {
        response->set_accepted(false);
        response->set_message("status must be one of leased, running, completed, failed, submitted, aggregated");
        return grpc::Status::OK;
    }

    std::string error_message;
    bool accepted = state_store_.UpdateWorkerTaskStatus(
        request->worker_id(),
        request->task_id(),
        request->status(),
        request->detail(),
        request->progress_percent(),
        &error_message
    );
    response->set_accepted(accepted);
    response->set_message(accepted ? "Worker task status recorded" : error_message);
    return grpc::Status::OK;
}

grpc::Status WorkerServiceImpl::Heartbeat(grpc::ServerContext* context,
                                          const pqfl::WorkerHeartbeat* request,
                                          pqfl::WorkerHeartbeatAck* response) {
    if (!IsAuthorizedWorker(context)) {
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "Worker certificate required");
    }
    std::string validation_error = ValidateIdentifier(request->worker_id(), "worker_id");
    if (!validation_error.empty()) {
        response->set_accepted(false);
        response->set_server_time(CurrentTimestamp());
        return grpc::Status::OK;
    }

    response->set_accepted(state_store_.RecordWorkerHeartbeat(
        request->worker_id(),
        request->active_task_id(),
        request->detail()
    ));
    response->set_server_time(CurrentTimestamp());
    return grpc::Status::OK;
}
