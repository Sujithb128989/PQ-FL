#!/bin/sh
set -eu

IMAGE_NAME="${IMAGE_NAME:-pqfl-server}"
CONTAINER_NAME="${CONTAINER_NAME:-pqfl-smoke-server}"
ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
CERT_DIR="${ROOT_DIR}/certs-test"
DATA_DIR="${ROOT_DIR}/smoke-data"
MODELS_DIR="${ROOT_DIR}/smoke-models"

rm -rf "${CERT_DIR}" "${DATA_DIR}" "${MODELS_DIR}"
mkdir -p "${CERT_DIR}" "${DATA_DIR}" "${MODELS_DIR}"

docker build -t "${IMAGE_NAME}" "${ROOT_DIR}"
docker run --rm "${IMAGE_NAME}" /app/pqfl_self_test
docker run --rm -v "${CERT_DIR}:/app/certs" "${IMAGE_NAME}" /app/scripts/generate_certs.sh /app/certs

docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true

cleanup() {
  docker rm -f "${CONTAINER_NAME}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker run -d --name "${CONTAINER_NAME}" \
  -v "${CERT_DIR}:/app/certs:ro" \
  -v "${DATA_DIR}:/app/data" \
  -v "${MODELS_DIR}:/app/models" \
  "${IMAGE_NAME}" >/dev/null

sleep 5

docker run --rm --network "container:${CONTAINER_NAME}" \
  -v "${CERT_DIR}:/app/certs:ro" \
  "${IMAGE_NAME}" /app/pqfl_admin_demo

docker run --rm --network "container:${CONTAINER_NAME}" \
  -v "${CERT_DIR}:/app/certs:ro" \
  -v "${DATA_DIR}:/app/data:ro" \
  "${IMAGE_NAME}" /app/pqfl_worker_demo demo-worker-1 demo-worker-1-trainer

docker run --rm --network "container:${CONTAINER_NAME}" \
  -v "${CERT_DIR}:/app/certs:ro" \
  -v "${DATA_DIR}:/app/data:ro" \
  "${IMAGE_NAME}" /app/pqfl_worker_demo demo-worker-2 demo-worker-2-trainer

docker run --rm --network "container:${CONTAINER_NAME}" \
  -v "${CERT_DIR}:/app/certs:ro" \
  -v "${DATA_DIR}:/app/data:ro" \
  "${IMAGE_NAME}" /app/pqfl_worker_demo demo-worker-1 demo-worker-1-trainer

docker run --rm --network "container:${CONTAINER_NAME}" \
  -v "${CERT_DIR}:/app/certs:ro" \
  -v "${DATA_DIR}:/app/data:ro" \
  "${IMAGE_NAME}" /app/pqfl_worker_demo demo-worker-2 demo-worker-2-trainer

STATUS_OUTPUT="$(docker run --rm --network "container:${CONTAINER_NAME}" \
  -v "${CERT_DIR}:/app/certs:ro" \
  "${IMAGE_NAME}" /app/pqfl_admin_cli status)"

printf '%s\n' "${STATUS_OUTPUT}"

echo "${STATUS_OUTPUT}" | grep -q "^registered_clients=2$"
echo "${STATUS_OUTPUT}" | grep -q "^completed_rounds=2$"
echo "${STATUS_OUTPUT}" | grep -q "^stored_models=2$"
echo "${STATUS_OUTPUT}" | grep -q "^registered_workers=2$"
echo "${STATUS_OUTPUT}" | grep -q "^active_worker_tasks=0$"

test -f "${DATA_DIR}/state.json"
test -f "${MODELS_DIR}/demo-tenant/demo-model/global_model_round_1.bin"
test -f "${MODELS_DIR}/demo-tenant/demo-model/global_model_round_2.bin"

echo "Smoke verification passed."
