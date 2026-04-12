$ErrorActionPreference = "Stop"

$imageName = "pqfl-server"
$containerName = "pqfl-smoke-server"
$repoRoot = Split-Path -Parent $PSScriptRoot
$certDir = Join-Path $repoRoot "certs-test"
$dataDir = Join-Path $repoRoot "smoke-data"
$modelsDir = Join-Path $repoRoot "smoke-models"

foreach ($path in @($certDir, $dataDir, $modelsDir)) {
    if (Test-Path $path) {
        Remove-Item -Recurse -Force $path
    }
}
New-Item -ItemType Directory -Force -Path $certDir | Out-Null
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null
New-Item -ItemType Directory -Force -Path $modelsDir | Out-Null

docker build -t $imageName $repoRoot
docker run --rm $imageName /app/pqfl_self_test
docker run --rm -v "${certDir}:/app/certs" $imageName /app/scripts/generate_certs.sh /app/certs

try {
    docker rm -f $containerName | Out-Null
} catch {
}

docker run -d --name $containerName `
  -v "${certDir}:/app/certs:ro" `
  -v "${dataDir}:/app/data" `
  -v "${modelsDir}:/app/models" `
  $imageName | Out-Null

try {
    Start-Sleep -Seconds 5
    docker run --rm --network "container:$containerName" -v "${certDir}:/app/certs:ro" $imageName /app/pqfl_admin_demo
    docker run --rm --network "container:$containerName" -v "${certDir}:/app/certs:ro" -v "${dataDir}:/app/data:ro" $imageName /app/pqfl_worker_demo demo-worker-1 demo-worker-1-trainer
    docker run --rm --network "container:$containerName" -v "${certDir}:/app/certs:ro" -v "${dataDir}:/app/data:ro" $imageName /app/pqfl_worker_demo demo-worker-2 demo-worker-2-trainer
    docker run --rm --network "container:$containerName" -v "${certDir}:/app/certs:ro" -v "${dataDir}:/app/data:ro" $imageName /app/pqfl_worker_demo demo-worker-1 demo-worker-1-trainer
    docker run --rm --network "container:$containerName" -v "${certDir}:/app/certs:ro" -v "${dataDir}:/app/data:ro" $imageName /app/pqfl_worker_demo demo-worker-2 demo-worker-2-trainer
    $statusOutput = docker run --rm --network "container:$containerName" -v "${certDir}:/app/certs:ro" $imageName /app/pqfl_admin_cli status
    Write-Host $statusOutput
    if ($statusOutput -notmatch "(?m)^registered_clients=2$") {
        throw "Expected registered_clients=2"
    }
    if ($statusOutput -notmatch "(?m)^completed_rounds=2$") {
        throw "Expected completed_rounds=2"
    }
    if ($statusOutput -notmatch "(?m)^stored_models=2$") {
        throw "Expected stored_models=2"
    }
    if ($statusOutput -notmatch "(?m)^registered_workers=2$") {
        throw "Expected registered_workers=2"
    }
    if ($statusOutput -notmatch "(?m)^active_worker_tasks=0$") {
        throw "Expected active_worker_tasks=0"
    }

    $statePath = Join-Path $dataDir "state.json"
    if (-not (Test-Path $statePath)) {
        throw "state.json was not created"
    }

    $state = Get-Content $statePath -Raw | ConvertFrom-Json
    if (-not $state.models -or $state.models.Count -lt 1) {
        throw "No persisted models were found in state.json"
    }

    $checkpointPath = Join-Path $modelsDir "demo-tenant\\demo-model\\global_model_round_1.bin"
    if (-not (Test-Path $checkpointPath)) {
        throw "Encrypted checkpoint was not written"
    }
    $checkpointPath2 = Join-Path $modelsDir "demo-tenant\\demo-model\\global_model_round_2.bin"
    if (-not (Test-Path $checkpointPath2)) {
        throw "Second encrypted checkpoint was not written"
    }

    Write-Host "Smoke verification passed."
}
finally {
    try {
        docker rm -f $containerName | Out-Null
    } catch {
    }
}
