# Kodi build image for Switch 2: ounce-devkit plus the host tools Kodi's build runs natively
# (TexturePacker, JsonSchemaBuilder, flatc, autotools for internal deps), plus SWIG and a Java runtime for
# the Python add-on bindings (xbmc/interfaces/swig: swig -xml, then tools/codegenerator under groovy),
# and meson for dav1d.
FROM localhost/ounce-devkit:latest
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      autoconf automake libtool libtool-bin gettext autopoint gperf nasm patch xz-utils bzip2 unzip curl ca-certificates \
      build-essential pkg-config \
      liblzo2-dev libpng-dev libgif-dev libjpeg-dev zlib1g-dev \
      swig default-jre-headless meson \
      libffi-dev libssl-dev libbz2-dev liblzma-dev libsqlite3-dev libexpat1-dev \
    && rm -rf /var/lib/apt/lists/*
