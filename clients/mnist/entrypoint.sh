#!/bin/sh
set -e

# PQ-FL MNIST Client Entrypoint
# Starts stunnel (OQS Dilithium5 mTLS sidecar) then runs the Python trainer.

echo "=== PQ-FL MNIST Client (PQC mTLS via stunnel) ==="

# Verify stunnel is built against OQS-OpenSSL
echo "stunnel TLS backend:"
stunnel -version 2>&1 | head -3 || true
echo ""

# Verify the client certificate is Dilithium5
echo "Client certificate algorithm:"
/opt/openssl/bin/openssl x509 -in /app/certs/client.crt -noout -text 2>/dev/null \
  | grep -A1 "Public Key Algorithm" || echo "(could not inspect cert)"
echo ""

# Start stunnel in background — it will listen on 127.0.0.1:50052
# and forward to pqfl-server:50051 with Dilithium5 mTLS
echo "Starting stunnel OQS mTLS proxy (127.0.0.1:50052 -> pqfl-server:50051)..."
stunnel /app/stunnel-client.conf &
STUNNEL_PID=$!

# Wait for stunnel to bind
sleep 1

# Verify stunnel is running
if ! kill -0 "$STUNNEL_PID" 2>/dev/null; then
  echo "FATAL: stunnel failed to start"
  exit 1
fi
echo "stunnel running (pid=$STUNNEL_PID)"
echo ""

# Run the Python MNIST trainer in proxy mode
# --proxy-mode: uses insecure channel to local stunnel (TLS handled by stunnel)
# --address: connects to stunnel on localhost:50052
echo "Starting MNIST trainer in proxy mode..."
python3 /app/client/train.py \
  --proxy-mode \
  --address localhost:50052 \
  "$@"
EXIT_CODE=$?

# Cleanup
kill "$STUNNEL_PID" 2>/dev/null || true
exit $EXIT_CODE
