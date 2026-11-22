#!/usr/bin/env bash
# Starts redis + livekit-server + ingress on localhost and writes .run/env with LIVEKIT_* for the tools.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p .run && chmod 700 .run
if [[ ! -f .run/env ]]; then
  key="EME$(openssl rand -hex 6)"; secret="$(openssl rand -hex 32)"
  printf 'LIVEKIT_URL=ws://127.0.0.1:7880\nLIVEKIT_API_KEY=%s\nLIVEKIT_API_SECRET=%s\n' "$key" "$secret" > .run/env
  chmod 600 .run/env
fi
source .run/env
cat > .run/livekit.yaml <<YAML
port: 7880
bind_addresses: ["127.0.0.1"]
rtc:
  tcp_port: 7881
  port_range_start: 50000
  port_range_end: 50200
  use_external_ip: false
  node_ip: 127.0.0.1
redis:
  address: 127.0.0.1:6379
keys:
  ${LIVEKIT_API_KEY}: ${LIVEKIT_API_SECRET}
ingress:
  whip_base_url: http://127.0.0.1:8085/w
logging:
  level: info
YAML
cat > .run/ingress.yaml <<YAML
api_key: ${LIVEKIT_API_KEY}
api_secret: ${LIVEKIT_API_SECRET}
ws_url: ws://127.0.0.1:7880
redis:
  address: 127.0.0.1:6379
rtmp_port: -1
whip_port: 8085
http_relay_port: 9091
health_port: 9092
rtc_config:
  udp_port: 7885
  use_external_ip: false
  node_ip: 127.0.0.1
cpu_cost:
  rtmp_cpu_cost: 2
  whip_cpu_cost: 2
  whip_bypass_transcoding_cpu_cost: 0.1
logging:
  level: info
YAML
chmod 644 .run/*.yaml  # containers run as non-root
docker compose up -d --force-recreate
ready=0
for i in $(seq 1 30); do curl -sf http://127.0.0.1:7880 >/dev/null && curl -sf http://127.0.0.1:9092 >/dev/null && { ready=1; break; }; sleep 1; done
[[ $ready == 1 ]] || { echo "stack did not become ready; see: docker compose logs" >&2; exit 1; }

# Redis is not persistent: (re)create the WHIP ingress (no transcoding) and store its endpoint + stream key.
LK="${LK:-$HOME/deps/lk/lk}"
export LIVEKIT_URL LIVEKIT_API_KEY LIVEKIT_API_SECRET
cat > .run/ingress-req.json <<JSON
{ "input_type": 1, "name": "eme-whip", "room_name": "eme-spike", "participant_identity": "eme-whip-publisher",
  "participant_name": "EME WHIP", "enable_transcoding": false }
JSON
"$LK" ingress create .run/ingress-req.json >/dev/null
"$LK" ingress list --json | python3 -c '
import json, sys
d = json.load(sys.stdin); items = d.get("items", d) if isinstance(d, dict) else d
i = [x for x in items if x.get("name") == "eme-whip"][-1]
print("WHIP_URL=%s\nWHIP_TOKEN=%s" % (i["url"], i.get("stream_key") or i.get("streamKey")))' > .run/whip.env
chmod 600 .run/whip.env
echo "livekit + ingress ready (WHIP endpoint in .run/whip.env)"
