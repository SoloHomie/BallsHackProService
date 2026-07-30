FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y \
    build-essential cmake g++ \
    libssl-dev libmysqlclient-dev libcurl4-openssl-dev \
    libhiredis-dev libredis++-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src/build
RUN cmake .. -DCMAKE_BUILD_TYPE=Release && make -j$(nproc)

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libmysqlclient21 libcurl4 libssl3 \
    libhiredis1.1.0 libredis++1 \
    ca-certificates && rm -rf /var/lib/apt/lists/*

COPY --from=builder /src/build/SuretyService /app/SuretyService
COPY --from=builder /src/assets/ /app/assets/
COPY --from=builder /src/version.json /src/announcement.json /app/
RUN mkdir -p /app/html

WORKDIR /app
EXPOSE 8920
CMD ["./SuretyService"]
