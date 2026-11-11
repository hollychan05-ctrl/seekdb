# MiniVectorDB - 2026 OceanBase database competition, round 2 (task 600081).
# Builds the database from source and starts it in stdin/stdout JSONL mode.
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make ca-certificates \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY minivectordb/ /src/minivectordb/
RUN cmake -S /src/minivectordb -B /src/build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /src/build -j"$(nproc)"

FROM ubuntu:24.04

WORKDIR /app
COPY --from=build /src/build/minivectordb /app/minivectordb
RUN chmod 0755 /app/minivectordb \
 && mkdir -p /data

VOLUME ["/data"]
ENTRYPOINT ["/app/minivectordb"]
CMD ["--data-dir", "/data"]
