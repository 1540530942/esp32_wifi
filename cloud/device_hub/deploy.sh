#!/usr/bin/env bash
# Rebuild and restart device-hub on tang, reproducing every field of the
# running container.
#
# This exists because the deploy was hand-typed each time and twice lost a
# field that only mattered in production:
#
#   * a missing --add-host meant the container could not reach the MQTT broker
#   * a missing --network wangyutang_platform_default put it outside the network
#     Caddy proxies to, and the whole site returned 502
#
# Both times `docker inspect` had been run beforehand and the relevant field was
# simply not read out of the output. A checklist that lives in a doc does not
# help if the command is retyped from memory; this file IS the checklist.
#
# The container deliberately sits on TWO networks: `bridge` for the published
# 8102 port and host.docker.internal, and `wangyutang_platform_default` so Caddy
# can route to it by name. Dropping either one breaks something different.
#
# Why this is a script and not a compose service: device_hub is NOT in
# /root/wangyutang_platform/docker-compose.yml. That compose project is what
# creates the wangyutang_platform_default network, and this container was
# started by hand and attached to it afterwards -- which is exactly why the
# network is easy to forget on a redeploy. Moving the service into that compose
# file would be the better fix, but it is a shared file covering a dozen other
# services, so that is left as a deliberate todo rather than done in passing.
#
#   ./deploy.sh            build, restart, verify
#   ./deploy.sh --check    verify only, change nothing
set -euo pipefail

HOST=tang
NAME=device-hub
IMAGE=device-hub:local
PORT=8102
VOLUME=wangyutang_platform_device_hub_data
CONTEXT=/root/wangyutang_platform/device_hub
# tang cannot reach registry-1.docker.io (the build fails with a dial timeout,
# and `docker run alpine` fails the same way). The Dockerfile takes PYTHON_IMAGE
# as a build arg for exactly this reason; point it at the mirrored base that is
# already in the local image store so a rebuild never needs Docker Hub.
PYTHON_IMAGE=docker.m.daocloud.io/library/python:3.12-slim
NETWORKS=(bridge wangyutang_platform_default)
HEALTH_URL=https://www.wangyutang.cn/devices/api/list

verify() {
    echo "--- 校验 ---"
    local nets
    nets=$(ssh "$HOST" "sudo docker inspect $NAME -f '{{range \$k,\$v := .NetworkSettings.Networks}}{{\$k}} {{end}}'")
    echo "networks: $nets"
    for n in "${NETWORKS[@]}"; do
        if [[ "$nets" != *"$n"* ]]; then
            echo "!! 缺少网络 $n —— 这正是上次 502 的成因" >&2
            return 1
        fi
    done
    ssh "$HOST" "sudo docker inspect $NAME -f '{{.HostConfig.ExtraHosts}} {{.Config.Env}}'" \
        | grep -q 'host.docker.internal' || {
            echo "!! 缺少 host.docker.internal —— 连不上 MQTT broker" >&2; return 1; }
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' -m 20 "$HEALTH_URL")
    echo "health: $code"
    [[ "$code" == "200" ]] || { echo "!! 健康检查未通过" >&2; return 1; }
    echo "OK"
}

if [[ "${1:-}" == "--check" ]]; then
    verify
    exit $?
fi

echo "--- 上传源码 ---"
scp server.py "$HOST:/tmp/device_hub_server.py"
# The build context lives under /root, so every step here needs sudo.
ssh "$HOST" "sudo cp /tmp/device_hub_server.py $CONTEXT/server.py"

echo "--- 构建 ---"
ssh "$HOST" "cd $CONTEXT && sudo docker build -q --build-arg PYTHON_IMAGE=$PYTHON_IMAGE -t $IMAGE ."

echo "--- 重建容器 ---"
# First network goes on `docker run`; the rest are connected afterwards, because
# `docker run` accepts only one --network.
ssh "$HOST" "sudo docker rm -f $NAME >/dev/null 2>&1 || true
sudo docker run -d --name $NAME \
    --restart unless-stopped \
    --network ${NETWORKS[0]} \
    -p $PORT:$PORT \
    -v $VOLUME:/app/data \
    --add-host host.docker.internal:host-gateway \
    -e MQTT_HOST=host.docker.internal \
    -e PYTHONDONTWRITEBYTECODE=1 -e PYTHONUNBUFFERED=1 \
    $IMAGE \
    uvicorn server:app --host 0.0.0.0 --port $PORT >/dev/null
$(for n in "${NETWORKS[@]:1}"; do echo "sudo docker network connect $n $NAME || true"; done)"

sleep 6
verify
