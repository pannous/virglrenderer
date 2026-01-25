#!/bin/bash
set -e

# Build configuration
BUILD_DIR="${BUILD_DIR:-build}"
INSTALL_DIR="${INSTALL_DIR:-install}"
BUILD_TYPE="release"
VENUS="auto"
TESTS="false"
CLEAN="false"
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# Parse arguments
for arg in "$@"; do
    case $arg in
        --clean)
            CLEAN="true"
            ;;
        --debug)
            BUILD_TYPE="debug"
            ;;
        --release)
            BUILD_TYPE="release"
            ;;
        --venus)
            VENUS="true"
            ;;
        --no-venus)
            VENUS="false"
            ;;
        --tests)
            TESTS="true"
            ;;
        -j*)
            JOBS="${arg#-j}"
            ;;
        --help)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --clean       Clean build directory before building"
            echo "  --debug       Build with debug symbols (default: release)"
            echo "  --release     Build in release mode"
            echo "  --venus       Enable Venus Vulkan passthrough"
            echo "  --no-venus    Disable Venus"
            echo "  --tests       Enable building tests"
            echo "  -jN           Use N parallel jobs (default: $JOBS)"
            echo ""
            echo "Environment variables:"
            echo "  BUILD_DIR     Build directory (default: builddir)"
            echo "  INSTALL_DIR   Install prefix (default: install)"
            exit 0
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Run '$0 --help' for usage"
            exit 1
            ;;
    esac
done

# Auto-detect macOS and enable Venus by default
if [[ "$VENUS" == "auto" ]]; then
    if [[ "$(uname)" == "Darwin" ]]; then
        VENUS="true"
        echo "📦 Detected macOS - enabling Venus Vulkan passthrough"
    else
        VENUS="false"
    fi
fi

# Clean if requested
if [[ "$CLEAN" == "true" ]]; then
    echo "🧹 Cleaning build directory: $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Configure if needed
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    echo "⚙️  Configuring meson build..."
    echo "   Build type: $BUILD_TYPE"
    echo "   Venus: $VENUS"
    echo "   Tests: $TESTS"
    echo "   Install prefix: $(pwd)/$INSTALL_DIR"

    meson setup "$BUILD_DIR" \
        --prefix="$(pwd)/$INSTALL_DIR" \
        --buildtype="$BUILD_TYPE" \
        -Dvenus="$VENUS" \
        -Dtests="$TESTS"
else
    echo "🔧 Build directory exists, reconfiguring if needed..."
    meson configure "$BUILD_DIR" \
        --buildtype="$BUILD_TYPE" \
        -Dvenus="$VENUS" \
        -Dtests="$TESTS"
fi

# Build
echo "🔨 Building with $JOBS parallel jobs..."
meson compile -C "$BUILD_DIR" -j "$JOBS"

echo "✅ Build complete!"
echo ""
echo "Build artifacts:"
echo "  Library:     $BUILD_DIR/src/libvirglrenderer.dylib"
echo "  Server:      $BUILD_DIR/server/virgl_render_server"
echo ""
echo "To install:"
echo "  meson install -C $BUILD_DIR"
echo ""
echo "Development environment paths:"
echo "  export DYLD_LIBRARY_PATH=$(pwd)/$BUILD_DIR/src:/opt/homebrew/lib:\${DYLD_LIBRARY_PATH:-}"
echo "  export RENDER_SERVER_EXEC_PATH=$(pwd)/$BUILD_DIR/server/virgl_render_server"
