# syntax=docker/dockerfile:1.7
# Copyright (C) 2026 Donnie V. Savage
# Alpine source build of FRR + OpenEIGRP. GitHub or local sources are
# staged by tools/docker.sh; both follow the same patch/install/build path.
ARG ALPINE_VERSION=3.22
FROM alpine:${ALPINE_VERSION} AS builder

RUN apk add --no-cache \
      autoconf automake bash bison build-base ca-certificates c-ares-dev \
      flex gawk git json-c-dev libcap-dev libedit-dev libtool \
      libyang-dev linux-headers make ncurses-dev openssl-dev \
      pcre2-dev pkgconf protobuf-c-compiler protobuf-c-dev \
      python3 rsync

# FRR's Clippy requires gelf.h/libelf and Python development headers.
# Keep this in a separate layer so existing builder dependencies remain
# cached when correcting a dependency on slow connections.
RUN apk add --no-cache elfutils-dev python3-dev

# vtysh links GNU Readline (libreadline / readline/history.h); libedit
# and ncurses alone do not satisfy FRR configure's Readline checks.
# Keep this late so the existing toolchain/Clippy layers remain cached.
RUN apk add --no-cache readline-dev

ARG EIGRP_SHA
ARG FRR_SHA
ARG EIGRP_VERSION
ARG BUILD_JOBS=4

# Both modes receive immutable, staged source contexts from tools/docker.sh.
# Local contexts include the developer's working edits and a Git checkout for
# EIGRP's release injector; remote contexts are fetched from both GitHub repos.
COPY --from=eigrp-source / /src-eigrp/
COPY --from=frr-source / /src-frr/
ARG SOURCE_MODE=build
RUN set -eu; \
    test -f /src-eigrp/tools/frr.sh; \
    test -f /src-frr/bootstrap.sh; \
    test "$(git -C /src-eigrp rev-parse HEAD)" = "$EIGRP_SHA"; \
    if [ "$SOURCE_MODE" = build ]; then \
      test "$(git -C /src-frr rev-parse HEAD)" = "$FRR_SHA"; \
    else \
      test "$SOURCE_MODE" = local; \
    fi

# Deliberately match local development's three separate stages. FRR-wide
# edits remain in frr/patch/ and fail rather than accepting fuzz or drift.
RUN set -eux; \
    cd /src-eigrp; \
    bash tools/frr.sh --patch --frr-root /src-frr; \
    bash tools/frr.sh --install --frr-root /src-frr; \
    EIGRP_BUILD_VERSION="$EIGRP_VERSION" bash tools/frr.sh --configure \
      --frr-root /src-frr -- \
      --prefix=/usr --libdir=/usr/lib --enable-vtysh --enable-pcre2posix \
      --disable-doc --disable-doc-html; \
    EIGRP_BUILD_VERSION="$EIGRP_VERSION" bash tools/frr.sh --build \
      --frr-root /src-frr --jobs "$BUILD_JOBS"; \
    make -C /src-frr DESTDIR=/image install; \
    install -d /image/etc/frr; \
    cp /src-frr/tools/etc/frr/daemons /image/etc/frr/daemons

FROM alpine:${ALPINE_VERSION} AS runtime

# Runtime libraries match Alpine 3.22's libyang 2 ABI, not FRR's compiled APK.
RUN apk add --no-cache \
      bash ca-certificates c-ares iproute2 json-c libcap libedit \
      libyang libstdc++ ncurses-libs ncurses-terminfo-base openssl pcre2 protobuf-c \
      python3 readline tini \
    && addgroup -S frr \
    && addgroup -S frrvty \
    && adduser -S -D -H -G frr frr \
    && addgroup frr frrvty

COPY --from=builder /image/ /
COPY docker/daemons /etc/frr/daemons
COPY docker/frr.conf /etc/frr/frr.conf
COPY docker/docker-start /usr/lib/frr/docker-start
# Template lives outside /etc/frr so existing named volumes can receive it too.
COPY docker/vtysh.conf /usr/share/frr/docker-vtysh.conf

ARG EIGRP_VERSION
ARG EIGRP_SHA
ARG FRR_SHA
LABEL org.opencontainers.image.title="OpenEIGRP with FRRouting" \
      org.opencontainers.image.source="https://github.com/diivious/eigrp" \
      org.opencontainers.image.version="${EIGRP_VERSION}" \
      org.opencontainers.image.revision="${EIGRP_SHA}" \
      org.openeigrp.frr.revision="${FRR_SHA}" \
      org.openeigrp.frr.release="${EIGRP_VERSION}"

RUN install -d -o frr -g frr -m 0775 /run/frr /var/log/frr \
    && chown -R frr:frrvty /etc/frr \
    && chmod 0640 /etc/frr/frr.conf /etc/frr/daemons \
    && chmod 0755 /usr/lib/frr/docker-start \
    && test -x /usr/lib/frr/eigrpd \
    && test -x /usr/lib/frr/zebra \
    && test -x /usr/lib/frr/watchfrr \
    && test -x /usr/bin/vtysh

VOLUME ["/etc/frr"]
STOPSIGNAL SIGTERM
ENTRYPOINT ["/sbin/tini", "--"]
CMD ["/usr/lib/frr/docker-start"]
