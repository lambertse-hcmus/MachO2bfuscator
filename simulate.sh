#!/usr/bin/env bash

echo "[INFO] Starting build..."

echo "[INFO] Running build.sh -debug"
./build.sh -debug
if [[ $? -ne 0 ]]; then
    echo "[ERROR] build.sh failed"
    exit 1
fi
cd ~/Documents/tesckey/

echo "[INFO] Running MachO2bfuscator..."
~/src/opensource/lambertse/MachO2bfuscator/build/MachO2bfuscator -o Payload/testckey_objc.app/testckey_objc original_testckey_objc --verbose \
    --
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
rm -f ~/Documents/tesckey/testckey_objc.ipa

echo "[INFO] Creating new IPA..."
zip -r testckey_objc.ipa Payload
if [[ $? -ne 0 ]]; then
    echo "[ERROR] zip failed"
    exit 1
fi

echo "[INFO] Done!"
