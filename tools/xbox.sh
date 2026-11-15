#!/usr/bin/env bash
# Talks to the Xbox Device Portal: install builds, launch the app, fetch logs and
# screenshots. Set XBOX to the console's address (default 192.168.2.3).
#
#   tools/xbox.sh install dist/            install the .msix and .appx files in a folder
#   tools/xbox.sh run [args]               start Claude Code (via autostart.txt) and launch the app
#   tools/xbox.sh input '\e[B\r'           send keystrokes (escapes: \e \r \n \t \xNN)
#   tools/xbox.sh stop                     close the app
#   tools/xbox.sh logs [dir]               download loader.log and output.log
#   tools/xbox.sh shot [dir]               save the app's terminal as screen.txt and screen.png
#   tools/xbox.sh screenshot [file.png]    save what's on screen (fails while the app is in front)
set -euo pipefail

XBOX="${XBOX:-192.168.2.3}"
BASE="https://$XBOX:11443"
JAR="$(mktemp)"
trap 'rm -f "$JAR"' EXIT

csrf() {
    curl -sk -c "$JAR" -o /dev/null "$BASE/api/os/info"
    awk '$6 == "CSRF-Token" { print $7 }' "$JAR"
}

package_name() {
    curl -sk "$BASE/api/app/packagemanager/packages" |
        python3 -I -c 'import json,sys; print(next((p["PackageFullName"] for p in json.load(sys.stdin)["InstalledPackages"] if p["PackageFullName"].startswith("Cexbox_")), ""))'
}

app_id() {
    curl -sk "$BASE/api/app/packagemanager/packages" |
        python3 -I -c 'import json,sys; print(next((p["PackageRelativeId"] for p in json.load(sys.stdin)["InstalledPackages"] if p["PackageFullName"].startswith("Cexbox_")), ""))'
}

b64() { printf '%s' "$1" | base64 | tr -d '\n'; }

case "${1:-}" in
install)
    dir="${2:-dist}"
    msix="$(ls "$dir"/Cexbox_*.msix | head -1)"
    token="$(csrf)"
    args=(-F "package=@$msix")
    for dep in "$dir"/*.appx; do args+=(-F "dep=@$dep"); done
    echo "installing $(basename "$msix")"
    curl -sk -b "$JAR" -H "X-CSRF-Token: $token" "${args[@]}" \
        "$BASE/api/app/packagemanager/package?package=$(basename "$msix")" -w "upload: http %{http_code}\n" -o /dev/null
    for _ in $(seq 1 120); do
        code="$(curl -sk -o /tmp/xbox-state.json -w '%{http_code}' "$BASE/api/app/packagemanager/state")"
        if [ "$code" != "204" ]; then
            cat /tmp/xbox-state.json; echo
            break
        fi
        sleep 1
    done
    echo "installed: $(package_name)"
    ;;
run)
    shift
    pkg="$(package_name)"
    token="$(csrf)"
    printf '%s' "${*:-}" > /tmp/autostart.txt
    curl -sk -b "$JAR" -H "X-CSRF-Token: $token" -F "file=@/tmp/autostart.txt;filename=autostart.txt" \
        "$BASE/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$pkg&path=%5CLocalState" \
        -w "autostart: http %{http_code}\n" -o /dev/null
    curl -sk -b "$JAR" -H "X-CSRF-Token: $token" -X DELETE "$BASE/api/taskmanager/app?package=$(b64 "$pkg")" -o /dev/null || true
    sleep 2
    curl -sk -b "$JAR" -H "X-CSRF-Token: $token" -X POST --data "" \
        "$BASE/api/taskmanager/app?appid=$(b64 "$(app_id)")&package=$(b64 "$pkg")" -w "launch: http %{http_code}\n" -o /dev/null
    ;;
input)
    # Sends keystrokes to the running program. Escapes: \e \r \n \t \xNN (e.g. '\e[B\r' = down, enter).
    pkg="$(package_name)"
    token="$(csrf)"
    printf '%s' "${2:-}" > /tmp/input.txt
    curl -sk -b "$JAR" -H "X-CSRF-Token: $token" -F "file=@/tmp/input.txt;filename=input.txt" \
        "$BASE/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$pkg&path=%5CLocalState" \
        -w "input: http %{http_code}\n" -o /dev/null
    ;;
stop)
    token="$(csrf)"
    curl -sk -b "$JAR" -H "X-CSRF-Token: $token" -X DELETE "$BASE/api/taskmanager/app?package=$(b64 "$(package_name)")" \
        -w "stop: http %{http_code}\n" -o /dev/null
    ;;
logs)
    dir="${2:-dist/xbox-logs}"
    mkdir -p "$dir"
    pkg="$(package_name)"
    for f in loader.log output.log app.log; do
        curl -sk -G "$BASE/api/filesystem/apps/file" \
            --data-urlencode "knownfolderid=LocalAppData" --data-urlencode "packagefullname=$pkg" \
            --data-urlencode "filename=$f" --data-urlencode 'path=\LocalState\logs' -o "$dir/$f"
    done
    echo "saved to $dir"
    ;;
shot)
    # Asks the app to save its terminal (text and image), then downloads both.
    dir="${2:-dist/xbox-logs}"
    mkdir -p "$dir"
    pkg="$(package_name)"
    token="$(csrf)"
    printf 'now' > /tmp/screenshot.txt
    curl -sk -b "$JAR" -H "X-CSRF-Token: $token" -F "file=@/tmp/screenshot.txt;filename=screenshot.txt" \
        "$BASE/api/filesystem/apps/file?knownfolderid=LocalAppData&packagefullname=$pkg&path=%5CLocalState" -o /dev/null
    sleep 3
    for f in screen.txt screen.png; do
        curl -sk -G "$BASE/api/filesystem/apps/file" \
            --data-urlencode "knownfolderid=LocalAppData" --data-urlencode "packagefullname=$pkg" \
            --data-urlencode "filename=$f" --data-urlencode 'path=\LocalState\logs' -o "$dir/$f"
    done
    echo "saved $dir/screen.txt and $dir/screen.png"
    ;;
screenshot)
    out="${2:-dist/xbox-screen.png}"
    curl -sk "$BASE/ext/screenshot?download=true" -o "$out" -w "screenshot: http %{http_code}\n"
    ;;
*)
    sed -n '2,10p' "$0"
    exit 1
    ;;
esac
