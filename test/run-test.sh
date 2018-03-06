#!/usr/bin/env bash

# safer bash script 
set -o nounset -o errexit -o pipefail
# don't split on spaces, only on lines
IFS=$'\n\t'

readonly TARGET="$1"

kill_descendant_processes() {
    local pid="$1"
    local and_self="${2:-false}"
    if children="$(pgrep -P "$pid")"; then
        for child in $children; do
            kill_descendant_processes "$child" true
        done
    fi
    if [[ "$and_self" == true ]]; then
        kill "$pid"
    fi
}

go run server.go --port 5000 &
readonly SERVER_PID=$!
kill_server() {
    kill $SERVER_PID
}
trap kill_server EXIT

"$TARGET" --normalPort=5000 --listenPort=6000 HELLO &
readonly PROXY_PID=$!

kill_proxy() {
    kill_descendant_processes $SERVER_PID  
    kill $PROXY_PID || true
}
trap kill_proxy EXIT

sleep 2

run_test() {
    go run client.go --port 6000  --connections "$1" --parallel "$2"
}

echo "" 
echo "/----------------"
echo "| Running single threaded test case"
echo "\\----------------"
run_test 10 1
run_test 200 1

echo "" 
echo "/----------------"
echo "/----------------"
echo "| Running multi-threaded test case"
echo "\\----------------"
run_test 20 20
run_test 200 40

echo "/----------------"
echo "| All test are green"
echo "\\----------------"

kill $PROXY_PID
