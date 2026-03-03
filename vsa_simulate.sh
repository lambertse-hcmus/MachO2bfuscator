#!/usr/bin/env bash

echo "[INFO] Starting build..."

echo "[INFO] Running build.sh -debug"
./build.sh -debug
if [[ $? -ne 0 ]]; then
    echo "[ERROR] build.sh failed"
    exit 1
fi
cd ~/Documents/ipa

echo "[INFO] Running MachO2bfuscator..."
~/src/opensource/lambertse/MachO2bfuscator/build/MachO2bfuscator -o Payload/vkeyapp_prod_release.app/vkeyapp_prod_release origin_vkeyapp_prod_release --verbose \
    --class-filter-file=class_filter.txt \
    --selector-filter-file=selector_filter.txt 
    # --dependency="/Users/tri.le/Downloads/libobjc.A.dylib" \
    # --dependency="/Users/tri.le/Downloads/UIKit" \
    # --dependency="/Users/tri.le/Downloads/UIKitCore"
# --blacklist-class=AppDelegate,ViewController,SceneDelegate \
    # --dependency /System/Library/Frameworks/Foundation.framework/Foundation \
    # --dependency /System/Library/Frameworks/UIKit.framework/UIKit 


if [[ $? -ne 0 ]]; then
    echo "[ERROR] MachO2bfuscator failed"
    exit 1
fi

echo "[INFO] Removing old IPA (if exists)..."
rm -f ~/Documents/ipa/vkeyapp_sandbox_release.ipa

echo "[INFO] Creating new IPA..."
zip -r vkeyapp_sandbox_release.ipa Payload Signatures SwiftSupport Symbols
if [[ $? -ne 0 ]]; then
    echo "[ERROR] zip failed"
    exit 1
fi

echo "[INFO] Done!"

