#include <filesystem>
#include <iostream>
#include <stdexcept>
#include "crypto_engine.hpp"
#include "state_store.hpp"

namespace {

void Assert(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestCryptoRotation(const std::filesystem::path& temp_root) {
    CryptoEngine crypto((temp_root / "master.key").string());
    std::string blob_v1 = crypto.EncryptBlob("hello-world");
    Assert(crypto.DecryptBlob(blob_v1) == "hello-world", "initial decrypt failed");

    int rotated_version = crypto.RotateMasterKey();
    Assert(rotated_version == 2, "key rotation did not advance version");

    std::string blob_v2 = crypto.EncryptBlob("hello-world-v2");
    Assert(crypto.DecryptBlob(blob_v1) == "hello-world", "old blob decrypt failed after rotation");
    Assert(crypto.DecryptBlob(blob_v2) == "hello-world-v2", "new blob decrypt failed after rotation");
}

void TestSessionKeyDerivationUsesSecret() {
    std::vector<unsigned char> secret_a(32, 0x11);
    std::vector<unsigned char> secret_b(32, 0x22);
    std::vector<std::string> context = {"pqfl-worker", "tenant-a", "model-a", "client-1", "1", "submit"};

    std::vector<unsigned char> key_a1 = CryptoEngine::DeriveSessionKeyWithSecret(secret_a, context);
    std::vector<unsigned char> key_a2 = CryptoEngine::DeriveSessionKeyWithSecret(secret_a, context);
    std::vector<unsigned char> key_b = CryptoEngine::DeriveSessionKeyWithSecret(secret_b, context);
    std::vector<unsigned char> key_context_changed = CryptoEngine::DeriveSessionKeyWithSecret(
        secret_a,
        {"pqfl-worker", "tenant-a", "model-a", "client-1", "2", "submit"});

    Assert(key_a1 == key_a2, "same secret and context must derive the same key");
    Assert(key_a1 != key_b, "different secrets must derive different keys");
    Assert(key_a1 != key_context_changed, "different contexts must derive different keys");
}

void TestKemSessionKeyAgreement(const std::filesystem::path& temp_root) {
    CryptoEngine crypto((temp_root / "master.key").string());
    std::vector<unsigned char> kem_ciphertext;
    std::vector<unsigned char> shared_secret;
    Assert(CryptoEngine::EncapsulateKem(crypto.GetKemPublicKey(), kem_ciphertext, shared_secret),
           "ML-KEM-1024 encapsulation should succeed");

    std::vector<std::string> context = {"pqfl-worker", "tenant-a", "model-a", "client-1", "1", "submit"};
    auto client_key = CryptoEngine::DeriveSessionKeyFromKemSecret(shared_secret, context);
    auto server_key = crypto.DeriveKemSessionKey(context, kem_ciphertext);
    Assert(client_key == server_key, "ML-KEM-1024 client and server keys should match");
}

void TestStateStoreLifecycle(const std::filesystem::path& temp_root) {
    StateStore store((temp_root / "state.json").string());
    store.Load();
    store.RecordClientRegistration({
        "tenant-a", "model-a", "client-1", "pqfl-edge-client", 42, "x86", "dataset-a", -1,
        "2026-04-09T00:00:00.000Z"
    });
    auto clients = store.ListClients("tenant-a", "model-a");
    Assert(clients.size() == 1, "client should be persisted");
    Assert(clients.front().peer_identity == "pqfl-edge-client", "peer identity should be persisted");
    store.RecordWeightSubmission("tenant-a", "model-a", "client-1", 1);
    int version = store.RecordAggregatedModel(
        "tenant-a", "model-a", 1,
        (temp_root / "model.bin").string(),
        "fedavg", 1, 1, 4
    );
    Assert(version == 1, "first model version should be 1");
    Assert(store.GetActiveModelVersion("tenant-a", "model-a") == 1, "active model version mismatch");

    PersistedModelRecord should_not_restore;
    Assert(!store.RollbackModel("tenant-a", "model-a", 99, &should_not_restore),
           "rollback to a missing version should fail");
    Assert(store.GetActiveModelVersion("tenant-a", "model-a") == 1,
           "failed rollback must preserve the current active model");

    PersistedModelRecord restored;
    Assert(store.RollbackModel("tenant-a", "model-a", 1, &restored), "rollback should succeed");
    Assert(restored.version == 1, "rollback returned wrong version");

    auto status = store.GetSystemStatus();
    Assert(status.registered_clients == 1, "registered client count mismatch");
    Assert(status.completed_rounds == 1, "completed rounds mismatch");
    Assert(status.total_submissions == 1, "submission count mismatch");
    Assert(status.stored_models == 1, "stored models count mismatch");

    PersistedDatasetRecord dataset = store.UpsertDataset({
        "tenant-a", "model-a", "dataset-a", "fingerprint-a", "s3://bucket/dataset-a",
        "parquet", 1000, 4, true, ""
    });
    Assert(dataset.dataset_id == "dataset-a", "dataset upsert failed");

    PersistedTrainingJobRecord job = store.CreateTrainingJob({
        "", "tenant-a", "model-a", "dataset-a", 2, 2, 2, 120, "planned", 1, 0, 0, "", "", "test job"
    });
    Assert(!job.job_id.empty(), "job id should be populated");

    PersistedAssignmentLease assignment;
    PersistedTrainingJobRecord leased_job;
    PersistedDatasetRecord leased_dataset;
    Assert(store.LeaseTrainingAssignment("tenant-a", "model-a", "client-1", 1, 2, 120,
                                         &assignment, &leased_job, &leased_dataset),
           "assignment lease should succeed");
    Assert(assignment.round_index == 1, "leased round mismatch");
    Assert(leased_dataset.dataset_id == "dataset-a", "leased dataset mismatch");

    std::string assignment_error;
    Assert(store.ValidateTrainingAssignmentSubmission("tenant-a", "model-a", "client-1",
                                                      leased_job.job_id, assignment.assignment_id, 1, 1,
                                                      &assignment_error),
           "assignment validation should succeed");
    Assert(store.AcceptTrainingAssignmentSubmission("tenant-a", "model-a", "client-1",
                                                    leased_job.job_id, assignment.assignment_id, 1, 1,
                                                    &assignment_error),
           "assignment submission should succeed");

    PersistedWorkerRecord worker = store.UpsertWorker({
        "worker-1", "client-1", "tenant-a", "model-a", "host-a", "sidecar", 1,
        {"cpu"}, "idle", "", "", ""
    });
    Assert(worker.worker_id == "worker-1", "worker upsert failed");

    PersistedAssignmentLease assignment2;
    PersistedTrainingJobRecord leased_job2;
    PersistedDatasetRecord leased_dataset2;
    Assert(store.LeaseTrainingAssignment("tenant-a", "model-a", "client-1", 1, 2, 120,
                                         &assignment2, &leased_job2, &leased_dataset2),
           "second assignment lease should succeed");
    PersistedWorkerTaskRecord task = store.LeaseWorkerTask(worker, assignment2, leased_job2, leased_dataset2);
    Assert(!task.task_id.empty(), "worker task id should be populated");
    std::string worker_error;
    Assert(store.UpdateWorkerTaskStatus("worker-1", task.task_id, "running", "started", 50, &worker_error),
           "worker task should transition to running");
    Assert(store.RecordWorkerHeartbeat("worker-1", task.task_id, "still running"),
           "worker heartbeat should succeed");

    store.AdvanceTrainingJobRound("tenant-a", "model-a", 1, 2);

    auto jobs = store.ListTrainingJobs("tenant-a", "model-a");
    Assert(!jobs.empty(), "training jobs should be listed");
    Assert(jobs.back().completed_rounds == 1, "completed training rounds should advance");
    Assert(jobs.back().current_round == 2, "next training round should advance");
    auto workers = store.ListWorkers("tenant-a", "model-a");
    Assert(workers.size() == 1, "worker should be listed");
    auto worker_tasks = store.ListWorkerTasks("tenant-a", "model-a", "worker-1");
    Assert(!worker_tasks.empty(), "worker task should be listed");
}

} // namespace

int main() {
    std::filesystem::path temp_root = std::filesystem::temp_directory_path() / "pqfl-self-test";
    std::filesystem::remove_all(temp_root);
    std::filesystem::create_directories(temp_root);

    try {
        TestCryptoRotation(temp_root / "crypto");
        TestSessionKeyDerivationUsesSecret();
        TestKemSessionKeyAgreement(temp_root / "kem");
        TestStateStoreLifecycle(temp_root / "state");
        std::cout << "pqfl_self_test: PASS" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "pqfl_self_test: FAIL: " << e.what() << std::endl;
        return 1;
    }

    std::filesystem::remove_all(temp_root);
    return 0;
}
