# ---- 构建阶段 ----------------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY tests ./tests
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build \
    && ./build/llmgw_tests

# ---- 运行阶段 ----------------------------------------------------------------
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home llmgw
COPY --from=build /src/build/llmgw /usr/local/bin/llmgw
USER llmgw
EXPOSE 8080
ENTRYPOINT ["llmgw"]
CMD ["--config", "/etc/llmgw/config.json"]
