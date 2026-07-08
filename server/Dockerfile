# Multi-stage build for zzcaster-relay.
#
# Build:
#   docker build -t zzcaster-relay ./server
#
# Run (exposes TCP 3939 for signaling, UDP 3939 for UdpData + STUN):
#   docker run -d --name zzcaster-relay \
#     -p 3939:3939/tcp \
#     -p 3939:3939/udp \
#     --restart unless-stopped \
#     zzcaster-relay
#
# Override defaults via env vars:
#   -e ZZ_RELAY_ADDR=:3939
#   -e ZZ_RELAY_TTL=60s
#   -e ZZ_LOG_LEVEL=info

# Stage 1: Build
FROM golang:1.22-alpine AS builder
WORKDIR /app

# Cache module downloads — go.mod is small and rarely changes.
COPY go.mod ./
# (no go.sum yet — single-module project with no external deps)

# Copy source
COPY . .

# Build static binary, strip debug info to minimize image size.
# CGO_ENABLED=0 → fully static, no libc dependency, runs in scratch.
RUN CGO_ENABLED=0 go build -ldflags="-s -w" -o /zzcaster-relay .

# Stage 2: Runtime — scratch base, ~10MB image
FROM scratch
COPY --from=builder /zzcaster-relay /zzcaster-relay

# Expose both TCP and UDP on port 3939.
# (Docker requires explicit /tcp or /udp suffix — these are documentation
#  only, the actual port mapping is set via `docker run -p`.)
EXPOSE 3939/tcp
EXPOSE 3939/udp

ENTRYPOINT ["/zzcaster-relay"]
