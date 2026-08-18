#!/bin/zsh

set -eu

repo_dir="${0:A:h:h}"
build_dir="${CUTMACHINE_DEV_BUILD_DIR:-${repo_dir}/build}"
app_path="${build_dir}/CUTMACHINE.app"

build_and_launch() {
    cmake -S "${repo_dir}" -B "${build_dir}" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    cmake --build "${build_dir}" --target cutmachine_app -j
    osascript -e 'tell application id "com.cutmachine.editor" to quit' \
        >/dev/null 2>&1 || true
    for _ in {1..40}; do
        if ! pgrep -x CUTMACHINE >/dev/null 2>&1; then
            break
        fi
        sleep 0.1
    done
    open "${app_path}"
}

build_and_launch
print "CUTMACHINE dev : surveillance active (Ctrl-C pour arrêter)"

source_fingerprint() {
    {
        find "${repo_dir}/src" "${repo_dir}/assets" -type f \
            -exec stat -f '%m:%z:%N' {} +
        stat -f '%m:%z:%N' "${repo_dir}/CMakeLists.txt"
    } | cksum
}

previous_fingerprint="$(source_fingerprint)"
while sleep 0.5; do
    current_fingerprint="$(source_fingerprint)"
    if [[ "${current_fingerprint}" == "${previous_fingerprint}" ]]; then
        continue
    fi
    previous_fingerprint="${current_fingerprint}"
    if ! build_and_launch; then
        print -u2 "Build échoué ; attente de la prochaine modification."
    fi
done
