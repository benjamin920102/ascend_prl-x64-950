#!/bin/bash
# Mount the host Ascend devices, driver libraries, and DCMI into the container.
#
# Env:  WALLET (required)  POOL (default 127.0.0.1)  PORT (default 7000)
#       DEVICES (space/comma davinci ids, required — the mining NPUs)
#       TAG (required: Linux amd64 / 950 CANN image)  NAME (default ascend_prl)
#       BIN (default kryptex binary; select ascend_prl_k1 for K1)
set -eu
: "${WALLET:?set WALLET=prl1...}"
: "${DEVICES:?set DEVICES to the device ids reported by npu-smi}"
: "${TAG:?set TAG to your Linux amd64 image with a matching 950 CANN runtime}"
TAG="$TAG"; NAME="${NAME:-ascend_prl}"
POOL="${POOL:-127.0.0.1}"; PORT="${PORT:-7000}"
DEVICES="$DEVICES"; DEVICES="${DEVICES//,/ }"

dev_args=(); vis=""
for d in $DEVICES; do dev_args+=(--device "/dev/davinci$d"); vis="${vis:+$vis,}$d"; done
dev_args+=(--device /dev/davinci_manager --device /dev/devmm_svm --device /dev/hisi_hdc)

mount_args=(
  -v /usr/local/Ascend/driver/lib64/:/usr/local/Ascend/driver/lib64/
  -v /usr/local/Ascend/driver/version.info:/usr/local/Ascend/driver/version.info
  -v /etc/ascend_install.info:/etc/ascend_install.info
  -v /usr/local/dcmi:/usr/local/dcmi
  -v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi
)

echo "running $NAME on NPUs: $vis  pool=$POOL:$PORT"
docker run --platform linux/amd64 -d --name "$NAME" --network host --restart unless-stopped --privileged \
  "${dev_args[@]}" "${mount_args[@]}" \
  -e WALLET="$WALLET" -e POOL="$POOL" -e PORT="$PORT" \
  -e ASCEND_RT_VISIBLE_DEVICES="$vis" \
  ${BIN:+-e BIN="$BIN"} ${WORKER:+-e WORKER="$WORKER"} \
  ${PRL_COEXIST_GUARD:+-e PRL_COEXIST_GUARD="$PRL_COEXIST_GUARD"} \
  ${PRL_GUARD_POLL_MS:+-e PRL_GUARD_POLL_MS="$PRL_GUARD_POLL_MS"} \
  ${PRL_GUARD_ACK_S:+-e PRL_GUARD_ACK_S="$PRL_GUARD_ACK_S"} \
  "$TAG"
echo "logs: docker logs -f $NAME   (per-NPU: docker exec $NAME tail -f /opt/pearl/prl-dev*.log)"
echo "stop: docker rm -f $NAME"
