# Cryptographic Architecture

This document describes the layered crypto design in PQ-FL and where post-quantum algorithms are used versus classical ones.

## Layer diagram

```
┌─────────────────────────────────────────────────────┐
│                    Transport                        │
│  ML-KEM-1024 key exchange + ML-DSA-87 signatures    │
│  via OQS-OpenSSL mutual TLS                         │
└────────────────────────┬────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────┐
│                 Payload encryption                  │
│  AES-256-GCM with per-session keys                  │
│  Key = HMAC-SHA256(payload_secret, context)          │
└────────────────────────┬────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────┐
│                 At-rest encryption                  │
│  AES-256-GCM with versioned master keys             │
│  Blob format: [PQF2][key_version:4][iv:12]          │
│               [ciphertext][tag:16]                  │
└────────────────────────┬────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────┐
│                 Model integrity                     │
│  Server signs streamed model updates with its       │
│  private key (ML-DSA-87 via OQS-OpenSSL)            │
└─────────────────────────────────────────────────────┘
```

## Transport layer (post-quantum)

The Docker build compiles OQS-OpenSSL with liboqs 0.12.0. Certificate generation uses `dilithium5` (ML-DSA-87, NIST FIPS 204) for all CA, server, client, admin, and worker certificates. When gRPC performs the TLS handshake through this OpenSSL fork, the key exchange can negotiate ML-KEM-1024 (NIST FIPS 203) and signatures use ML-DSA-87.

This means the transport channel is resistant to harvest-now-decrypt-later attacks from a future quantum adversary.

Relevant files:
- `scripts/generate_certs.sh` — certificate generation with `dilithium5`
- `Dockerfile` — OQS-OpenSSL build from source

## Payload encryption (classical)

Weight tensors are encrypted end-to-end between the client and the server using AES-256-GCM. The session key for each RPC is derived as:

```
key = HMAC-SHA256(
    payload_secret,
    "pqfl-session-v3" || "|" || len(peer_identity) || ":" || peer_identity
                      || "|" || len(tenant_id) || ":" || tenant_id
                      || "|" || len(model_id) || ":" || model_id
                      || "|" || len(client_id) || ":" || client_id
                      || "|" || len(round_index) || ":" || round_index
                      || "|" || len(direction) || ":" || direction
                      [|| "|" || len(client_nonce) || ":" || client_nonce]
)
```

The length-prefixed structure prevents context confusion attacks where two different contexts could produce the same HMAC input.

Relevant files:
- `server/crypto_engine.cpp` — `DeriveSessionKeyWithSecret()`

### Why not PQ here?

The payload layer uses symmetric crypto (AES-256), which is already quantum-resistant at 256-bit key sizes (Grover's algorithm halves the effective security to 128 bits, which is still sufficient). The key derivation uses HMAC-SHA256, which is also considered safe against known quantum attacks.

The shared `payload_secret` is a 32-byte random value generated on first server boot and stored at `data/payload.key`. Both the server and demo clients read this file. In a production deployment, this would be replaced with a proper key-establishment protocol (possibly using ML-KEM for the initial key agreement).

## At-rest encryption (classical)

Model checkpoints are stored as encrypted blobs with a custom header:

```
[4 bytes: magic "PQF2"]
[4 bytes: key version, big-endian uint32]
[12 bytes: IV]
[variable: AES-256-GCM ciphertext]
[16 bytes: GCM authentication tag]
```

Key rotation generates a new 32-byte master key and increments the version. Old keys are retained in a JSON registry (`data/master.key.meta.json`) so that older checkpoints remain decryptable.

Relevant files:
- `server/crypto_engine.cpp` — `EncryptBlob()`, `DecryptBlob()`, `RotateMasterKey()`

## Model integrity (post-quantum)

When the server streams aggregated model updates to clients via `StreamGlobalModel`, each update includes a signature computed with the server's private key. Because the server certificate uses ML-DSA-87, this signature is post-quantum.

The signed payload is: `tenant_id | model_id | round_index | model_version | encrypted_weights`

Clients verify this signature against the server's public key extracted from its certificate.

Relevant files:
- `server/federated_service.cpp` — `SignData()`, `StreamGlobalModelCall::Proceed()`
- `server/worker_demo_client.cpp` — `VerifySignature()`

## Threat model summary

| Threat | Mitigation | PQ? |
|---|---|---|
| Passive eavesdropping on wire | ML-KEM-1024 + ML-DSA-87 TLS | Yes |
| Active MITM | Mutual TLS with certificate pinning | Yes (PQ signatures) |
| Harvest-now-decrypt-later | PQ key exchange at transport | Yes |
| Weight tampering in transit | AES-256-GCM authenticated encryption | Classical (sufficient) |
| Checkpoint theft at rest | AES-256-GCM with versioned keys | Classical (sufficient) |
| Model poisoning | Krum / Trimmed Mean aggregation | N/A (algorithmic) |
| Privacy leakage from gradients | Differential privacy noise + clipping | N/A (algorithmic) |
| Forged model updates | ML-DSA-87 server signature | Yes |
| Unauthorized admin access | mTLS peer identity allowlist | Yes (PQ certificates) |
