#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ccs_root="${CCS_ROOT:-/Applications/ti/ccs2100}"
dss="${ccs_root}/ccs/ccs_base/scripting/bin/dss.sh"
jre="${ccs_root}/ccs/ccs-server.app/jre/Contents/Home"

if [[ ! -x "${dss}" ]]; then
    echo "DSS runner not found: ${dss}" >&2
    exit 2
fi

if [[ ! -x "${jre}/bin/java" ]]; then
    echo "CCS bundled JRE not found: ${jre}/bin/java" >&2
    exit 2
fi

export TI_APPDATA_DIR="${TI_APPDATA_DIR:-/tmp/v2k-ti-appdata}"
export JAVA_HOME="${jre}"
export PATH="${jre}/bin:/usr/bin:/bin:/usr/sbin:/sbin"

cd "${repo_root}"
exec "${dss}" "${repo_root}/tools/ccs/flash_dual_core_f28p65x.js" "$@"
