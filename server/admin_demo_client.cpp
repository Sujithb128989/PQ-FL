#include <grpcpp/grpcpp.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include "admin.grpc.pb.h"

namespace {

constexpr char kDefaultAddress[] = "localhost:50051";
constexpr char kTenantId[] = "demo-tenant";
constexpr char kModelId[] = "demo-model";
constexpr char kDatasetId[] = "demo-dataset";

std::string ReadFile(const std::string& filepath) {
    std::ifstream file(filepath);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

class AdminDemoClient {
public:
    explicit AdminDemoClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(pqfl::Admin::NewStub(channel)) {}

    bool Bootstrap() {
        pqfl::UpsertDatasetRequest dataset_req;
        dataset_req.set_tenant_id(kTenantId);
        dataset_req.set_model_id(kModelId);
        dataset_req.set_dataset_id(kDatasetId);
        dataset_req.set_fingerprint("demo-dataset-v1");
        dataset_req.set_storage_uri("file:///datasets/demo-dataset");
        dataset_req.set_format("synthetic");
        dataset_req.set_sample_count(200);
        dataset_req.set_shard_count(2);
        dataset_req.set_active(true);

        pqfl::DatasetRecord dataset_res;
        grpc::ClientContext dataset_ctx;
        grpc::Status dataset_status = stub_->UpsertDataset(&dataset_ctx, dataset_req, &dataset_res);
        if (!dataset_status.ok()) {
            std::cout << "UpsertDataset failed: " << dataset_status.error_message() << std::endl;
            return false;
        }

        pqfl::CreateTrainingJobRequest job_req;
        job_req.set_tenant_id(kTenantId);
        job_req.set_model_id(kModelId);
        job_req.set_dataset_id(kDatasetId);
        job_req.set_target_rounds(2);
        job_req.set_planned_clients(2);
        job_req.set_min_clients_per_round(2);
        job_req.set_assignment_ttl_seconds(180);
        job_req.set_notes("Bootstrap demo job with signature-verified round replay");

        pqfl::TrainingJob job_res;
        grpc::ClientContext job_ctx;
        grpc::Status job_status = stub_->CreateTrainingJob(&job_ctx, job_req, &job_res);
        if (!job_status.ok()) {
            std::cout << "CreateTrainingJob failed: " << job_status.error_message() << std::endl;
            return false;
        }

        std::cout << "Bootstrapped dataset " << dataset_res.dataset_id()
                  << " and training job " << job_res.job_id() << std::endl;
        return true;
    }

private:
    std::unique_ptr<pqfl::Admin::Stub> stub_;
};

int main() {
    const char* env_address = std::getenv("PQFL_ADDRESS");
    std::string address = env_address ? env_address : kDefaultAddress;
    std::string cert_dir = "/app/certs";
    grpc::SslCredentialsOptions ssl_opts;
    ssl_opts.pem_root_certs = ReadFile(cert_dir + "/ca.crt");
    ssl_opts.pem_cert_chain = ReadFile(cert_dir + "/admin.crt");
    ssl_opts.pem_private_key = ReadFile(cert_dir + "/admin.key");
    auto creds = grpc::SslCredentials(ssl_opts);

    AdminDemoClient client(grpc::CreateChannel(address, creds));
    return client.Bootstrap() ? 0 : 1;
}
