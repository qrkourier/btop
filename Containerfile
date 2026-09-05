# SPDX-License-Identifier: Apache-2.0

# Match the minimum GNU toolchain exercised by upstream's Linux CMake CI.
FROM docker.io/library/gcc:14-bookworm

RUN apt-get update \
	&& apt-get install --yes --no-install-recommends \
		ca-certificates \
		cmake \
		git \
		lowdown \
		ninja-build \
		python3 \
		python3-pyte \
	&& rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src

ENTRYPOINT ["/src/scripts/container-entrypoint"]
