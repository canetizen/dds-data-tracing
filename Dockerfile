# === STAGE 1: Generate C code from IDL ===
FROM ubuntu:22.04 AS idlgen

RUN apt-get update && apt-get install -y \
    cyclonedds-tools \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /gen

COPY shared/CombatMessages.idl .

RUN idlc -l c CombatMessages.idl

# === STAGE 2: Build OpenTelemetry SDK ===
FROM ubuntu:22.04 AS otel-builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    libcurl4-openssl-dev \
    libprotobuf-dev \
    protobuf-compiler \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /deps

# OpenTelemetry C++ SDK
RUN git clone --depth 1 --branch v1.12.0 https://github.com/open-telemetry/opentelemetry-cpp.git && \
    cd opentelemetry-cpp && \
    mkdir build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTING=OFF \
        -DWITH_OTLP_HTTP=ON \
        -DWITH_OTLP_GRPC=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/otel \
        -DBUILD_SHARED_LIBS=ON && \
    make -j$(nproc) && \
    make install

# === STAGE 3: Build Application ===
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    cyclonedds-dev \
    libcurl4-openssl-dev \
    libprotobuf-dev \
    nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

# Copy OTel SDK
COPY --from=otel-builder /opt/otel /opt/otel

ARG SERVICE_NAME

WORKDIR /app

# Copy IDL generated files
COPY --from=idlgen /gen/CombatMessages.c ./generated/
COPY --from=idlgen /gen/CombatMessages.h ./generated/

# Copy service source code
COPY services/${SERVICE_NAME}/main.cpp .
COPY services/${SERVICE_NAME}/CMakeLists.txt .

# Build
ENV CMAKE_PREFIX_PATH=/opt/otel
RUN cmake . && make

# === STAGE 4: Runtime ===
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    cyclonedds-dev \
    libcurl4 \
    libprotobuf23 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=otel-builder /opt/otel/lib /opt/otel/lib
COPY --from=builder /app/app .
COPY shared/cyclonedds.xml /shared/cyclonedds.xml

ENV LD_LIBRARY_PATH=/opt/otel/lib
ENV CYCLONEDDS_URI=file:///shared/cyclonedds.xml

CMD ["./app"]
