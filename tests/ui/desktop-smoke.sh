#!/usr/bin/env bash
# Smoke test the real GTK process and its private snapshot import.
set -euo pipefail
bin=$(realpath "$1")
backend=${2:-x11}
work=$(mktemp -d)
ulimit -c 0
compositor=
app=
cleanup() {
    if [ -n "$app" ]; then
        kill "$app" 2>/dev/null || true
        wait "$app" 2>/dev/null || true
    fi
    if [ -n "$compositor" ]; then
        kill "$compositor" 2>/dev/null || true
        wait "$compositor" 2>/dev/null || true
    fi
    rm -rf -- "$work"
}
trap cleanup EXIT
mkdir "$work/source" "$work/runtime"
chmod 700 "$work/runtime"
export XDG_RUNTIME_DIR="$work/runtime" XDG_CONFIG_HOME="$work/config"
export XDG_CACHE_HOME="$work/cli-cache"
printf 'metadata fixture\n' > "$work/source/한글.txt"
"$bin/diskscope-cli" scan --root "$work/source" --output "$work/result.dscan"
"$bin/diskscope-cli" export --input "$work/result.dscan" --format jsonl \
    --selected-id 2 --metric allocated --output "$work/selected.jsonl"
grep -q '"export_scope":"selection"' "$work/selected.jsonl"
renderers=(default cairo)
restart_args=()
case "$backend" in
    x11) : "${DISPLAY:?Run under xvfb-run or set a test X11 display}" ;;
    wayland)
        export WAYLAND_DISPLAY=diskscope-smoke
        # The headless compositor has no DRM device. On WSL 1 the Mesa
        # EGL frame path can fault even when software GL is requested.
        renderers=(cairo)
        restart_args=(--cairo)
        weston --backend=headless-backend.so --socket="$WAYLAND_DISPLAY" \
            --idle-time=0 --log="$work/weston.log" &
        compositor=$!
        for _ in {1..80}; do
            [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ] || break
            kill -0 "$compositor" 2>/dev/null || break
            sleep 0.1
        done
        if [ ! -S "$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY" ]; then
            cat "$work/weston.log" >&2
            exit 1
        fi
        ;;
    *) printf 'Expected x11 or wayland\n' >&2; exit 2 ;;
esac

for renderer in "${renderers[@]}"; do
    args=()
    scale=1
    if [ "$renderer" = cairo ]; then args+=(--cairo); scale=2; fi
    export XDG_CACHE_HOME="$work/$renderer"
    GDK_BACKEND="$backend" GDK_SCALE="$scale" GTK_A11Y=none G_DEBUG=fatal-criticals \
        "$bin/diskscope" "${args[@]}" "$work/result.dscan" > "$work/app.log" 2>&1 &
    app=$!
    copy=
    for _ in {1..100}; do
        kill -0 "$app" 2>/dev/null || break
        if [ -d "$XDG_CACHE_HOME" ]; then
            copy=$(find "$XDG_CACHE_HOME" -type f -name import.dscan -print -quit)
            [ -z "$copy" ] || break
        fi
        sleep 0.1
    done
    if [ -z "$copy" ] || ! kill -0 "$app" 2>/dev/null; then
        cat "$work/app.log" >&2
        printf 'GTK import failed on %s with renderer %s\n' "$backend" "$renderer" >&2
        exit 1
    fi
    "$bin/diskscope-cli" export --input "$copy" --format json --output "$work/verified.json"
    sleep 0.3
    kill -0 "$app"
    import_dir=$(dirname "$copy")
    test -s "$import_dir/cache-owner.v1"
    if flock -n "$import_dir/scan.lock" true; then
        printf 'Displayed snapshot failed to hold its cache lease\n' >&2
        exit 1
    fi
    kill "$app"
    wait "$app" || true
    app=
    flock -n "$import_dir/scan.lock" true
    printf 'PASS GTK import · %s · %s · GDK_SCALE=%s\n' "$backend" "$renderer" "$scale"
done

export XDG_CACHE_HOME="$work/cli-cache"
lease=$(find "$XDG_CACHE_HOME" -type f -name scan.lock -print -quit)
test -n "$lease"
test -s "$(dirname "$lease")/cache-owner.v1"
flock -n "$lease" true
mv "$work/source" "$work/moved-source"
GDK_BACKEND="$backend" GTK_A11Y=none G_DEBUG=fatal-criticals \
    "$bin/diskscope" "${restart_args[@]}" > "$work/app.log" 2>&1 &
app=$!
recovered=false
for _ in {1..100}; do
    kill -0 "$app" 2>/dev/null || break
    for descriptor in "/proc/$app/fd/"*; do
        if [ "$(readlink "$descriptor" 2>/dev/null || true)" = "$lease" ]; then
            recovered=true
            break 2
        fi
    done
    sleep 0.1
done
if ! "$recovered"; then
    cat "$work/app.log" >&2
    printf 'GTK restart did not acquire the cached scan on %s\n' "$backend" >&2
    exit 1
fi
sleep 0.3
kill -0 "$app"
kill "$app"
wait "$app" || true
app=
flock -n "$lease" true
printf 'PASS startup cache recovery · %s · original directory moved\n' "$backend"
