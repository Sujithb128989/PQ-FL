#!/usr/bin/env python3
"""
MNIST federated training client for PQ-FL.

The client registers with the server, leases the assigned scheduled round,
loads the latest global model when one exists, trains a local MNIST shard,
and submits AES-256-GCM encrypted weights back to the control plane.
"""

import argparse
import hashlib
import hmac
import importlib
import os
import struct
import subprocess
import sys
from pathlib import Path

import grpc
import torch
import torch.nn as nn
import torch.nn.functional as F
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from torchvision import datasets, transforms

CLIENT_DIR = Path(__file__).resolve().parent
PROTO_DIR = Path(os.environ.get("PQFL_PROTO_DIR", str((CLIENT_DIR / ".." / ".." / "proto").resolve())))
GEN_DIR = CLIENT_DIR / "gen"
PEER_IDENTITY = "pqfl-edge-client"


def ensure_proto_stubs() -> None:
    GEN_DIR.mkdir(exist_ok=True)
    (GEN_DIR / "__init__.py").touch()
    source = PROTO_DIR / "pqfl.proto"
    expected = [GEN_DIR / "pqfl_pb2.py", GEN_DIR / "pqfl_pb2_grpc.py"]
    if all(path.exists() and path.stat().st_mtime >= source.stat().st_mtime for path in expected):
        return

    try:
        import grpc_tools.protoc  # noqa: F401
    except ImportError as exc:
        raise RuntimeError(
            "grpcio-tools is required to generate protobuf stubs. "
            "Run: pip install -r clients/mnist/requirements.txt"
        ) from exc

    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "grpc_tools.protoc",
            f"-I{PROTO_DIR}",
            f"--python_out={GEN_DIR}",
            f"--grpc_python_out={GEN_DIR}",
            str(source),
        ],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip() or "failed to generate protobuf stubs")


ensure_proto_stubs()
sys.path.insert(0, str(GEN_DIR))
pqfl_pb2 = importlib.import_module("pqfl_pb2")
pqfl_pb2_grpc = importlib.import_module("pqfl_pb2_grpc")


class MnistNet(nn.Module):
    """Small CNN for MNIST. About 21k parameters."""

    def __init__(self):
        super().__init__()
        self.conv1 = nn.Conv2d(1, 8, 3, padding=1)
        self.conv2 = nn.Conv2d(8, 16, 3, padding=1)
        self.pool = nn.MaxPool2d(2)
        self.fc1 = nn.Linear(16 * 7 * 7, 64)
        self.fc2 = nn.Linear(64, 10)

    def forward(self, x):
        x = self.pool(F.relu(self.conv1(x)))
        x = self.pool(F.relu(self.conv2(x)))
        x = x.view(x.size(0), -1)
        x = F.relu(self.fc1(x))
        return self.fc2(x)


def flatten_weights(model: nn.Module) -> list[float]:
    flat = []
    for param in model.parameters():
        flat.extend(param.detach().cpu().numpy().flatten().tolist())
    return flat


def load_weights(model: nn.Module, flat: list[float], device: torch.device) -> None:
    offset = 0
    for param in model.parameters():
        numel = param.numel()
        chunk = torch.tensor(flat[offset : offset + numel], dtype=param.dtype, device=device)
        param.data = chunk.view(param.shape)
        offset += numel
    if offset != len(flat):
        raise ValueError(f"unused base-model weights: consumed {offset}, got {len(flat)}")


def derive_session_key(secret: bytes, context_parts: list[str]) -> bytes:
    to_mac = "pqfl-session-v3"
    for part in context_parts:
        to_mac += f"|{len(part)}:{part}"
    return hmac.new(secret, to_mac.encode(), hashlib.sha256).digest()


def encrypt_weights(key: bytes, plaintext: bytes) -> tuple[bytes, bytes, bytes]:
    iv = os.urandom(12)
    ct_with_tag = AESGCM(key).encrypt(iv, plaintext, None)
    return ct_with_tag[:-16], iv, ct_with_tag[-16:]


def encapsulate_ml_kem(public_key: bytes) -> tuple[bytes, bytes]:
    try:
        from pqcrypto.kem import ml_kem_1024
    except ImportError as exc:
        raise RuntimeError(
            "pqcrypto is required for app-layer ML-KEM-1024. "
            "Run: pip install -r clients/mnist/requirements.txt"
        ) from exc
    ciphertext, shared_secret = ml_kem_1024.encrypt(public_key)
    return ciphertext, shared_secret


def generate_ml_kem_keypair() -> tuple[bytes, bytes]:
    try:
        from pqcrypto.kem import ml_kem_1024
    except ImportError as exc:
        raise RuntimeError(
            "pqcrypto is required for app-layer ML-KEM-1024. "
            "Run: pip install -r clients/mnist/requirements.txt"
        ) from exc
    return ml_kem_1024.generate_keypair()


