# v0.1 原型的镜像。在仓库根目录构建：docker build -f prototype/Dockerfile -t llmgw-prototype .
# ---- 构建阶段 ----------------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target llmgw prototype_tests \
    && ./build/prototype/prototype_tests

# ---- 运行阶段 ----------------------------------------------------------------
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home llmgw
COPY --from=build /src/build/prototype/llmgw /usr/local/bin/llmgw
USER llmgw
EXPOSE 8080
ENTRYPOINT ["llmgw"]
CMD ["--config", "/etc/llmgw/config.json"]
