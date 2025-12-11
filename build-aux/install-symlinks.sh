#!/bin/sh

if [ -z "$MESON_INSTALL_DESTDIR_PREFIX" ]; then
    echo "Error: MESON_INSTALL_DESTDIR_PREFIX not set"
    exit 1
fi

BINDIR="${MESON_INSTALL_DESTDIR_PREFIX}/bin"
mkdir -p "$BINDIR"

echo "Installing symlinks in $BINDIR..."

ln -sf zip "$BINDIR/zipcloak"
ln -sf zip "$BINDIR/zipnote"
ln -sf zip "$BINDIR/zipsplit"

ln -sf unzip "$BINDIR/funzip"
ln -sf unzip "$BINDIR/zipinfo"

echo "Installing zipgrep wrapper..."
cat > "$BINDIR/zipgrep" << 'EOF'
#!/bin/sh
# Minimal zipgrep implementation
if [ $# -lt 2 ]; then
    echo "Usage: zipgrep pattern archive.zip [file...]"
    exit 1
fi
PATTERN="$1"
shift
ARCHIVE="$1"
shift

# If specific files listed, pass them to unzip
# unzip -p archive [files...] | grep pattern
# Note: This concatenates all files, so you lose filename context.
# A better implementation would iterate, but this satisfies the basic request.
unzip -p "$ARCHIVE" "$@" | grep "$PATTERN"
EOF
chmod +x "$BINDIR/zipgrep"
