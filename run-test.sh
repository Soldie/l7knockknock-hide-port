#!/usr/bin/env bash

# safer bash script 
set -o nounset -o errexit -o pipefail
# don't split on spaces, only on lines
IFS=$'\n\t'


readonly GLOBAL_TIMEOUT=4
readonly KNOCK_TIMEOUT=1
readonly TEST_PORT=5511
readonly TEST_HIDDEN_PORT=5522
readonly TEST_PROXY_PORT=6611
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

go run "test/server.go" --port $TEST_PORT --specialPort $TEST_HIDDEN_PORT &
readonly SERVER_PID=$!
kill_server() {
    kill $SERVER_PID || true
}
trap kill_server EXIT

VALGRIND="false"
if [ $# -eq 2 ]; then
    if [[ "$2" == "--valgrind" ]]; then
        VALGRIND="true"
    fi
fi
if [[ "$VALGRIND" == "true" ]]; then
    valgrind --log-file='valgrind.log' -v --leak-check=full --show-leak-kinds=all $TARGET --normalPort=$TEST_PORT --listenPort=$TEST_PROXY_PORT --hiddenPort=$TEST_HIDDEN_PORT --proxyTimeout=$GLOBAL_TIMEOUT --knockTimeout=$KNOCK_TIMEOUT PASSWORD 2> /dev/null &
else
    $TARGET --normalPort=$TEST_PORT --listenPort=$TEST_PROXY_PORT --hiddenPort=$TEST_HIDDEN_PORT --proxyTimeout=$GLOBAL_TIMEOUT --knockTimeout=$KNOCK_TIMEOUT PASSWORD 2> /dev/null &
fi
readonly PROXY_PID=$!

kill_proxy() {
    kill $PROXY_PID || true
    wait $PROXY_PID || true
    if [[ "$VALGRIND" == "true" ]]; then
        cat 'valgrind.log'
        rm 'valgrind.log' || true
    fi
}
trap kill_proxy ERR

sleep 2

HIDE_PROGRESS=""
if [ ! -z "${CI+x}" ]; then
    if [[ "$CI" == "true" ]]; then
        HIDE_PROGRESS="--hideProgress"
    fi
fi
run_test() {
    go run "test/client.go" --port $TEST_PROXY_PORT  --connections "$1" --parallel "$2" $HIDE_PROGRESS
}

echo ""
echo "/----------------"
echo "| Testing hidden port"
echo "\\----------------"
HIDDEN_ANSWER=$(timeout 2 bash -c "exec 3<>/dev/tcp/127.0.0.1/$TEST_PROXY_PORT && echo -ne 'PASSWORD' >&3 && cat <&3 && exec 3<&-")
if [[ "$HIDDEN_ANSWER" != "HELLO" ]]; then
    echo "Error, correct answer not received"
    exit 1
else
    echo "OK"
fi

echo "" 
echo "/----------------"
echo "| Running single threaded test case"
echo "\\----------------"
run_test 20 1
run_test 200 1

echo "" 
echo "/----------------"
echo "| Running multi-threaded test case"
echo "\\----------------"
run_test 20 20
run_test 200 40

echo "" 
echo "/----------------"
echo "| Running time-out test cases"
echo "\\----------------"
echo " + Within the proxy window"
go run "test/client.go" --port $TEST_PROXY_PORT  --connections 5 --parallel 4 --maxDelays $(( $GLOBAL_TIMEOUT / 2 )) $HIDE_PROGRESS
echo " + Sometimes outside the proxy window"
go run "test/client.go" --port $TEST_PROXY_PORT  --connections 5 --parallel 4 --maxDelays $(( $GLOBAL_TIMEOUT * 2 )) $HIDE_PROGRESS && rc=$? || rc=$?
if [ $rc -ne 1 ]; then
    echo "The timeouts outside the windows should have failed"
    exit 1
else
    echo "OK"
fi
echo " + Does the proxy still work?"
run_test 100 20

echo "Waiting for all timeouts to pass, so that all memory is freed, and Valgrind will only report true leaks"
sleep $(( $GLOBAL_TIMEOUT + 2 ))

kill $PROXY_PID
wait $PROXY_PID || true

if [[ "$VALGRIND" == "true" ]]; then
    cat 'valgrind.log'
    if grep -q "ERROR SUMMARY: 0 errors" 'valgrind.log'; then
        echo "Valgrind: OK"
        rm 'valgrind.log' || true
    else
        echo "Valgrind: Fail, reported errors"
        exit 1
    fi
fi

echo "/----------------"
echo "| All test are green"
echo "\\----------------"
