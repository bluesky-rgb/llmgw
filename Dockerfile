# llmgw 的镜像（新架构）。在仓库根目录构建：docker build -t llmgw .
# 原型（性能对照组）的镜像见 prototype/Dockerfile。
# ---- 构建阶段 ----------------------------------------------------------------
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git ca-certificates libssl-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DLLMGW_BUILD_TESTS=OFF -DLLMGW_BUILD_PROTOTYPE=OFF \
    && cmake --build build --target llmgw_app \
    && ./build/src/app/llmgw check -c config/llmgw.example.json --skip-env

# ---- 运行阶段 ----------------------------------------------------------------
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
      libssl3 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home llmgw
COPY --from=build /src/build/src/app/llmgw /usr/local/bin/llmgw
COPY config/llmgw.example.json /etc/llmgw/llmgw.json
USER llmgw
# 数据面、观测面（/health、/ready）
EXPOSE 8080 9100
ENTRYPOINT ["llmgw"]
# 任务 2.2 之前网关不做客户端鉴权：容器中监听 0.0.0.0，所以默认会拒绝启动并说明原因。
# 确认只有可信的客户端能访问时再加 --allow-unauthenticated（见 docs/usage.md）
CMD ["serve", "-c", "/etc/llmgw/llmgw.json"]
