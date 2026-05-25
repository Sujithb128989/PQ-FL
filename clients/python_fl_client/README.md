# Python FL Training Client

A Python federated learning client that trains a small CNN on an image dataset shard and submits encrypted weight updates to the PQ-FL server.

## Setup

```bash
pip install -r requirements.txt

# Generate Python proto stubs
python -m grpc_tools.protoc \
    -I../../proto \
    --python_out=gen \
    --grpc_python_out=gen \
    ../../proto/pqfl.proto

mkdir -p gen && touch gen/__init__.py
```

## Usage

```bash
# With the server running and certs/payload.key available:
python train.py \
    --client-id python-worker-1 \
    --round 1 \
    --shard 0 \
    --epochs 1 \
    --certs /app/certs \
    --payload-key /app/data/payload.key
```

Run multiple clients with different `--shard` values (0-3) to simulate federated training across data partitions. Once `required_clients` have submitted, the server aggregates and produces a new global model checkpoint.

## What it does

1. Registers with the PQ-FL server over mutual TLS
2. Fetches training configuration (learning rate, batch size, aggregation strategy)
3. Trains a ~21k-parameter CNN on its dataset shard
4. Derives a per-session AES-256-GCM key using the same HMAC-SHA256 KDF as the C++ server
5. Encrypts the flattened weight tensor and submits it
6. Reports whether aggregation was triggered

## Model

Small CNN: `Conv(1→8) → Conv(8→16) → FC(784→64) → FC(64→10)`. About 21,000 parameters. Enough to demonstrate real gradient flow through the FL pipeline without requiring a GPU.
