# Multi-stage build: OQS-OpenSSL + liboqs + gRPC + PQ-FL server

# STAGE 1: Build
FROM ubuntu:focal AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
      git wget cmake ninja-build build-essential \
      python3-dev python3-pip libssl-dev libc-ares-dev zlib1g-dev protobuf-compiler \
      libffi-dev patchelf pkg-config \
      nlohmann-json3-dev \
      && apt-get clean

# --- Build liboqs 0.12.0 + OQS-OpenSSL from Source ---
# liboqs 0.12.0 provides NIST-final ML-KEM-1024 and ML-DSA-87
RUN git clone --branch OQS-OpenSSL_1_1_1-stable --single-branch \
      https://github.com/open-quantum-safe/openssl.git /tmp/openssl \
    && cd /tmp/openssl \
    && git clone --branch 0.12.0 --single-branch \
         https://github.com/open-quantum-safe/liboqs.git oqs \
    && cd oqs && mkdir build && cd build \
    && cmake .. -GNinja -DCMAKE_INSTALL_PREFIX=/tmp/openssl/oqs -DOQS_BUILD_ONLY_LIB=ON \
    && ninja -j2 \
    && ninja install \
    && cd /tmp/openssl \
    && ./Configure linux-x86_64 shared --prefix=/opt/openssl \
    && make -j2 \
    && make install \
    && rm -rf /tmp/openssl

# --- Build gRPC from Source ---
RUN git clone --recurse-submodules -b v1.62.1 https://github.com/grpc/grpc /tmp/grpc \
  && cd /tmp/grpc \
  && mkdir -p build && cd build \
  && cmake .. \
       -DCMAKE_BUILD_TYPE=Release \
       -DCMAKE_INSTALL_PREFIX=/usr/local \
       -DgRPC_INSTALL=ON \
       -DgRPC_BUILD_TESTS=OFF \
       -DBUILD_SHARED_LIBS=ON \
       -DgRPC_SSL_PROVIDER=package \
       -DOPENSSL_ROOT_DIR=/opt/openssl \
       -DgRPC_ABSL_PROVIDER=module \
       -DgRPC_RE2_PROVIDER=module \
       -DgRPC_CARES_PROVIDER=package \
       -DgRPC_ZLIB_PROVIDER=package \
       -DgRPC_PROTOBUF_PROVIDER=module \
  && make -j2 \
  && make install \
  && ldconfig \
  && rm -rf /tmp/grpc

# --- Configure Environment ---
ENV PATH="/opt/openssl/bin:${PATH}"
ENV LD_LIBRARY_PATH="/opt/openssl/lib:/usr/local/lib"
ENV OpenSSL_DIR=/opt/openssl
ENV PKG_CONFIG_PATH="/opt/openssl/lib/pkgconfig:/usr/local/lib/pkgconfig"

# --- Build PQ-FL Server ---
WORKDIR /app
COPY ./server ./server
COPY ./proto ./proto
WORKDIR /app/server
RUN cmake . \
    -DOpenSSL_ROOT_DIR=/opt/openssl \
    -DCMAKE_PREFIX_PATH=/usr/local \
    -DCMAKE_BUILD_TYPE=Release
RUN make -j2 VERBOSE=1


# STAGE 2: Runtime
FROM ubuntu:focal

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
      ca-certificates libc-ares-dev patchelf \
      && apt-get clean

# Copy artifacts from builder
COPY --from=builder /opt/openssl /opt/openssl
COPY --from=builder /usr/local /usr/local
COPY --from=builder /app/server/pqfl_server /app/pqfl_server
COPY --from=builder /app/server/pqfl_test_client /app/pqfl_test_client
COPY --from=builder /app/server/pqfl_self_test /app/pqfl_self_test
COPY --from=builder /app/server/pqfl_admin_demo /app/pqfl_admin_demo
COPY --from=builder /app/server/pqfl_admin_cli /app/pqfl_admin_cli
COPY --from=builder /app/server/pqfl_worker_demo /app/pqfl_worker_demo
COPY ./scripts /app/scripts
COPY ./config.json /app/config.json

# Update dynamic linker cache
RUN ldconfig

# Runtime environment
ENV PATH="/opt/openssl/bin:${PATH}"
ENV LD_LIBRARY_PATH="/opt/openssl/lib:/usr/local/lib"

WORKDIR /app
RUN chmod +x /app/scripts/*.sh
RUN mkdir -p /app/data /app/models

EXPOSE 50051
CMD ["/app/pqfl_server"]
