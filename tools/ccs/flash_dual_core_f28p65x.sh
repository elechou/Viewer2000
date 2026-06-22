#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ccs_root="${CCS_ROOT:-/Applications/ti/ccs2100}"
dss="${ccs_root}/ccs/ccs_base/scripting/bin/dss.sh"

if [[ ! -x "${dss}" ]]; then
    echo "DSS runner not found: ${dss}" >&2
    exit 2
fi

jre=""
for candidate in \
    "${ccs_root}/ccs/ccs-server.app/jre/Contents/Home" \
    "${ccs_root}/ccs/eclipse/Ccstudio.app/jre/Contents/Home" \
    "${ccs_root}/ccs/eclipse/jre/Contents/Home" \
    "${ccs_root}/ccs/eclipse/jre"; do
    if [[ -x "${candidate}/bin/java" ]]; then
        jre="${candidate}"
        break
    fi
done

if [[ -z "${jre}" ]]; then
    echo "CCS bundled JRE not found below ${ccs_root}" >&2
    exit 2
fi

export CCS_ROOT="${ccs_root}"
export TI_APPDATA_DIR="${TI_APPDATA_DIR:-${TMPDIR:-/tmp}/v2k-ti-appdata}"
export JAVA_HOME="${jre}"
export PATH="${jre}/bin:/usr/bin:/bin:/usr/sbin:/sbin"

mkdir -p "${TI_APPDATA_DIR}"
cd "${repo_root}"
exec "${dss}" "${repo_root}/tools/ccs/flash_dual_core_f28p65x.js" "$@"
