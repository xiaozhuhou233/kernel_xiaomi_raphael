#!/usr/bin/env bash
set -euo pipefail

KERNEL_IMAGE="${1:-out/arch/arm64/boot/Image.gz-dtb}"
DTBO_IMAGE="${2:-out/arch/arm64/boot/dtbo.img}"
OUTPUT_ZIP="${3:-out/raphael-90hz-ksunext-ak3.zip}"

AK3_REPO="https://github.com/osm0sis/AnyKernel3.git"
AK3_COMMIT="e4b1bb25ca2aabcfd57f694a5998d87130701b71"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

test -s "$KERNEL_IMAGE"
test -s "$DTBO_IMAGE"

git clone --quiet --filter=blob:none "$AK3_REPO" "$STAGE/AnyKernel3"
git -C "$STAGE/AnyKernel3" checkout --quiet "$AK3_COMMIT"
rm -rf "$STAGE/AnyKernel3/.git" \
       "$STAGE/AnyKernel3/.github" \
       "$STAGE/AnyKernel3/README.md"

cat > "$STAGE/AnyKernel3/anykernel.sh" <<'AK3'
### AnyKernel3 Ramdisk Mod Script
## Raphael 60/90 Hz + KernelSU Next

properties() { '
kernel.string=Raphael 60/90Hz KernelSU Next
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=raphael
device.name2=raphaelin
device.name3=
device.name4=
device.name5=
supported.versions=
supported.patchlevels=
supported.vendorpatchlevels=
'; }

boot_attributes() {
set_perm_recursive 0 0 755 644 $RAMDISK/*;
set_perm_recursive 0 0 750 750 $RAMDISK/init* $RAMDISK/sbin;
}

BLOCK=/dev/block/bootdevice/by-name/boot;
IS_SLOT_DEVICE=0;
RAMDISK_COMPRESSION=auto;
PATCH_VBMETA_FLAG=auto;

. tools/ak3-core.sh;

dump_boot;
flash_dtbo;
write_boot;
AK3

cp "$KERNEL_IMAGE" "$STAGE/AnyKernel3/Image.gz-dtb"
cp "$DTBO_IMAGE" "$STAGE/AnyKernel3/dtbo.img"

mkdir -p "$(dirname "$OUTPUT_ZIP")"
rm -f "$OUTPUT_ZIP"
(
  cd "$STAGE/AnyKernel3"
  zip -q -r "$OLDPWD/$OUTPUT_ZIP" .
)

unzip -t "$OUTPUT_ZIP"
unzip -l "$OUTPUT_ZIP" | grep -E 'Image.gz-dtb|dtbo.img|anykernel.sh|tools/ak3-core.sh'
sha256sum "$OUTPUT_ZIP" | tee "$OUTPUT_ZIP.sha256"
