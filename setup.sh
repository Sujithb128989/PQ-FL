#!/bin/bash
set -e

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo -e "${GREEN}=== PQ-FL: Post-Quantum Federated Learning Pipeline ===${NC}"
echo -e "${GREEN}=== Setup & Integration Test ===${NC}\n"

IMAGE_NAME="pqfl-server"
CONTAINER_NAME="pqfl-server-container"

echo -e "${YELLOW}Step 1: Cleaning up previous runs...${NC}"
docker stop $CONTAINER_NAME > /dev/null 2>&1 || true
docker rm $CONTAINER_NAME > /dev/null 2>&1 || true
docker stop pqfl-cert-gen > /dev/null 2>&1 || true
docker rm pqfl-cert-gen > /dev/null 2>&1 || true
mkdir -p certs
mkdir -p data
mkdir -p models
echo -e "${GREEN}Cleanup complete.${NC}"

echo -e "\n${YELLOW}Step 2: Building PQ-FL server image...${NC}"
docker build -t $IMAGE_NAME -f Dockerfile .
echo -e "${GREEN}Server image built successfully!${NC}"

echo -e "\n${YELLOW}Step 3: Running self-tests...${NC}"
docker run --rm $IMAGE_NAME /app/pqfl_self_test
echo -e "${GREEN}Self-tests passed.${NC}"

echo -e "\n${YELLOW}Step 4: Generating ML-DSA-87 certificates...${NC}"
docker run --name pqfl-cert-gen -v "$(pwd)/certs:/app/certs" $IMAGE_NAME \
    sh -c "/app/scripts/generate_certs.sh /app/certs && chown -R $(id -u):$(id -g) /app/certs"
docker rm pqfl-cert-gen > /dev/null 2>&1
echo -e "${GREEN}Certificates generated.${NC}"

echo -e "\n${YELLOW}Step 5: Starting PQ-FL server...${NC}"
CONTAINER_ID=$(docker run -d -p 50051:50051 \
    --name $CONTAINER_NAME \
    -v "$(pwd)/certs:/app/certs:ro" \
    -v "$(pwd)/models:/app/models" \
    -v "$(pwd)/data:/app/data" \
    $IMAGE_NAME)

trap "echo -e '\n${YELLOW}Stopping server...${NC}'; docker stop $CONTAINER_ID; docker rm $CONTAINER_ID" EXIT

echo "Waiting for server to initialize..."
sleep 5
echo "--- Server logs ---"
docker logs --tail 20 $CONTAINER_ID
echo "---"

echo -e "\n${YELLOW}Step 6: Bootstrapping dataset + training job...${NC}"
docker run --rm --network container:$CONTAINER_NAME \
    -v "$(pwd)/certs:/app/certs:ro" \
    $IMAGE_NAME /app/pqfl_admin_demo
echo -e "${GREEN}Bootstrap complete.${NC}"

echo -e "\n${YELLOW}Step 7: Running two worker sidecars for round 1...${NC}"
docker run --rm --network container:$CONTAINER_NAME \
    -v "$(pwd)/certs:/app/certs:ro" \
    -v "$(pwd)/data:/app/data:ro" \
    $IMAGE_NAME /app/pqfl_worker_demo demo-worker-1 demo-worker-1-trainer
docker run --rm --network container:$CONTAINER_NAME \
    -v "$(pwd)/certs:/app/certs:ro" \
    -v "$(pwd)/data:/app/data:ro" \
    $IMAGE_NAME /app/pqfl_worker_demo demo-worker-2 demo-worker-2-trainer
echo -e "\n${YELLOW}Step 8: Replaying signed base model in round 2...${NC}"
docker run --rm --network container:$CONTAINER_NAME \
    -v "$(pwd)/certs:/app/certs:ro" \
    -v "$(pwd)/data:/app/data:ro" \
    $IMAGE_NAME /app/pqfl_worker_demo demo-worker-1 demo-worker-1-trainer
docker run --rm --network container:$CONTAINER_NAME \
    -v "$(pwd)/certs:/app/certs:ro" \
    -v "$(pwd)/data:/app/data:ro" \
    $IMAGE_NAME /app/pqfl_worker_demo demo-worker-2 demo-worker-2-trainer
echo -e "${GREEN}Worker sidecar demo complete.${NC}"

echo -e "\n${YELLOW}Step 9: Verifying final control-plane state...${NC}"
STATUS_OUTPUT=$(docker run --rm --network container:$CONTAINER_NAME \
    -v "$(pwd)/certs:/app/certs:ro" \
    $IMAGE_NAME /app/pqfl_admin_cli status)
printf '%s\n' "$STATUS_OUTPUT"
echo "$STATUS_OUTPUT" | grep -q "^registered_clients=2$"
echo "$STATUS_OUTPUT" | grep -q "^completed_rounds=2$"
echo "$STATUS_OUTPUT" | grep -q "^stored_models=2$"
echo "$STATUS_OUTPUT" | grep -q "^registered_workers=2$"
echo "$STATUS_OUTPUT" | grep -q "^active_worker_tasks=0$"
test -f "$(pwd)/models/demo-tenant/demo-model/global_model_round_1.bin"
test -f "$(pwd)/models/demo-tenant/demo-model/global_model_round_2.bin"
echo -e "${GREEN}Verification complete.${NC}"

echo -e "\n${GREEN}=== PQ-FL Server Running ===${NC}"
echo -e "${GREEN}gRPC endpoint: localhost:50051${NC}"
echo -e "${GREEN}Model checkpoints: ./models/${NC}\n"
