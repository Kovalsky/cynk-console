#!/bin/sh
# Install cynk-console from GitHub Releases.
#
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/Kovalsky/cynk-console/master/scripts/install.sh | sh
#
# Environment variables:
#   CYNK_VERSION      — version to install (default: latest)
#   CYNK_INSTALL_DIR  — installation directory (default: /usr/local/bin)

set -eu

REPO="Kovalsky/cynk-console"
VERSION="${CYNK_VERSION:-}"
INSTALL_DIR="${CYNK_INSTALL_DIR:-/usr/local/bin}"

info()  { printf '  \033[1;34m>\033[0m %s\n' "$*"; }
error() { printf '  \033[1;31m!\033[0m %s\n' "$*" >&2; }
ok()    { printf '  \033[1;32m✓\033[0m %s\n' "$*"; }

detect_platform() {
  case "$(uname -s)" in
    Linux*)  echo "linux" ;;
    Darwin*) echo "macos" ;;
    *)       error "Unsupported OS: $(uname -s)"; exit 1 ;;
  esac
}

detect_arch() {
  case "$(uname -m)" in
    x86_64|amd64)   echo "x86_64" ;;
    aarch64|arm64)
      platform="$(detect_platform)"
      if [ "$platform" = "macos" ]; then
        echo "arm64"
      else
        echo "aarch64"
      fi
      ;;
    *) error "Unsupported architecture: $(uname -m)"; exit 1 ;;
  esac
}

resolve_version() {
  if [ -n "$VERSION" ]; then
    echo "$VERSION"
    return
  fi

  if command -v curl >/dev/null 2>&1; then
    tag=$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"//;s/".*//')
  elif command -v wget >/dev/null 2>&1; then
    tag=$(wget -qO- "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"//;s/".*//')
  else
    error "curl or wget required"
    exit 1
  fi

  if [ -z "$tag" ]; then
    error "Could not determine latest version"
    exit 1
  fi

  echo "$tag"
}

download() {
  url="$1"
  dest="$2"
  if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$dest" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -qO "$dest" "$url"
  fi
}

main() {
  platform="$(detect_platform)"
  arch="$(detect_arch)"
  version="$(resolve_version)"
  binary="cynk-console-${platform}-${arch}"
  base_url="https://github.com/${REPO}/releases/download/${version}"

  info "Platform: ${platform} ${arch}"
  info "Version: ${version}"

  tmpdir="$(mktemp -d)"
  trap 'rm -rf "$tmpdir"' EXIT

  info "Downloading ${binary}..."
  download "${base_url}/${binary}" "${tmpdir}/cynk-console"

  info "Downloading checksums..."
  if download "${base_url}/checksums.txt" "${tmpdir}/checksums.txt" 2>/dev/null; then
    expected=$(grep "${binary}" "${tmpdir}/checksums.txt" | awk '{print $1}')
    if [ -n "$expected" ]; then
      if command -v sha256sum >/dev/null 2>&1; then
        actual=$(sha256sum "${tmpdir}/cynk-console" | awk '{print $1}')
      elif command -v shasum >/dev/null 2>&1; then
        actual=$(shasum -a 256 "${tmpdir}/cynk-console" | awk '{print $1}')
      else
        info "No sha256sum available, skipping checksum verification"
        actual="$expected"
      fi

      if [ "$expected" != "$actual" ]; then
        error "Checksum mismatch!"
        error "  expected: ${expected}"
        error "  actual:   ${actual}"
        exit 1
      fi
      ok "Checksum verified"
    fi
  else
    info "No checksums available, skipping verification"
  fi

  chmod +x "${tmpdir}/cynk-console"

  if [ -w "$INSTALL_DIR" ] 2>/dev/null; then
    cp "${tmpdir}/cynk-console" "${INSTALL_DIR}/cynk-console"
  elif [ -w "$(dirname "$INSTALL_DIR")" ] 2>/dev/null; then
    mkdir -p "$INSTALL_DIR"
    cp "${tmpdir}/cynk-console" "${INSTALL_DIR}/cynk-console"
  else
    info "Installing to ${INSTALL_DIR} (requires sudo)..."
    sudo mkdir -p "$INSTALL_DIR"
    sudo cp "${tmpdir}/cynk-console" "${INSTALL_DIR}/cynk-console"
  fi

  ok "Installed cynk-console ${version} to ${INSTALL_DIR}/cynk-console"

  if [ "$platform" = "macos" ]; then
    info "Note: if macOS blocks the binary, run:"
    info "  xattr -d com.apple.quarantine ${INSTALL_DIR}/cynk-console"
  fi

  "${INSTALL_DIR}/cynk-console" --version
}

main
