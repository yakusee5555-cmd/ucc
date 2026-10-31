#!/usr/bin/env bash
# UCC Web IDE — one-command deploy on any Linux VPS with docker.
# Usage on the VPS:  ./deploy-vps.sh        (or UCC_PORT=8080 ./deploy-vps.sh)
set -euo pipefail
cd "$(dirname "$0")"

PORT="${UCC_PORT:-3001}"        # port 3000 is usually taken by other apps
NAME="${UCC_NAME:-ucc-ide}"

command -v docker >/dev/null 2>&1 || {
  echo "ERROR: docker is not installed on this machine."
  echo "Install it first: https://docs.docker.com/engine/install/"
  exit 1
}

echo "[1/4] Building docker image (a few minutes on first run)..."
docker build -t ucc-ide:latest .

if docker ps -a --format '{{.Names}}' | grep -q "^${NAME}$"; then
  echo "[2/4] Removing old container..."
  docker rm -f "$NAME" >/dev/null
else
  echo "[2/4] No old container to remove."
fi

echo "[3/4] Starting container on port ${PORT}..."
docker run -d --name "$NAME" --restart unless-stopped -p "${PORT}:3000" ucc-ide:latest >/dev/null

if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q "Status: active"; then
  echo "      (opening firewall port ${PORT})"
  (sudo -n ufw allow "${PORT}/tcp" >/dev/null 2>&1 || ufw allow "${PORT}/tcp" >/dev/null 2>&1) || true
fi

echo "[4/4] Waiting for the IDE to come up..."
for _ in $(seq 1 30); do
  if curl -sf "http://localhost:${PORT}/api/health" >/dev/null 2>&1; then
    echo ""
    echo "OK — the UCC Web IDE is live."
    PUBLIC_IP="$(curl -s --max-time 5 https://api.ipify.org 2>/dev/null || echo '<this-servers-ip>')"
    echo "Open it at:  http://${PUBLIC_IP}:${PORT}"
    echo "Redeploy later with:  git pull && ./deploy-vps.sh"
    exit 0
  fi
  sleep 2
done

echo ""
echo "Container started but the health check failed. Last 30 log lines:"
docker logs --tail 30 "$NAME"
exit 1
