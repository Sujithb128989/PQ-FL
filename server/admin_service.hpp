#ifndef ADMIN_SERVICE_HPP
#define ADMIN_SERVICE_HPP

#include <grpcpp/grpcpp.h>
#include "admin.grpc.pb.h"
#include "crypto_engine.hpp"
#include "state_store.hpp"
#include "federated_service.hpp"

class AdminServiceImpl final : public pqfl::Admin::Service {
public:
    AdminServiceImpl(StateStore& state_store,
                     CryptoEngine& crypto,
                     WeightBuffer& buffer);

    grpc::Status GetSystemStatus(grpc::ServerContext* context,
                                 const pqfl::AdminEmpty* request,
                                 pqfl::SystemStatus* response) override;
    grpc::Status ListModels(grpc::ServerContext* context,
                            const pqfl::ListModelsRequest* request,
                            pqfl::ModelRegistrySnapshot* response) override;
    grpc::Status ListClients(grpc::ServerContext* context,
                             const pqfl::ListClientsRequest* request,
                             pqfl::RegisteredClients* response) override;
    grpc::Status ListWorkers(grpc::ServerContext* context,
                             const pqfl::ListWorkersRequest* request,
                             pqfl::RegisteredWorkers* response) override;
    grpc::Status ListWorkerTasks(grpc::ServerContext* context,
                                 const pqfl::ListWorkerTasksRequest* request,
                                 pqfl::WorkerTasksResponse* response) override;
    grpc::Status UpsertDataset(grpc::ServerContext* context,
                               const pqfl::UpsertDatasetRequest* request,
                               pqfl::DatasetRecord* response) override;
    grpc::Status ListDatasets(grpc::ServerContext* context,
                              const pqfl::ListDatasetsRequest* request,
                              pqfl::DatasetCatalog* response) override;
    grpc::Status CreateTrainingJob(grpc::ServerContext* context,
                                   const pqfl::CreateTrainingJobRequest* request,
                                   pqfl::TrainingJob* response) override;
    grpc::Status ListTrainingJobs(grpc::ServerContext* context,
                                  const pqfl::ListTrainingJobsRequest* request,
                                  pqfl::TrainingJobsResponse* response) override;
    grpc::Status RollbackModel(grpc::ServerContext* context,
                               const pqfl::RollbackModelRequest* request,
                               pqfl::RollbackModelResponse* response) override;
    grpc::Status RotateAtRestKey(grpc::ServerContext* context,
                                 const pqfl::RotateKeyRequest* request,
                                 pqfl::RotateKeyResponse* response) override;
    grpc::Status ListAuditEvents(grpc::ServerContext* context,
                                 const pqfl::AuditEventsRequest* request,
                                 pqfl::AuditEventsResponse* response) override;

private:
    StateStore& state_store_;
    CryptoEngine& crypto_;
    WeightBuffer& buffer_;
};

#endif // ADMIN_SERVICE_HPP
