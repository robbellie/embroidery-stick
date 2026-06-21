#!/bin/bash
set -e

GO_VERSION="1.22.4"
ARCH="linux-amd64"
TARBALL="go${GO_VERSION}.${ARCH}.tar.gz"
URL="https://go.dev/dl/${TARBALL}"

echo "Downloading Go ${GO_VERSION}..."
wget -q --show-progress "${URL}" -O "/tmp/${TARBALL}"

echo "Installing to /usr/local/go..."
rm -rf /usr/local/go
tar -C /usr/local -xzf "/tmp/${TARBALL}"
rm "/tmp/${TARBALL}"

echo 'export PATH=$PATH:/usr/local/go/bin' > /etc/profile.d/golang.sh

export PATH=$PATH:/usr/local/go/bin
echo "Done: $(go version)"
echo "Open a new terminal to use 'go'."
