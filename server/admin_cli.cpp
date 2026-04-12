#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "admin.grpc.pb.h"

namespace {

constexpr char kDefaultAddress[] = "localhost:50051";
constexpr char kDefaultCertDir[] = "/app/certs";

std::string ReadFile(const std::string& filepath) {
    std::ifstream file(filepath);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

void PrintHelp() {
    std::cout
        << "pqfl_admin_cli commands:\n"
        << "  status\n"
        << "  models\n"
        << "  datasets\n"
        << "  jobs\n"
        << "  workers\n"
        << "  tasks\n"
        << "  audit [limit]\n";
}

} // namespace

class AdminCli {
public:
    explicit AdminCli(std::shared_ptr<grpc::Channel> channel)
        : stub_(pqfl::Admin::NewStub(channel)) {}

    int Run(const std::string& command, int audit_limit) {
        if (command == "status") {
            return PrintStatus();
        }
        if (command == "models") {
            return PrintModels();
        }
        if (command == "datasets") {
            return PrintDatasets();
        }
        if (command == "jobs") {
            return PrintJobs();
        }
        if (command == "workers") {
            return PrintWorkers();
        }
        if (command == "tasks") {
            return PrintTasks();
        }
        if (command == "audit") {
            return PrintAudit(audit_limit);
        }

        PrintHelp();
        return 1;
    }

private:
    int PrintStatus() {
        pqfl::AdminEmpty req;
        pqfl::SystemStatus res;
        grpc::ClientContext ctx;
        grpc::Status status = stub_->GetSystemStatus(&ctx, req, &res);
        if (!status.ok()) {
            std::cerr << "GetSystemStatus failed: " << status.error_message() << std::endl;
            return 1;
        }

        std::cout
            << "registered_clients=" << res.registered_clients() << "\n"
            << "completed_rounds=" << res.completed_rounds() << "\n"
            << "total_submissions=" << res.total_submissions() << "\n"
            << "active_key_version=" << res.active_key_version() << "\n"
            << "state_path=" << res.state_path() << "\n"
            << "registered_datasets=" << res.registered_datasets() << "\n"
            << "stored_models=" << res.stored_models() << "\n"
            << "active_training_jobs=" << res.active_training_jobs() << "\n"
            << "pending_assignments=" << res.pending_assignments() << "\n"
            << "leased_assignments=" << res.leased_assignments() << "\n"
            << "submitted_assignments=" << res.submitted_assignments() << "\n"
            << "registered_workers=" << res.registered_workers() << "\n"
            << "busy_workers=" << res.busy_workers() << "\n"
            << "active_worker_tasks=" << res.active_worker_tasks() << "\n"
            << "failed_worker_tasks=" << res.failed_worker_tasks() << std::endl;
        return 0;
    }

    int PrintModels() {
        pqfl::ListModelsRequest req;
        pqfl::ModelRegistrySnapshot res;
        grpc::ClientContext ctx;
        grpc::Status status = stub_->ListModels(&ctx, req, &res);
        if (!status.ok()) {
            std::cerr << "ListModels failed: " << status.error_message() << std::endl;
            return 1;
        }
        for (const auto& model : res.models()) {
            std::cout << model.tenant_id() << "/" << model.model_id()
                      << " version=" << model.active_version()
                      << " round=" << model.latest_round()
                      << " key_version=" << model.key_version()
                      << " checkpoint=" << model.checkpoint_path() << std::endl;
        }
        return 0;
    }

    int PrintDatasets() {
        pqfl::ListDatasetsRequest req;
        pqfl::DatasetCatalog res;
        grpc::ClientContext ctx;
        grpc::Status status = stub_->ListDatasets(&ctx, req, &res);
        if (!status.ok()) {
            std::cerr << "ListDatasets failed: " << status.error_message() << std::endl;
            return 1;
        }
        for (const auto& dataset : res.datasets()) {
            std::cout << dataset.tenant_id() << "/" << dataset.model_id()
                      << " dataset=" << dataset.dataset_id()
                      << " format=" << dataset.format()
                      << " uri=" << dataset.storage_uri()
                      << " samples=" << dataset.sample_count()
                      << std::endl;
        }
        return 0;
    }

