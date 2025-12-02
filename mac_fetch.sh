#!/bin/bash

set -e

echo "=================================================="
echo "     🟦 MacOS 自动拉取并复制 VLC AAR 文件"
echo "=================================================="

REPO_DIR="$HOME/Code/MoonVR/decoder-sync-MoonVR"
AAR_NAME="libvlc-release.aar"

# 目标路径
DEST1="/Users/guannan15/Code/MoonVR/ExtraPlugins/app/libs/${AAR_NAME}"
DEST2="/Users/guannan15/Code/MoonVR/ExtraPlugins/Common-BaseSupport/libs/${AAR_NAME}"

echo "📌 仓库目录: $REPO_DIR"
cd "$REPO_DIR"

echo "🔄 正在从 GitHub 拉取最新 AAR..."
git pull

# 检查 AAR 是否存在
if [ ! -f "$AAR_NAME" ]; then
    echo "❌ 未找到 AAR 文件: $AAR_NAME"
    echo "请确认 Linux 编译已成功并 push 上传。"
    exit 1
fi

echo "✅ 找到 AAR 文件: $AAR_NAME"

# 创建目标目录
mkdir -p "$(dirname "$DEST1")"
mkdir -p "$(dirname "$DEST2")"

echo "📦 正在复制到目标目录..."

cp -f "$AAR_NAME" "$DEST1"
cp -f "$AAR_NAME" "$DEST2"

echo "=================================================="
echo "     🎉 复制完成！位置如下："
echo "     → $DEST1"
echo "     → $DEST2"
echo "=================================================="