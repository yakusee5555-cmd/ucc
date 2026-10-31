# UCC — C compiler + Web IDE, single container.
# Builds the native `ucc` compiler binary, installs the web IDE deps,
# and serves the IDE on port 3000.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc \
        make \
        nodejs \
        npm \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY . /app

# Build the compiler (C11, warnings as hygiene) and install web deps.
RUN gcc -std=c11 -O2 -Wall -Wextra -Isrc src/*.c -o build/ucc \
    && npm install --prefix web --omit=dev

EXPOSE 3000

# UCC_BIN defaults to /app/build/ucc (see web/server.js).
CMD ["node", "web/server.js"]
