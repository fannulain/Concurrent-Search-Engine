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

# ---------- Test stage ----------
FROM build AS test

RUN apt-get update \
    && apt-get install -y --no-install-recommends libgtest-dev clang-format clang \
    && rm -rf /var/lib/apt/lists/*

COPY .clang-format .
COPY tests ./tests

RUN clang-format --dry-run --Werror \
        src/Source.cpp \
        src/*/*.h src/*/*.cpp \
        tests/*.cpp \
    && cmake -S . -B build-test -G Ninja -DCMAKE_BUILD_TYPE=Release -DCSE_BUILD_TESTS=ON \
    && cmake --build build-test --parallel \
    && ctest --test-dir build-test --output-on-failure \
    && cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCSE_BUILD_TESTS=ON -DCSE_ENABLE_TSAN=ON \
    && cmake --build build-tsan --parallel \
    && ctest --test-dir build-tsan --output-on-failure

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
