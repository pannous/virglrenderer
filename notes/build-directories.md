# Build Directory Explanation

## Why Are There Multiple Directories?

The virglrenderer repository currently has several directories that look like build folders:

### 1. `build/` - Meson build directory
- **Type**: Meson build output directory
- **Purpose**: Contains compiled objects, ninja files, and build artifacts
- **Created by**: `meson setup build`
- **Updated**: Jan 25 14:22

### 2. `builddir/` - Alternative Meson build directory
- **Type**: Meson build output directory (duplicate)
- **Purpose**: Same as `build/`, possibly with different configuration options
- **Created by**: `meson setup builddir`
- **Updated**: Jan 25 12:26
- **Note**: Having two build directories allows testing different configurations without reconfiguring

### 3. `install/` - Installation prefix
- **Type**: Installation destination directory
- **Purpose**: Contains the installed libraries, headers, and binaries (like `lib/`, `include/`, `bin/`, `libexec/`)
- **Created by**: `meson install` with `-D prefix=install`
- **Not a build directory**: This is the OUTPUT of the build process

### 4. `server/` - SOURCE directory (NOT a build folder!)
- **Type**: Source code directory
- **Purpose**: Contains the virgl render server C source files
- **Files**: `main.c`, `render_client.c`, `render_server.c`, etc.
- **This is NOT a build artifact**: It's part of the source tree

## Recommendation

You typically only need ONE build directory. The current setup has:
- Two build directories (`build` and `builddir`) - likely redundant unless testing different configs
- One install directory (`install`) - this is fine
- One source directory (`server`) - this is part of the project

## Development vs Install Paths

For active development, use build directory paths (no install step needed):

```bash
# Development setup
export DYLD_LIBRARY_PATH=/opt/other/virglrenderer/build/src:/opt/homebrew/lib:${DYLD_LIBRARY_PATH:-}
export RENDER_SERVER_EXEC_PATH=/opt/other/virglrenderer/build/server/virgl_render_server
```

For testing installed layout:

```bash
# Install setup
export DYLD_LIBRARY_PATH=/opt/other/virglrenderer/install/lib:/opt/homebrew/lib:${DYLD_LIBRARY_PATH:-}
export RENDER_SERVER_EXEC_PATH=/opt/other/virglrenderer/install/libexec/virgl_render_server
```

## Using the Build Script

The new `build.sh` script defaults to using `build/`:

```bash
# Standard build
./build.sh

# Clean build from scratch
./build.sh --clean

# Build with debug symbols
./build.sh --debug

# Build with tests enabled
./build.sh --tests

# Use a different build directory
BUILD_DIR=build ./build.sh
```

## Cleanup Suggestion

Since `build/` is now the standard, you can safely remove the duplicate:
```bash
rm -rf builddir  # Keep build/ as the standard build directory
```

Both `build/` and `builddir/` appear to have similar configurations based on their timestamps and structure.
