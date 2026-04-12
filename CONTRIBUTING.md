# Contributing

## Build

Everything runs through Docker. Native host builds are not supported.

```bash
make build   # build the Docker image
make test    # run the offline self-test
make certs   # generate ML-DSA-87 certificates
make run     # start the server container
```

## Code style

C++17 with the `.clang-format` in the repo root. Run `clang-format -i server/*.cpp server/*.hpp` before committing.

Proto files use `proto3` syntax and follow the existing naming conventions.

## Tests

- `pqfl_self_test` covers crypto round-trip, key rotation, session key derivation, and state store lifecycle.
- `make smoke` runs a full end-to-end with two workers + admin bootstrap.
- `scripts/run_smoke_verification.sh` (or `.ps1` for Windows) does the same from scratch.

## Adding a new aggregation strategy

1. Add the method to `WeightBuffer` in `federated_service.cpp`.
2. Wire it into `AddWeightAndCheckAggregate` alongside the existing `krum`/`trimmed_mean`/`fedavg` branches.
3. Add the name to the validation set in `config.cpp`.
4. Update the proto comment in `pqfl.proto` if needed.

## Adding a new admin RPC

1. Define the request/response in `proto/admin.proto`.
2. Add the RPC to the `Admin` service definition.
3. Implement the handler in `admin_service.cpp`.
4. Add the subcommand to `admin_cli.cpp`.
