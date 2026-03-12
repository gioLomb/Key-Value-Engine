#!/bin/bash

HOST="http://localhost:8080"
PASS=0
FAIL=0

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

# ── helpers ────────────────────────────────────────────────────────────────────

check() {
    local description=$1
    local expected=$2
    local actual=$3

    if echo "$actual" | grep -q "$expected"; then
        echo -e "${GREEN}[PASS]${NC} $description"
        ((PASS++))
    else
        echo -e "${RED}[FAIL]${NC} $description"
        echo "       expected: '$expected'"
        echo "       got:      '$actual'"
        ((FAIL++))
    fi
}

check_status() {
    local description=$1
    local expected_status=$2
    local actual_status=$3

    if [ "$actual_status" -eq "$expected_status" ]; then
        echo -e "${GREEN}[PASS]${NC} $description"
        ((PASS++))
    else
        echo -e "${RED}[FAIL]${NC} $description"
        echo "       expected status: $expected_status"
        echo "       got status:      $actual_status"
        ((FAIL++))
    fi
}

cleanup() {
    kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    rm -f server
}

# ── build ──────────────────────────────────────────────────────────────────────

gcc -DDEBUG route_handler.c hash_table.c server_functions.c threadPool.c -lpthread -o server
if [ $? -ne 0 ]; then
    echo "Compilation failed"
    exit 1
fi

# ── start server ───────────────────────────────────────────────────────────────

# kill any leftover process on the port
kill $(lsof -t -i:8080) 2>/dev/null
sleep 0.5

./server &
SERVER_PID=$!
trap cleanup EXIT INT TERM
sleep 1

# ── tests ──────────────────────────────────────────────────────────────────────

echo "================================"
echo "  Running server tests"
echo "================================"
echo ""

# --- /set ---
echo ">> SET"

echo ">> SET MULTIPLE RECORDS"

names=("mario" "luigi" "peach" "bowser" "toad" "yoshi" "wario" "donkey")

for i in "${!names[@]}"; do
    key="name_$i"
    val="${names[$i]}"
    response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/set?key=$key&val=$val")
    check_status "set key=$key val=$val" 200 "$response"
done

echo ""

response=$(curl -s "$HOST/set?key=name&val=mario")
check        "set returns 'stored'"                "stored" "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/set?key=name")
check_status "set missing val returns 400"         400 "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/set?val=mario")
check_status "set missing key returns 400"         400 "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/set")
check_status "set no params returns 400"           400 "$response"

echo ""

# --- /get ---
echo ">> GET"

response=$(curl -s "$HOST/get?key=name")
check        "get existing key returns value"      "mario" "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/get?key=name")
check_status "get existing key returns 200"        200 "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/get?key=nonexistent")
check_status "get nonexistent key returns 404"     404 "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/get")
check_status "get no params returns 400"           400 "$response"

echo ""

# --- /delete ---
echo ">> DELETE"

curl -s "$HOST/set?key=todelete&val=temp" > /dev/null

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/delete?key=todelete")
check_status "delete existing key returns 200"     200 "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/get?key=todelete")
check_status "deleted key is no longer found"      404 "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/delete?key=todelete")
check_status "delete already deleted key returns 404" 404 "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/delete")
check_status "delete no params returns 400"        400 "$response"

echo ""

# --- routing ---
echo ">> ROUTING"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/nonexistent")
check_status "unknown route returns 404"           404 "$response"

echo ""

# --- overwrite ---
echo ">> OVERWRITE"

curl -s "$HOST/set?key=color&val=red"  > /dev/null
curl -s "$HOST/set?key=color&val=blue" > /dev/null
response=$(curl -s "$HOST/get?key=color")
check        "overwrite existing key updates value" "blue" "$response"

echo ""

# --- sanitization ---
echo ">> SANITIZATION"

response=$(curl -s -o /dev/null -w "%{http_code}" "$HOST/set?key=bad%01key&val=test")
check_status "set key with percent-encoded control char returns 400" 400 "$response"

response=$(curl -s -o /dev/null -w "%{http_code}" --get \
    --data-urlencode "key=bad"$'\x01'"key" \
    --data-urlencode "val=test" \
    "$HOST/set")
check_status "set key with literal control char returns 400" 400 "$response"

echo ""

# ── results ────────────────────────────────────────────────────────────────────

echo "================================"
echo "  Results: ${PASS} passed, ${FAIL} failed"
echo "================================"

[ "$FAIL" -eq 0 ] && exit 0 || exit 1