#!/bin/bash
set -e

# 你的 Mac 编译生成的库路径
DECODER_SRC="/Users/guannan15/Code/MoonVR/1125send/JM/lib/libxavc3ddecoder.a"

# wrapper 路径
WRAPPER_SRC="/Users/guannan15/Code/MoonVR/1125send/avc3ddecoder.c"

# 复制到仓库根目录
echo "➤ Copy decoder .a..."
cp "$DECODER_SRC" "./libxavc3ddecoder.a"

echo "➤ Copy wrapper .c..."
cp "$WRAPPER_SRC" "./avc3ddecoder.c"

# Git 操作
echo "➤ Git commit & push..."
git add libxavc3ddecoder.a avc3ddecoder.c
git commit -m "update decoder & wrapper"
git push

echo "✓ Done: Synced to GitHub"
