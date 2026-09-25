# syntax=docker/dockerfile:1.7-labs
# Melee Tactics on the web: the browser build and the server that hosts it
# and pairs players over WebRTC (server/). One image, run the same way locally
# and on fly.io:
#
#   docker build -t melee-tactics .
#   docker run --rm -p 8080:8080 melee-tactics      # http://localhost:8080
#
# No game data is in the image: players load their own disc in the page.
# The wasm stage mirrors .github/workflows/pages.yml (ubuntu-24.04, LLVM 22's
# LibTooling for disc_lower, GCC 14 as its scalar_storage_order oracle).

FROM ubuntu:24.04 AS wasm
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gcc-14 g++-14 cmake ninja-build python3 git curl wget \
        ca-certificates xz-utils lsb-release gnupg software-properties-common \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-14 100 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-14 100 \
    && wget -q https://apt.llvm.org/llvm.sh && bash llvm.sh 22 \
    && apt-get install -y --no-install-recommends libclang-22-dev libclang-cpp22-dev llvm-22-dev \
    && rm -rf /var/lib/apt/lists/* llvm.sh
ENV LLVM_ROOT=/usr/lib/llvm-22
WORKDIR /src
# The pinned Emscripten SDK in a layer of its own: source changes do not
# download it again.
COPY tools/browser/common.py tools/browser/setup_sdk.py tools/browser/
RUN python3 tools/browser/setup_sdk.py
# The page shell and the server are added in the last stage, so editing them
# does not recompile the game.
COPY --exclude=server --exclude=platforms/browser/index.html \
     --exclude=platforms/browser/shell.mjs --exclude=platforms/browser/link.mjs \
     --exclude=platforms/browser/touch.mjs \
     --exclude=platforms/browser/manifest.webmanifest . .
RUN python3 tools/browser/build.py --jobs "$(nproc)" \
    && mkdir /web \
    && cp build/browser/runtime/platforms/browser/*.js \
          build/browser/runtime/platforms/browser/*.mjs \
          build/browser/runtime/platforms/browser/*.wasm /web/

FROM golang:1-bookworm AS server
WORKDIR /server
COPY server/go.mod server/go.sum ./
RUN go mod download
COPY server/ ./
RUN go test ./... && CGO_ENABLED=0 go build -trimpath -ldflags="-s -w" -o /melee-tactics-server .

FROM gcr.io/distroless/static-debian12
COPY --from=server /melee-tactics-server /app/server
COPY --from=wasm /web /app/web
COPY platforms/browser/index.html platforms/browser/shell.mjs platforms/browser/link.mjs \
     platforms/browser/touch.mjs platforms/browser/manifest.webmanifest /app/web/
ENV WEB_DIR=/app/web PORT=8080
EXPOSE 8080
USER nonroot
ENTRYPOINT ["/app/server"]
