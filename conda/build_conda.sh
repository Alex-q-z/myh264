#!/usr/bin/env bash

set -eou pipefail

docker run -it --rm \
    -v `pwd`/..:/home/myh264 \
    "pytorch/conda-cuda:latest" \
    /bin/bash 