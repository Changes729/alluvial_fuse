#!/bin/bash

SCRIPT_DIR=$(dirname $(readlink -f "$0"))
MAIN_DIR=$SCRIPT_DIR/..

# Install prefix: default ~/.local, binary lands in ~/.local/bin
PREFIX=${PREFIX:-$HOME/.local}
BUILD_DIR=${BUILD_DIR:-$MAIN_DIR/build}
BIN_NAME=alluvial_fuse

function usage() {
  echo "Usage: $0 [install|uninstall]"
  echo "  install (default)  build if needed, then install to \${PREFIX}/bin (default: ~/.local/bin)"
  echo "  PREFIX=<dir>       override install prefix, e.g. PREFIX=/usr/local $0"
  echo "  uninstall          remove files listed in install_manifest.txt"
}

function do_install() {
  # build first (incremental, fast when already up to date)
  "$SCRIPT_DIR/build.sh" deploy || exit 1

  cmake --install "$BUILD_DIR" --prefix "$PREFIX" || exit 1

  echo
  echo "Installed: $PREFIX/bin/$BIN_NAME"
  echo "Ensure $PREFIX/bin is in PATH:"
  echo "  export PATH=\"$PREFIX/bin:\$PATH\""
}

function do_uninstall() {
  local manifest=$BUILD_DIR/install_manifest.txt
  if [ ! -f $manifest ]; then
    echo "No install manifest found: $manifest"
    exit 1
  fi

  while IFS= read -r f || [ -n "$f" ]; do
    if [ -f "$f" ]; then
      rm -v "$f"
    fi
  done < $manifest
  echo "Uninstalled from $PREFIX"
}

## Start from here. #####################################
input=${1:-install}

case "$input" in
"-h" | "--help" | "help")
  usage
  exit 0
  ;;

"install")
  do_install
  ;;

"uninstall")
  do_uninstall
  ;;

*)
  usage
  exit 1
  ;;
esac
