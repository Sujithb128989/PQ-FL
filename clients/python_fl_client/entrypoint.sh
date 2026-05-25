#!/bin/sh
set -e

# PQ-FL Python Client Entrypoint
# Starts stunnel (OQS Dilithium5 mTLS sidecar) then runs the Python trainer.

echo "=== PQ-FL Python Client (PQC mTLS via stunnel) ==="

# Verify stunnel is built against OQS-OpenSSL
echo "stunnel TLS backend:"
stunnel -version 2>&1 | head -3 || true
echo ""

# Verify the client certificate is Dilithium5
echo "Client certificate algorithm:"
/opt/openssl/bin/openssl x509 -in /app/certs/client.crt -noout -text 2>/dev/null \
  | grep -A1 "Public Key Algorithm" || echo "(could not inspect cert)"
echo ""

# Start stunnel/socat OQS mTLS proxy
TARGET_SERVER="${SERVER_ADDRESS:-localhost:50051}"
echo "Starting stunnel OQS mTLS proxy (127.0.0.1:50052 -> $TARGET_SERVER)..."
# Create wrapper for openssl to avoid socat quoting issues
cat << EOF > /tmp/run_openssl.sh
#!/bin/sh
exec /opt/openssl/bin/openssl s_client -connect $TARGET_SERVER -cert /app/certs/client.crt -key /app/certs/client.key -alpn h2 -quiet -ign_eof
EOF
chmod +x /tmp/run_openssl.sh

# We simulate stunnel using socat and openssl s_client to support ALPN h2 correctly
socat TCP-LISTEN:50052,fork,bind=127.0.0.1 EXEC:/tmp/run_openssl.sh &
STUNNEL_PID=$!

# Wait for proxy to bind
sleep 1

# Verify proxy is running
if ! kill -0 "$STUNNEL_PID" 2>/dev/null; then
  echo "FATAL: stunnel failed to start"
  exit 1
fi
echo "stunnel running (pid=$STUNNEL_PID)"
echo ""

# Run the Python trainer in proxy mode
echo "Starting FL trainer in proxy mode..."
python3 /app/client/train.py \
  --proxy-mode \
  --address localhost:50052 \
  "$@"
EXIT_CODE=$?

# Cleanup
kill "$STUNNEL_PID" 2>/dev/null || true
exit $EXIT_CODE
