FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake g++ git \
    libssl-dev libmysqlclient-dev libcurl4-openssl-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

# Build redis++ from source (not in Ubuntu 24.04 repos)
RUN cd /tmp && \
    git clone --depth 1 --branch 1.3 https://github.com/sewenew/redis-plus-plus.git && \
    cd redis-plus-plus && mkdir build && cd build && \
    cmake .. -DREDIS_PLUS_PLUS_BUILD_TEST=OFF -DCMAKE_BUILD_TYPE=Release && \
    make -j$(nproc) && make install && ldconfig && \
    rm -rf /tmp/redis-plus-plus

COPY . /src
WORKDIR /src/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/usr/local && make -j$(nproc)

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libmysqlclient21 libcurl4 libssl3 \
    libhiredis1.1.0 \
    ca-certificates && rm -rf /var/lib/apt/lists/*

# Copy redis++ shared library from builder
COPY --from=builder /usr/local/lib/libredis++.so* /usr/local/lib/
RUN ldconfig

COPY --from=builder /src/build/SuretyService /app/SuretyService
COPY --from=builder /src/assets/ /app/assets/
COPY --from=builder /src/version.json /src/announcement.json /app/
RUN mkdir -p /app/html

WORKDIR /app
EXPOSE 8920
CMD ["./SuretyService"]
