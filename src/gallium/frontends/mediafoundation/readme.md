# DX12 Video Encode Media Foundation Transform (Microsoft Corporation (C) 2025)

## Overview
This folder contains the implementation of an async media foundation transform (MFT) that uses DX12 video encode API via the backend D3D12 pipe objects.

## Files
| Files                               | Description |
| -----                               | ---- |
| codecapi.cpp                        | implements ICodecAPI interface |
| dpb_buffer_manager.cpp              | utility for dbp buffer creation and deletion |
| encode.cpp                          | contains codec agnostic encode related code |
| encoder_capabilities.cpp            | utility for querying hardware capability from pipe |
| encode_av1.cpp                      | encode specific to av1 (currently stub only) |
| encode_h264.cpp                     | encode specific to h264 |
| encode_hevc.cpp                     | encode specific to hevc |
| hmft_entrypoints.cpp                | entry point to hmft |
| mfbufferhelp.cpp                    | utility for mf buffer |
| mfd3dmanager.cpp                    | utility for managing d3d device |
| mfmediaeventgenerator.cpp           | implements the IMFMediaEventGenerator interface |
| mfpipeinterop.cpp                   | utility for various type translation function between MF and pipe |
| mfrealtimeclientex.cpp              | implements the IMFRealTimeClientEx interface|
| mfshutdown.cpp                      | implements the IMFShutdown interface |
| mftransform.cpp                     | implements the IMFTransform interface |
| reference_frames_tracker_av1.cpp    | managing dbp and gop state for av1 (currently stub only)|
| reference_frames_tracker_h264.cpp   | managing dbp and gop state for h264 |
| reference_frames_tracker_hevc.cpp   | managing dbp and gop state for hevc |
| videobufferlock.cpp                 | utility for video buffer lock |
| wpptrace.cpp                        | wpp trace |
| wppconfig/                          | folder containing wpp trace config |
| wil/                                | folder containing the Windows Implementation Library (https://github.com/microsoft/wil) |
| IDL/                                | folder containing the IDL files for the MFT|
| test/                               | folder containing the test code for the MFT|

Other files related to the MFT:
| Files                               | Description |
| -----                               | ---- |
| src/gallium/targets/mediafoundation | dllmain and resourc files for MFT, builds the dll |

## Build
Current implementation is designed to build three differt codec (AV1, H264, HEVC) with the same code (note AV1 is stub right now).  To build each codec, we run setup with different codec name.

To build the COM based H264 codec, you can use the following command to setup the build (please run the setup under the mesa folder, and by default the output will go to c:\bin)

```
meson setup build/ --pkg-config-path="C:\lib\pkgconfig" -Dgallium-d3d12-graphics=disabled -Dintel-elk=false -Dmicrosoft-clc=disabled -Dllvm=disabled -Dvalgrind=disabled -Dlmsensors=disabled -Dzlib=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dgbm=disabled -Dglx-direct=false -Denable-glcpp-tests=false -Dopengl=false -Dgallium-drivers=d3d12 -Dgallium-xa=disabled -Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-mediafoundation=enabled -Dc_args="/guard:cf /we4146 /we4308 /we4509 /we4510 /we4532 /we4533 /we4610 /we4700 /we4789 /wd4703" -Dc_link_args="/guard:cf /profile /DYNAMICBASE" -Dcpp_args="/guard:cf /we4146 /we4308 /we4509 /we4510 /we4532 /we4533 /we4610 /we4700 /we4789 /wd4703" -Dcpp_link_args="/guard:cf /profile /DYNAMICBASE" --default-library=static -Db_vscrt=mt -Dc_std=c17 -Dcpp_std=c++17 -Dmicrosoft-clc=disabled -Ddebug=false --buildtype=release -Dwarning_level=2 -Dvideo-codecs=h264enc -Dmediafoundation-windows-dll-name="msh264enchmft" -Dmediafoundation-store-dll=false -Dgallium-mediafoundation-test=true
```

To build the COM based HEVC codec, use the above command but replace all instances of h264enc with h265enc to setup the build.  Similarly for the pending AV1 codec, replace h264enc with av1enc.

For debug builds, change the debug/buildtype to true, e.g.

```
-Ddebug=true --buildtype=true
```

For building the unit test, add the following option to the meson setup.

```
-Dgallium-mediafoundation-test=true
```

To build, run the following command from the mesa folder.

```
ninja -C build/ install
```