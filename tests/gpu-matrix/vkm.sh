#!/bin/sh
# vkmark headless, old vs patched driver
for I in "VK_ICD_FILENAMES=/root/new_icd.json"; do
  echo "### ${I:-old}"
  timeout 400 chroot /tmp/edge /usr/bin/env PVR_I_WANT_A_BROKEN_VULKAN_DRIVER=1 $I vkmark --winsys headless -s 1280x720 2>&1 | grep -E "FPS|Score|rror"
done
echo DONE
