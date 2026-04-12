#!/bin/sh
set -e

# PQ-FL Certificate Generation using ML-DSA-87 (NIST FIPS 204)
# Uses OQS-OpenSSL with liboqs 0.12.0 for post-quantum signature scheme.

SIG_ALG="dilithium5"
if [ -z "$1" ]; then
    echo "Usage: $0 <certificate_directory>"
    exit 1
fi
CERT_DIR="$1"
DAYS_VALID=365

mkdir -p ${CERT_DIR}

echo "Using OpenSSL version:"
/opt/openssl/bin/openssl version
echo ""
echo "Generating PQ-FL certificates using ${SIG_ALG} (ML-DSA-87)..."

# 1. CA: self-signed root
/opt/openssl/bin/openssl req -x509 \
  -newkey ${SIG_ALG} -keyout ${CERT_DIR}/ca.key \
  -out ${CERT_DIR}/ca.crt -nodes -subj "/CN=PQ-FL-CA" -days ${DAYS_VALID}

# 2. Server key + CSR
/opt/openssl/bin/openssl req -new \
  -newkey ${SIG_ALG} -keyout ${CERT_DIR}/server.key \
  -out ${CERT_DIR}/server.csr -nodes -subj "/CN=pqfl-server"

# 3. Sign server cert with SAN (required by gRPC)
cat <<EOF > ${CERT_DIR}/san.cnf
[req]
distinguished_name=dn
[san]
subjectAltName=DNS:localhost,DNS:pqfl-server
[dn]
EOF

/opt/openssl/bin/openssl x509 -req \
  -in ${CERT_DIR}/server.csr -out ${CERT_DIR}/server.crt \
  -CA ${CERT_DIR}/ca.crt -CAkey ${CERT_DIR}/ca.key -CAcreateserial \
  -days ${DAYS_VALID} -extfile ${CERT_DIR}/san.cnf -extensions san

# 4. Client key + cert
/opt/openssl/bin/openssl req -new \
  -newkey ${SIG_ALG} -keyout ${CERT_DIR}/client.key \
  -out ${CERT_DIR}/client.csr -nodes -subj "/CN=pqfl-edge-client"

/opt/openssl/bin/openssl x509 -req \
  -in ${CERT_DIR}/client.csr -out ${CERT_DIR}/client.crt \
  -CA ${CERT_DIR}/ca.crt -CAkey ${CERT_DIR}/ca.key -CAcreateserial \
  -days ${DAYS_VALID}

# 5. Admin key + cert
/opt/openssl/bin/openssl req -new \
  -newkey ${SIG_ALG} -keyout ${CERT_DIR}/admin.key \
  -out ${CERT_DIR}/admin.csr -nodes -subj "/CN=pqfl-admin"

/opt/openssl/bin/openssl x509 -req \
  -in ${CERT_DIR}/admin.csr -out ${CERT_DIR}/admin.crt \
  -CA ${CERT_DIR}/ca.crt -CAkey ${CERT_DIR}/ca.key -CAcreateserial \
  -days ${DAYS_VALID}

# 6. Worker key + cert
/opt/openssl/bin/openssl req -new \
  -newkey ${SIG_ALG} -keyout ${CERT_DIR}/worker.key \
  -out ${CERT_DIR}/worker.csr -nodes -subj "/CN=pqfl-worker"

/opt/openssl/bin/openssl x509 -req \
  -in ${CERT_DIR}/worker.csr -out ${CERT_DIR}/worker.crt \
  -CA ${CERT_DIR}/ca.crt -CAkey ${CERT_DIR}/ca.key -CAcreateserial \
  -days ${DAYS_VALID}

# Cleanup
rm ${CERT_DIR}/*.csr ${CERT_DIR}/*.srl ${CERT_DIR}/san.cnf

echo ""
echo "✓ PQ-FL certificates generated (${SIG_ALG} / ML-DSA-87):"
ls -lh ${CERT_DIR}

echo ""
echo "Certificate algorithm verification:"
/opt/openssl/bin/openssl x509 -in ${CERT_DIR}/ca.crt -noout -text | grep -A2 "Public Key Algorithm"
