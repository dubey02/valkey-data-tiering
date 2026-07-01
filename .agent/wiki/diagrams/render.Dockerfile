# Reproducible mermaid (.mmd -> .png) renderer.
#
# This host is aarch64 + glibc 2.26 (Amazon Linux 2): Chrome-for-Testing has no
# Linux-arm64 build and modern chromium needs glibc >= 2.28, so mermaid-cli cannot
# render natively here. This image carries Debian's arm64 chromium + its own libc.
#
# Build:  docker build -f render.Dockerfile -t mermaid-render:local .
# Render: docker run --rm -u $(id -u):$(id -g) -e HOME=/tmp -v "$PWD":/data \
#           mermaid-render:local -i spill-sequence.mmd -o spill-sequence.png -p puppeteer-config.json
FROM node:22-slim

ENV PUPPETEER_SKIP_DOWNLOAD=true \
    PUPPETEER_EXECUTABLE_PATH=/usr/bin/chromium

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      chromium \
      fonts-liberation \
      fonts-dejavu-core \
 && rm -rf /var/lib/apt/lists/*

RUN npm install -g @mermaid-js/mermaid-cli@11.4.2

WORKDIR /data
ENTRYPOINT ["mmdc"]
