handover (unofficial) reimplemation

## usage

`./gradlew build` to build, `./gradlew installDebug` to install.

To enable Vulkan validation layer, execute `adb shell setprop debug.oculus.loadandinjectpackagedvvl.net.watasuke.handover 1`

To retrieve logs, the recommennded command is `clear; adb logcat -c && adb logcat -s HandOver OpenXR-Loader Vulkan DEBUG VALIDATION`
