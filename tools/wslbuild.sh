#!/bin/bash
# wslbuild.sh — 在 WSL 原生文件系统构建 ESP32-C6 固件（规避中文路径+9p 坑，见 ERR-001）
# 注意：不能用 source activate_idf_v6.0.2.sh —— 其 is_sourced 检测 $0，
#       从脚本文件 source 时 $0=脚本名 → 误判为执行 → exit 1 连带退出本 shell（ERR-002）。
#       改用 -e 模式导出环境变量。
# 用法: 把本脚本拷到 /tmp 后执行: bash /tmp/wb.sh [flash|monitor|clean]
set -e
# -e 输出 KEY=VALUE 行，但 PATH 值含括号/空格不能 eval；管道子 shell 中 export 失效，
# 所以用主进程 while + 进程替换逐行 export。
while IFS='=' read -r k v; do
    case "$k" in
        PATH) export PATH="$v:$PATH" ;;
        IDF_TOOLS_PATH|IDF_PATH|ESP_ROM_ELF_DIR|OPENOCD_SCRIPTS|IDF_PYTHON_ENV_PATH|ESP_CLANG_LIBS_PATH|IDF_COMPONENT_LOCAL_STORAGE_URL|ESP_IDF_VERSION) export "$k=$v" ;;
    esac
done < <(bash /root/.espressif/tools/activate_idf_v6.0.2.sh -e)
IDFPY="$IDF_PYTHON_ENV_PATH/bin/python $IDF_PATH/tools/idf.py"

WIN_DIR="/mnt/f/project/嵌入式项目/esp32c6ota/firmware"
LNX_DIR="$HOME/c6src/firmware"

# 1. 同步源码 → WSL 原生（排除构建生成物）
mkdir -p "$(dirname "$LNX_DIR")"
rsync -a --delete --exclude 'build/' --exclude 'sdkconfig.old' --exclude 'dependencies.lock' \
      "$WIN_DIR/" "$LNX_DIR/"

cd "$LNX_DIR"

# 2. 首次构建时设置 target
if [ ! -f sdkconfig ] || ! grep -q 'CONFIG_IDF_TARGET="esp32c6"' sdkconfig 2>/dev/null; then
    $IDFPY set-target esp32c6 >/dev/null
fi

# 3. 执行动作
case "$1" in
    flash)   $IDFPY -p /dev/ttyACM0 build flash 2>&1 | tail -25 ;;
    monitor) $IDFPY -p /dev/ttyACM0 monitor ;;
    clean)   $IDFPY fullclean ;;
    *)       $IDFPY build 2>&1 | tail -10 ;;
esac

# 4. 产物回拷到 Windows 项目目录
mkdir -p "$WIN_DIR/build"
cp -f build/*.bin build/*.elf build/*.map "$WIN_DIR/build/" 2>/dev/null || true
cp -f build/flasher_args.json "$WIN_DIR/build/" 2>/dev/null || true
cp -f build/partition_table/partition-table.bin "$WIN_DIR/build/partition-table.bin" 2>/dev/null || true
cp -f build/bootloader/bootloader.bin "$WIN_DIR/build/bootloader.bin" 2>/dev/null || true
cp -f build/sdkconfig "$WIN_DIR/build/sdkconfig" 2>/dev/null || true
echo "=== artifacts synced back to Windows dir ==="
