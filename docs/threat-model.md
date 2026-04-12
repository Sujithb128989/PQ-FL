# Threat Model

## Scope

PQ-FL is a single-node federated learning control plane. This document covers the threats it defends against and the ones it explicitly does not.

## Adversary assumptions

1. **Network adversary**: Can observe and modify traffic between clients and the server. Includes adversaries with future access to a cryptographically-relevant quantum computer (harvest-now-decrypt-later).
2. **Malicious clients**: Up to `f` out of `n` participants may submit poisoned weight updates trying to corrupt the global model.
3. **Compromised storage**: An attacker who gains read access to the server's disk should not be able to read plaintext model weights.

## Defenses

### Transport security

All gRPC channels use mutual TLS with post-quantum certificates:
- Key exchange: ML-KEM-1024 (NIST FIPS 203) via OQS-OpenSSL
- Signatures: ML-DSA-87 (NIST FIPS 204) via OQS-OpenSSL
- Both client and server authenticate via x.509 certificates signed by a shared CA

This protects against passive and active network adversaries, including those storing encrypted traffic for future quantum decryption.

### Payload encryption

Weight tensors are encrypted with AES-256-GCM using per-session keys derived from:
- A shared 32-byte payload secret
- The authenticated peer identity from the TLS certificate
- The tenant, model, client, round, and direction context

This binds the encryption to the authenticated identity of the sender and the specific RPC context, preventing replay and cross-context attacks.

### Byzantine-robust aggregation

The server supports three aggregation strategies:

| Strategy | Defense |
|---|---|
| FedAvg | None — simple weighted average, vulnerable to poisoning |
| Krum | Selects the update closest to its `n - f - 2` nearest neighbors, filtering outliers |
| Trimmed Mean | Drops the top and bottom `β` fraction per dimension before averaging |

Krum and Trimmed Mean provide statistical defenses against model poisoning by a minority of participants. They are not foolproof — a colluding majority can still corrupt the model.

### Differential privacy

After aggregation, the server optionally adds Gaussian noise calibrated to (ε, δ)-differential privacy:

```
σ = (clip_norm × √(2 × ln(1.25/δ))) / (ε × √n)
```

Gradient updates are L2-clipped to `clip_norm` before aggregation. The noise is added to the aggregated result, not to individual updates (central DP model).

### At-rest encryption

Model checkpoints are stored as AES-256-GCM encrypted blobs with versioned master keys. Key rotation generates a new master key; old keys are retained for backward compatibility. See `docs/crypto-architecture.md` for the blob format.

### Access control

- Admin RPCs require a peer certificate whose CN matches the `admin_peer_identities` allowlist.
- Worker RPCs require a peer certificate whose CN matches the `worker_peer_identities` allowlist.
- FL RPCs require an authenticated peer and enforce identity binding — a client that registered with certificate A cannot submit weights using certificate B.

## Known limitations

| Gap | Detail |
|---|---|
| No secure aggregation | The server sees plaintext weights after decryption. A real deployment should use additive masking or homomorphic encryption so the server only sees the aggregate. |
| Shared payload secret | The payload encryption key is a shared file, not negotiated per-session via KEM. A compromised secret exposes all sessions. |
| Single-node | No distributed coordination. The server is a single point of failure. |
| No client attestation | Clients prove identity via certificate but there is no hardware attestation or TEE verification. |
| Central DP only | Noise is added server-side after aggregation. Local DP (noise added on-device before submission) is not implemented. |
| No rate limiting | A malicious client can spam registration and config requests. |
| JSON state store | State is persisted to a local JSON file, not an append-only ledger or database. |
