# syntax=docker/dockerfile:1

# ---------- Build stage ----------
FROM ubuntu:24.04 AS build

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        g++ cmake ninja-build libboost-dev nlohmann-json3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt .
COPY src ./src

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --parallel

# ---------- Runtime stage ----------
FROM ubuntu:24.04 AS runtime

RUN apt-get update \
    && apt-get install -y --no-install-recommends curl ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home app

COPY --from=build /src/build/search_server /usr/local/bin/search_server

USER app
EXPOSE 8080
ENTRYPOINT ["/usr/local/bin/search_server"]
