#!/bin/bash
set -e

# === 配置区 ===

# 你的 VLC-Android 路径（按你 Linux 实际目录改一下）
VLC_ANDROID_DIR="$HOME/workbench/vlc-android"

# 本 Git 仓库目录（脚本所在目录）
REPO_DIR="$(cd "$(dirname "$0")" && pwd)"

# 需要复制到 VLC 的两个文件
DECODER_LIB="libxavc3ddecoder.a"
WRAPPER_FILE="avc3ddecoder.c"   # ← 你实际的名字替换这个

# 文件在 VLC-Android 中的存放位置（根据你项目调整）
VLC_DECODER_DIR="$VLC_ANDROID_DIR/extras/decoder"
VLC_WRAPPER_DIR="$VLC_ANDROID_DIR/extras/decoder"

# VLC 输出 AAR 的位置（请改成你真实的路径）
AAR_OUTPUT="$VLC_ANDROID_DIR/vlc-android/app/build/outputs/aar/app-release.aar"

# 回传 AAR 到 GitHub 仓库的文件名
OUTPUT_AAR_NAME="libvlc-release.aar"


echo "=============================================="
echo "   1. 复制 decoder + wrapper 文件到 VLC-Android"
echo "=============================================="

mkdir -p "$VLC_DECODER_DIR"

cp "$REPO_DIR/$DECODER_LIB" "$VLC_DECODER_DIR/" 
cp "$REPO_DIR/$WRAPPER_FILE" "$VLC_WRAPPER_DIR/"

echo "复制完成！"


echo "=============================================="
echo "          2. 开始编译 VLC Android"
echo "=============================================="

cd "$VLC_ANDROID_DIR"

# 你把下面这行换成你真实的 VLC 构建命令
./buildsystem/compile.sh

echo "VLC 编译完成！"


echo "=============================================="
echo "      3. 获取 AAR 并复制回 GitHub 仓库"
echo "=============================================="

if [ ! -f "$AAR_OUTPUT" ]; then
    echo "❌ 找不到 AAR 文件！路径：$AAR_OUTPUT"
    exit 1
fi

cp "$AAR_OUTPUT" "$REPO_DIR/$OUTPUT_AAR_NAME"

echo "AAR 已复制到仓库：$OUTPUT_AAR_NAME"


echo "=============================================="
echo "             4. 推送回 GitHub"
echo "=============================================="

cd "$REPO_DIR"

git add .
git commit -m "Linux 构建完成 $(date +"%Y-%m-%d %H:%M:%S")"
git push origin main

echo "推送成功！流程全部完成！"
echo "=============================================="