"""Read the instance UUID and verify a real seekdb SQL connection."""

import argparse
import hashlib
import json
import os
from pathlib import Path
from datetime import datetime, timezone
import uuid

import pymysql


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--base-dir")
    source.add_argument("--telemetry-file", type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2881)
    parser.add_argument("--user", default="root")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    telemetry_path = args.telemetry_file or Path(args.base_dir) / "run" / "telemetry.json"
    raw = telemetry_path.read_bytes()
    telemetry = json.loads(raw.decode("utf-8-sig"))
    deployment_uuid = telemetry["content"]["id"]
    if str(uuid.UUID(deployment_uuid)) != deployment_uuid.lower() or len(deployment_uuid) != 36:
        raise ValueError("content.id must be a complete, hyphenated deployment UUID")

    with pymysql.connect(
        host=args.host,
        port=args.port,
        user=args.user,
        password=os.environ.get("SEEKDB_PASSWORD", ""),
        database="test",
        connect_timeout=10,
        read_timeout=20,
        cursorclass=pymysql.cursors.DictCursor,
    ) as connection:
        with connection.cursor() as cursor:
            sql = "SELECT 1 AS probe, VERSION() AS version, CURRENT_USER() AS current_user_name, DATABASE() AS database_name"
            cursor.execute(sql)
            row = cursor.fetchone()
            if row["probe"] != 1:
                raise RuntimeError("SQL verification did not return the expected result")

    result = {
        "verified_at_utc": datetime.now(timezone.utc).isoformat(),
        "deployment_uuid": deployment_uuid,
        "telemetry_file": str(telemetry_path.resolve()),
        "telemetry_file_sha256": hashlib.sha256(raw).hexdigest(),
        "instance_recorded_sent": telemetry.get("sent"),
        "endpoint": f"{args.host}:{args.port}",
        "sql": sql,
        "result": row,
        "sql_verified": True,
    }
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