def decapsulate_ml_kem(secret_key: bytes, ciphertext: bytes) -> bytes:
    try:
        from pqcrypto.kem import ml_kem_1024
    except ImportError as exc:
        raise RuntimeError(
            "pqcrypto is required for app-layer ML-KEM-1024. "
            "Run: pip install -r clients/mnist/requirements.txt"
        ) from exc
    return ml_kem_1024.decrypt(secret_key, ciphertext)


def decrypt_weights_with_key(
    key: bytes,
    ciphertext: bytes,
    iv: bytes,
    tag: bytes,
) -> list[float]:
    plaintext = AESGCM(key).decrypt(iv, ciphertext + tag, None)
    if len(plaintext) % 4 != 0:
        raise ValueError("decrypted tensor byte length is not divisible by float32 size")
    count = len(plaintext) // 4
    return list(struct.unpack(f"{count}f", plaintext))


def train_one_round(
    model: nn.Module,
    device: torch.device,
    shard_index: int,
    data_dir: str,
    total_shards: int = 4,
    epochs: int = 1,
    batch_size: int = 32,
    lr: float = 0.01,
):
    transform = transforms.Compose([
        transforms.ToTensor(),
        transforms.Normalize((0.1307,), (0.3081,)),
    ])
    full_dataset = datasets.MNIST(root=data_dir, train=True, download=True, transform=transform)

    n = len(full_dataset)
    shard_size = n // total_shards
    start = shard_index * shard_size
    end = start + shard_size if shard_index < total_shards - 1 else n
    subset = torch.utils.data.Subset(full_dataset, range(start, end))
    loader = torch.utils.data.DataLoader(
        subset,
        batch_size=batch_size,
        shuffle=True,
        pin_memory=(device.type == "cuda"),
    )

    optimizer = torch.optim.SGD(model.parameters(), lr=lr)
    model.train()

    total_loss = 0.0
    correct = 0
    total = 0
    for _ in range(epochs):
        for images, labels in loader:
            images = images.to(device, non_blocking=True)
            labels = labels.to(device, non_blocking=True)
            optimizer.zero_grad()
            output = model(images)
            loss = F.cross_entropy(output, labels)
            loss.backward()
            optimizer.step()
            total_loss += loss.item() * images.size(0)
            correct += (output.argmax(1) == labels).sum().item()
            total += images.size(0)

    return len(subset), total_loss / total if total else 0.0, correct / total if total else 0.0


def build_channel(args) -> grpc.Channel:
    ca = open(os.path.join(args.certs, "ca.crt"), "rb").read()
    cert = open(os.path.join(args.certs, "client.crt"), "rb").read()
    key = open(os.path.join(args.certs, "client.key"), "rb").read()
    creds = grpc.ssl_channel_credentials(ca, key, cert)
    return grpc.secure_channel(args.address, creds)


def build_proxy_channel(args) -> grpc.Channel:
    """Connect via insecure channel to a local stunnel OQS mTLS proxy.

    In proxy mode, stunnel handles Dilithium5 mTLS to the PQ-FL server.
    The Python client connects to stunnel on localhost over plaintext.
    App-layer ML-KEM-1024 key agreement remains fully operational.
    """
    print(f"proxy_mode: connecting to stunnel OQS mTLS proxy at {args.address}")
    return grpc.insecure_channel(args.address)


def fetch_base_model(fl_stub, args, secret: bytes, last_received_round: int) -> tuple[int, list[float]]:
    stream_public_key, stream_secret_key = generate_ml_kem_keypair()
    request = pqfl_pb2.ModelRequest(
        client_id=args.client_id,
        last_received_round=last_received_round,
        tenant_id=args.tenant,
        model_id=args.model,
        client_kem_public_key=stream_public_key,
    )
    updates = fl_stub.StreamGlobalModel(request, timeout=10)
    try:
        update = next(updates)
    except StopIteration:
        return last_received_round, []
    except grpc.RpcError as exc:
        if exc.code() == grpc.StatusCode.DEADLINE_EXCEEDED:
            return last_received_round, []
        raise

    if update.key_agreement == "ml-kem-1024":
        shared_secret = decapsulate_ml_kem(stream_secret_key, update.kem_ciphertext)
        key = derive_session_key(shared_secret, [PEER_IDENTITY, args.tenant, args.model, args.client_id, "stream"])
        print(f"base_model_key_agreement: ml-kem-1024 ciphertext_bytes={len(update.kem_ciphertext)}")
    else:
        key = derive_session_key(secret, [PEER_IDENTITY, args.tenant, args.model, args.client_id, "stream"])
        print("base_model_key_agreement: shared-secret")

    weights = decrypt_weights_with_key(
        key,
        update.encrypted_weights,
        update.iv,
        update.auth_tag,
    )
    return update.round_index, weights