    int PrintJobs() {
        pqfl::ListTrainingJobsRequest req;
        pqfl::TrainingJobsResponse res;
        grpc::ClientContext ctx;
        grpc::Status status = stub_->ListTrainingJobs(&ctx, req, &res);
        if (!status.ok()) {
            std::cerr << "ListTrainingJobs failed: " << status.error_message() << std::endl;
            return 1;
        }
        for (const auto& job : res.jobs()) {
            std::cout << job.job_id()
                      << " status=" << job.status()
                      << " dataset=" << job.dataset_id()
                      << " round=" << job.current_round()
                      << "/" << job.target_rounds()
                      << " min_clients=" << job.min_clients_per_round()
                      << std::endl;
        }
        return 0;
    }

    int PrintWorkers() {
        pqfl::ListWorkersRequest req;
        pqfl::RegisteredWorkers res;
        grpc::ClientContext ctx;
        grpc::Status status = stub_->ListWorkers(&ctx, req, &res);
        if (!status.ok()) {
            std::cerr << "ListWorkers failed: " << status.error_message() << std::endl;
            return 1;
        }
        for (const auto& worker : res.workers()) {
            std::cout << worker.worker_id()
                      << " trainer=" << worker.trainer_client_id()
                      << " status=" << worker.status()
                      << " active_task=" << worker.active_task_id()
                      << std::endl;
        }
        return 0;
    }

    int PrintTasks() {
        pqfl::ListWorkerTasksRequest req;
        pqfl::WorkerTasksResponse res;
        grpc::ClientContext ctx;
        grpc::Status status = stub_->ListWorkerTasks(&ctx, req, &res);
        if (!status.ok()) {
            std::cerr << "ListWorkerTasks failed: " << status.error_message() << std::endl;
            return 1;
        }
        for (const auto& task : res.tasks()) {
            std::cout << task.task_id()
                      << " worker=" << task.worker_id()
                      << " status=" << task.status()
                      << " progress=" << task.progress_percent()
                      << std::endl;
        }
        return 0;
    }

    int PrintAudit(int limit) {
        pqfl::AuditEventsRequest req;
        req.set_limit(limit);
        pqfl::AuditEventsResponse res;
        grpc::ClientContext ctx;
        grpc::Status status = stub_->ListAuditEvents(&ctx, req, &res);
        if (!status.ok()) {
            std::cerr << "ListAuditEvents failed: " << status.error_message() << std::endl;
            return 1;
        }
        for (const auto& event : res.events()) {
            std::cout << event.timestamp() << " " << event.action()
                      << " tenant=" << event.tenant_id()
                      << " model=" << event.model_id()
                      << " client=" << event.client_id()
                      << " detail=\"" << event.detail() << "\""
                      << std::endl;
        }
        return 0;
    }

    std::unique_ptr<pqfl::Admin::Stub> stub_;
};

int main(int argc, char** argv) {
    const char* env_address = std::getenv("PQFL_ADDRESS");
    const char* env_cert_dir = std::getenv("PQFL_CERT_DIR");
    std::string address = env_address ? env_address : kDefaultAddress;
    std::string cert_dir = env_cert_dir ? env_cert_dir : kDefaultCertDir;
    std::string command = argc > 1 ? argv[1] : "status";
    int audit_limit = argc > 2 ? std::max(1, std::atoi(argv[2])) : 20;

    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = ReadFile(cert_dir + "/ca.crt");
    ssl_opts.pem_cert_chain = ReadFile(cert_dir + "/admin.crt");
    ssl_opts.pem_private_key = ReadFile(cert_dir + "/admin.key");

    AdminCli cli(grpc::CreateChannel(address, grpc::SslCredentials(ssl_opts)));
    return cli.Run(command, audit_limit);
}
