#ifndef WORKER_SERVICE_HPP
#define WORKER_SERVICE_HPP

#include <grpcpp/grpcpp.h>
#include "worker.grpc.pb.h"
#include "crypto_engine.hpp"
#include "federated_service.hpp"
#include "state_store.hpp"

class WorkerServiceImpl final : public pqfl::WorkerCoordinator::Service {
public:
    WorkerServiceImpl(StateStore& state_store,
                      ClientRegistry& registry,
                      CryptoEngine& crypto);

    grpc::Status RegisterWorker(grpc::ServerContext* context,
                                const pqfl::WorkerRegistration* request,
                                pqfl::WorkerRegistrationResponse* response) override;
    grpc::Status LeaseTrainingTask(grpc::ServerContext* context,
                                   const pqfl::WorkerTaskLeaseRequest* request,
                                   pqfl::WorkerTaskLeaseResponse* response) override;
    grpc::Status ReportTaskStatus(grpc::ServerContext* context,
                                  const pqfl::WorkerTaskStatusUpdate* request,
                                  pqfl::WorkerTaskStatusAck* response) override;
    grpc::Status Heartbeat(grpc::ServerContext* context,
                           const pqfl::WorkerHeartbeat* request,
                           pqfl::WorkerHeartbeatAck* response) override;

private:
    StateStore& state_store_;
    ClientRegistry& registry_;
    CryptoEngine& crypto_;
};

#endif // WORKER_SERVICE_HPP
