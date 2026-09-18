#!/usr/bin/env bash
# Rewrite <Program scheduled="..."> in _filler.sch to ~30s ago so join offset
# is near the start of the current item (needed for short media).
set -euo pipefail
ROOT="$(CDPATH= cd -- "$(dirname "$0")" && pwd)"
SCH="${ROOT}/schedules/_filler.sch"
# ISO-8601 with ms + local offset, 30s in the past
STAMP="$(date -d '30 seconds ago' '+%Y-%m-%dT%H:%M:%S.000%z' | sed -E 's/([+-][0-9]{2})([0-9]{2})$/\1:\2/')"
tmp="$(mktemp)"
sed -E "s/scheduled=\"[^\"]+\"/scheduled=\"${STAMP}\"/" "${SCH}" > "${tmp}"
mv "${tmp}" "${SCH}"
echo "Stamped schedule: scheduled=\"${STAMP}\""
