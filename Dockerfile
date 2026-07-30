FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake g++ \
    libssl-dev libmysqlclient-dev libcurl4-openssl-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src

# Build redis++ from bundled source
WORKDIR /src/third_party/redis-plus-plus
RUN mkdir -p build && cd build && \
    cmake .. -DREDIS_PLUS_PLUS_BUILD_TEST=OFF -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) && make install && ldconfig

# Build main project
WORKDIR /src/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local && make -j$(nproc)

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libmysqlclient21 libcurl4 libssl3 \
    libhiredis1.1.0 \
    curl \
    ca-certificates && rm -rf /var/lib/apt/lists/*

# Copy redis++ libraries from builder
COPY --from=builder /usr/local/lib/libredis++.so.1.3.11 /usr/local/lib/libredis++.so.1
RUN ln -sf /usr/local/lib/libredis++.so.1 /usr/local/lib/libredis++.so && ldconfig

COPY --from=builder /src/build/SuretyService /app/SuretyService
COPY --from=builder /src/assets/ /app/assets/
COPY --from=builder /src/version.json /src/announcement.json /app/
RUN mkdir -p /app/html

WORKDIR /app
EXPOSE 8920
CMD ["./SuretyService"]
