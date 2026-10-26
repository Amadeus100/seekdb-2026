#!/usr/bin/env bash
# Deploy an official image, verify SQL, and preserve its own telemetry UUID.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
EVIDENCE_DIR="${EVIDENCE_DIR:-$SCRIPT_DIR/evidence}"
SEEKDB_IMAGE="oceanbase/seekdb@sha256:4db4961869d554313520d6081fca29265ba6da6db7313311bd8acbed771a956b"
CONTAINER_NAME="contest-seekdb-task1-${GITHUB_RUN_ID:-$$}"
SEEKDB_PORT="${SEEKDB_PORT:-2881}"
mkdir -p "$EVIDENCE_DIR"

cleanup() {
  docker logs "$CONTAINER_NAME" > "$EVIDENCE_DIR/container.log" 2>&1 || true
  docker stop --time 30 "$CONTAINER_NAME" >/dev/null 2>&1 || true
  docker rm "$CONTAINER_NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker pull "$SEEKDB_IMAGE"
docker run -d --name "$CONTAINER_NAME" \
  -p "127.0.0.1:$SEEKDB_PORT:2881" \
  -e CPU_COUNT=2 -e MEMORY_LIMIT=2G \
  -e LOG_DISK_SIZE=4G -e DATAFILE_SIZE=1G -e DATAFILE_MAXSIZE=4G \
  "$SEEKDB_IMAGE"

# SQL must work before the telemetry file is considered a deployment result.
python3 - "$SEEKDB_PORT" <<'PY'
import sys, time
import pymysql

last_error = None
for attempt in range(90):
    try:
        with pymysql.connect(host="127.0.0.1", port=int(sys.argv[1]), user="root",
                             connect_timeout=3, read_timeout=5) as connection:
            with connection.cursor() as cursor:
                cursor.execute("SELECT 1")
                assert cursor.fetchone() == (1,)
        print("seekdb SQL ready")
        break
    except Exception as error:
        last_error = error
        time.sleep(2)
else:
    raise SystemExit(f"seekdb did not become ready: {last_error}")
PY

docker cp "$CONTAINER_NAME:/var/lib/seekdb/run/telemetry.json" "$EVIDENCE_DIR/telemetry.generated.json"
python3 "$SCRIPT_DIR/verify.py" \
  --telemetry-file "$EVIDENCE_DIR/telemetry.generated.json" \
  --port "$SEEKDB_PORT" --output "$EVIDENCE_DIR/sql-validation.json"

# The server has a short reporting timeout. Retry its exact generated payload
# against the same official endpoint and record the endpoint's actual response.
python3 - "$EVIDENCE_DIR" "$SEEKDB_IMAGE" <<'PY'
from datetime import datetime, timezone
import hashlib, json, pathlib, sys, time, urllib.request

out = pathlib.Path(sys.argv[1])
state = json.loads((out / "telemetry.generated.json").read_text())
deployment_uuid = state["content"]["id"]
payload = json.dumps({"content": state["content"], "component": state["component"]},
                     separators=(",", ":")).encode()
endpoint = "https://openwebapi.oceanbase.com/api/web/oceanbase/report"
for attempt in range(4):
    try:
        request = urllib.request.Request(endpoint, data=payload,
                                         headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(request, timeout=30) as response:
            status = response.status
            body = response.read().decode()
        break
    except Exception:
        if attempt == 3:
            raise
        time.sleep(3)
body_json = json.loads(body)
if not 200 <= status < 300:
    raise RuntimeError(f"Telemetry HTTP failure: {status}")
# Reject an explicit application-level failure even when the HTTP status is 200.
if body_json.get("success") is False or body_json.get("successful") is False:
    raise RuntimeError(f"Telemetry rejected: {body}")
if "code" in body_json and str(body_json["code"]) not in ("0", "200", "OK", "SUCCESS"):
    raise RuntimeError(f"Unexpected telemetry response code: {body}")
record = {
    "reported_at_utc": datetime.now(timezone.utc).isoformat(),
    "deployment_uuid": deployment_uuid,
    "image": sys.argv[2],
    "endpoint": endpoint,
    "method": "POST of the real instance-generated content and component, without changing their values",
    "payload_sha256": hashlib.sha256(payload).hexdigest(),
    "http_status": status,
    "response": body_json,
    "competition_claim_verified": False,
}
(out / "telemetry-report.json").write_text(json.dumps(record, indent=2) + "\n")
(out / "uuid.txt").write_text(deployment_uuid + "\n")
print("Deployment UUID:", deployment_uuid)
print("Telemetry endpoint response:", body)
PY

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  {
    echo '赛题一部署 UUID：'
    cat "$EVIDENCE_DIR/uuid.txt"
    echo ''
    echo 'SQL 已验证，遥测接收响应见 telemetry-report.json。'
    echo '最终认领及得分需在大赛平台提测确认。'
  } >> "$GITHUB_STEP_SUMMARY"
fi
