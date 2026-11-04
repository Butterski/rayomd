# SPDX-License-Identifier: Apache-2.0
#
# ghcr.io/butterski/rayomd: the static release binary (-DRAYOMD_STATIC=ON, static-pie) on scratch.
# The build context is the extracted rayomd-<VERSION>-linux-x64-static folder of a release (rayomd,
# LICENSE, NOTICE); the release workflow builds it from that artifact.
#
#   docker run --rm -v "$PWD:/w" ghcr.io/butterski/rayomd --export notes.md notes.pdf

# The parts of a root file system that rayomd needs and scratch lacks:
# - DejaVu Sans (about 760 KB of the image's 3.0 MB), the first font of rayomd's Linux search list:
#   without a TrueType font only WinAnsi text renders, other characters are counted as missing and
#   --doctor fails. Its license (Bitstream Vera and Arev, public-domain changes) allows
#   redistribution; its copyright file goes with it. Other scripts (CJK) need RAYOMD_FONT or
#   RAYOMD_FALLBACK_FONT pointing at a mounted font.
# - /tmp (mode 1777), where --doctor writes its test PDF.
# - /w, the working folder, owned by the image's user so that it is writable without a mount.
# There is no passwd entry, so Docker sets HOME=/: the image cache (~/.cache/rayomd) cannot be
# written and stays off; set XDG_CACHE_HOME to a mounted folder to keep re-encoded PNGs.
FROM debian:bookworm-slim AS rootfs
RUN apt-get update \
    && apt-get install -y --no-install-recommends fonts-dejavu-core \
    && install -D -m 644 /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
        /rootfs/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
    && install -D -m 644 /usr/share/doc/fonts-dejavu-core/copyright \
        /rootfs/usr/share/doc/fonts-dejavu-core/copyright \
    && install -d -m 1777 /rootfs/tmp \
    && install -d -m 755 -o 65532 -g 65532 /rootfs/w

FROM scratch
ARG VERSION=unknown
LABEL org.opencontainers.image.title="rayomd" \
      org.opencontainers.image.description="Markdown to PDF from a single static executable: no browser, no LaTeX, no runtime" \
      org.opencontainers.image.source="https://github.com/Butterski/rayomd" \
      org.opencontainers.image.licenses="Apache-2.0" \
      org.opencontainers.image.version="${VERSION}"
COPY --from=rootfs /rootfs/ /
COPY LICENSE NOTICE /usr/share/doc/rayomd/
COPY --chmod=755 rayomd /rayomd
USER 65532:65532
WORKDIR /w
ENTRYPOINT ["/rayomd"]
