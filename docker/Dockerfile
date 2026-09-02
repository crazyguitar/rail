FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      cmake \
      git \
      libfuse3-dev \
      libibverbs-dev \
      librdmacm-dev \
      liburing-dev \
      ninja-build \
      pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DRAIL_BUILD_TESTS=OFF \
      -DRAIL_BUILD_BENCH=OFF \
    && cmake --build build --target raild-cli --parallel

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
      ibverbs-providers \
      libfuse3-3 \
      libibverbs1 \
      liburing2 \
      librdmacm1t64 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/tools/raild/raild /usr/local/bin/raild

EXPOSE 18600
ENTRYPOINT ["/usr/local/bin/raild"]
CMD ["--serve", "/models"]
