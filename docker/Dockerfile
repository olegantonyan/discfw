FROM debian:bookworm-slim
ARG UID=1000
ARG GID=1000
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends bc build-essential bzip2 ca-certificates cpio curl file git libncurses-dev patch perl python3 rsync unzip wget xz-utils && rm -rf /var/lib/apt/lists/*
RUN groupadd -o -g "$GID" build && useradd -o -m -u "$UID" -g "$GID" build
ENV LC_ALL=C.UTF-8
USER build
