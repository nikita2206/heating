#!/bin/bash
# Build script for OpenTherm Gateway firmware
# Builds web UI (if needed) and ESP-IDF firmware

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WEB_UI_DIR="$SCRIPT_DIR/web-ui"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

print_step() {
    echo -e "${GREEN}==>${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}Warning:${NC} $1"
}

print_error() {
    echo -e "${RED}Error:${NC} $1"
}

# Parse arguments
SKIP_WEB=false
FORCE_WEB=false
CLEAN=false

for arg in "$@"; do
    case $arg in
        --skip-web)
            SKIP_WEB=true
            ;;
        --force-web)
            FORCE_WEB=true
            ;;
        --clean)
            CLEAN=true
            ;;
        --help|-h)
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --skip-web   Skip web UI build (firmware only, web UI must exist)"
            echo "  --force-web  Force rebuild web UI even if dist/ is up to date"
            echo "  --clean      Clean build artifacts before building"
            echo "  --help, -h   Show this help message"
            echo ""
            echo "Examples:"
            echo "  $0              # Build web UI (if needed) and firmware"
            echo "  $0 --force-web  # Force rebuild web UI, then firmware"
            echo "  $0 --skip-web   # Build firmware only"
            echo "  $0 --clean      # Clean and rebuild everything"
            exit 0
            ;;
        *)
            print_error "Unknown option: $arg"
            exit 1
            ;;
    esac
done

cd "$SCRIPT_DIR"

# Check if web UI needs building
WEB_UI_DIST="$WEB_UI_DIR/dist"
WEB_UI_MISSING=false
if [[ ! -f "$WEB_UI_DIST/index.html.gz" ]] || [[ ! -f "$WEB_UI_DIST/assets/index.js.gz" ]] || [[ ! -f "$WEB_UI_DIST/assets/index.css.gz" ]]; then
    WEB_UI_MISSING=true
    if [[ "$SKIP_WEB" == true ]]; then
        print_error "Web UI build output not found!"
        echo "Remove --skip-web flag or build manually:"
        echo "  cd $WEB_UI_DIR && npm install && npm run build"
        exit 1
    fi
fi

# Build web UI
if [[ "$SKIP_WEB" == false ]]; then
    if [[ "$FORCE_WEB" == true ]] || [[ "$WEB_UI_MISSING" == true ]]; then
        print_step "Building web UI..."
        cd "$WEB_UI_DIR"

        # Install dependencies if needed
        if [[ ! -d "node_modules" ]]; then
            print_step "Installing npm dependencies..."
            npm install
        fi

        npm run build
        cd "$SCRIPT_DIR"
        echo ""
    else
        print_step "Web UI up to date (use --force-web to rebuild)"
    fi
fi

# Source ESP-IDF environment if not already set
if [[ -z "$IDF_PATH" ]]; then
    print_step "Sourcing ESP-IDF environment..."
    if [[ -f "$HOME/esp/v5.5.1/esp-idf/export.sh" ]]; then
        source "$HOME/esp/v5.5.1/esp-idf/export.sh"
    elif [[ -f "$HOME/esp/esp-idf/export.sh" ]]; then
        source "$HOME/esp/esp-idf/export.sh"
    else
        print_error "ESP-IDF not found. Please set IDF_PATH or install ESP-IDF."
        exit 1
    fi
fi

# Clean if requested
if [[ "$CLEAN" == true ]]; then
    print_step "Cleaning build artifacts..."
    idf.py fullclean
fi

# Build firmware
print_step "Building ESP-IDF firmware..."
idf.py build

echo ""
print_step "Build complete!"
echo "  Firmware: $SCRIPT_DIR/build/opentherm_gateway.bin"
echo ""
echo "To flash: idf.py flash"
echo "To monitor: idf.py monitor"
