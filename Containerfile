# Kodi build image for Switch 2: ounce-devkit plus the host tools Kodi's build runs natively
# (TexturePacker, JsonSchemaBuilder, flatc, autotools for internal deps).
FROM localhost/ounce-devkit:latest
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      autoconf automake libtool libtool-bin gettext autopoint gperf nasm patch xz-utils bzip2 unzip curl ca-certificates \
      build-essential pkg-config \
      liblzo2-dev libpng-dev libgif-dev libjpeg-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*