def run(args) -> None:
    channel = build_proxy_channel(args) if args.proxy_mode else build_channel(args)
    fl_stub = pqfl_pb2_grpc.FederatedLearningStub(channel)
    secret = open(args.payload_key, "rb").read()
    device = torch.device(args.device if args.device != "auto" else ("cuda" if torch.cuda.is_available() else "cpu"))
    print(f"device: {device} cuda_available={torch.cuda.is_available()}")

    reg = fl_stub.RegisterClient(pqfl_pb2.ClientRegistration(
        client_id=args.client_id,
        tenant_id=args.tenant,
        model_id=args.model,
        local_dataset_size=15000,
        hardware_info=f"python-mnist-client/{device}",
        dataset_fingerprint=f"mnist-shard-{args.shard}",
    ))
    print(f"registered: accepted={reg.accepted} round={reg.current_round}")

    config = fl_stub.GetTrainingConfig(pqfl_pb2.ConfigRequest(
        client_id=args.client_id,
        tenant_id=args.tenant,
        model_id=args.model,
    ))
    print(
        f"config: ready={config.assignment_ready} round={config.assigned_round_index} "
        f"lr={config.learning_rate} strategy={config.aggregation_strategy}"
    )
    if not config.assignment_ready:
        raise RuntimeError(f"server did not assign training work: {config.assignment_message}")

    model = MnistNet().to(device)
    round_index = config.assigned_round_index
    print(f"round_source: server-assigned round={round_index}")
    if config.active_model_version > 0:
        fetched_round, base_weights = fetch_base_model(
            fl_stub,
            args,
            secret,
            max(0, round_index - 2),
        )
        if base_weights:
            expected = sum(param.numel() for param in model.parameters())
            if len(base_weights) != expected:
                raise RuntimeError(f"base model size mismatch: got {len(base_weights)}, expected {expected}")
            load_weights(model, base_weights, device)
            print(f"base_model: loaded round={fetched_round} version={config.active_model_version}")

    dataset_size, loss, acc = train_one_round(
        model,
        device,
        shard_index=args.shard,
        data_dir=args.data_dir,
        total_shards=args.total_shards,
        epochs=args.epochs,
        batch_size=config.batch_size or 32,
        lr=config.learning_rate or 0.01,
    )
    print(f"trained: loss={loss:.4f} acc={acc:.4f} samples={dataset_size}")

    nonce = f"mnist-{args.client_id}-round-{round_index}"
    key_context = [
        PEER_IDENTITY,
        args.tenant,
        args.model,
        args.client_id,
        str(round_index),
        "submit",
        nonce,
    ]
    kem_ciphertext = b""
    key_agreement = "shared-secret"
    if config.payload_kem_public_key:
        kem_ciphertext, shared_secret = encapsulate_ml_kem(config.payload_kem_public_key)
        session_key = derive_session_key(shared_secret, key_context)
        key_agreement = "ml-kem-1024"
        print(
            f"payload_key_agreement: ml-kem-1024 algorithm={config.payload_kem_algorithm} "
            f"ciphertext_bytes={len(kem_ciphertext)}"
        )
    else:
        session_key = derive_session_key(secret, key_context)
        print("payload_key_agreement: shared-secret")

    weights = flatten_weights(model)
    plaintext = struct.pack(f"{len(weights)}f", *weights)
    ciphertext, iv, tag = encrypt_weights(session_key, plaintext)

    resp = fl_stub.SubmitWeights(pqfl_pb2.WeightPayload(
        client_id=args.client_id,
        round_index=round_index,
        local_dataset_size=dataset_size,
        encrypted_weights=ciphertext,
        iv=iv,
        auth_tag=tag,
        tenant_id=args.tenant,
        model_id=args.model,
        base_model_version=config.base_model_version,
        client_nonce=nonce.encode(),
        job_id=config.job_id,
        assignment_id=config.assignment_id,
        kem_ciphertext=kem_ciphertext,
        key_agreement=key_agreement,
    ))
    print(
        f"submitted: accepted={resp.accepted} clients={resp.clients_received}/{resp.clients_required} "
        f"aggregated={resp.aggregation_triggered}"
    )
    print(f"server: {resp.message}")
    if not resp.accepted:
        raise RuntimeError(resp.message)
    channel.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="MNIST FL training client")
    parser.add_argument("--address", default="localhost:50051")
    parser.add_argument("--certs", default="/app/certs")
    parser.add_argument("--payload-key", default="/app/data/payload.key")
    parser.add_argument("--client-id", default="mnist-client-1")
    parser.add_argument("--tenant", default="demo-tenant")
    parser.add_argument("--model", default="demo-model")
    parser.add_argument("--shard", type=int, default=0, help="MNIST shard index")
    parser.add_argument("--total-shards", type=int, default=4)
    parser.add_argument("--epochs", type=int, default=1)
    parser.add_argument("--data-dir", default=str(CLIENT_DIR / "data"))
    parser.add_argument("--device", default="auto", choices=["auto", "cpu", "cuda"])
    parser.add_argument(
        "--proxy-mode",
        action="store_true",
        default=False,
        help="Connect via local stunnel OQS mTLS proxy (insecure channel to localhost)",
    )
    run(parser.parse_args())


if __name__ == "__main__":
    main()
