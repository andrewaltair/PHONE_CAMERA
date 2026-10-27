# PHONE_CAMERA native activation test

This is an independent, non-privileged Windows test. It does not include the
product's GUID header and never calls `DllRegisterServer`, `DllUnregisterServer`,
`MFCreateVirtualCamera`, `regsvr32`, an installer, a driver API, or a certificate
operation. It only uses `LoadLibraryW`, `GetProcAddress("DllGetClassObject")`,
COM class-factory calls, and Media Foundation/DirectShow interface calls.

The test fixes the new product identities in its own source:

- MF virtual-camera server: `A43186E6-3C6E-4FC2-8E62-7193B5AA0FF3`
- DirectShow server: `F1CAA121-F8A7-4DFB-BEAB-1DED6A7887EA`

`--mode pass` loads both supplied DLLs and verifies:

- the MF class factory accepts the new CLSID and creates `IMFActivate`;
- `IMFActivate` creates an `IMFMediaSource`;
- its presentation descriptor contains a selected Video/NV12 stream and a
  current Video/NV12 media type;
- the DirectShow class factory accepts the new CLSID, creates `IBaseFilter`,
  reports the same CLSID through `IPersist`, and exposes an output `Capture`
  pin whose first media type is Video/NV12.

The MF check deliberately stops before `IMFMediaSource::Start` or sample
requests. Activation builds the in-process descriptor and event objects, but it
does not start a delivery thread or request a phone frame. The DirectShow check
also only enumerates the filter and pin; it does not run a graph.

`--mode red` is for the old MyCam baseline. It requires both DLL files to exist,
then proves that each loaded DLL rejects the new CLSID at
`DllGetClassObject`. A missing DLL returns exit code `2` and is explicitly not
treated as red evidence.

## Build

From the repository root, with the VS 2022 x64 generator:

```powershell
cmake -S tests/native-activation -B build/native-activation -G 'Visual Studio 17 2022' -A x64
cmake --build build/native-activation --config Release --target phone_camera_native_activation_test
```

## Test the new build

After building the native application, run:

```powershell
& 'build/native-activation/Release/phone_camera_native_activation_test.exe' `
  --mode pass `
  --mf (Resolve-Path 'native/phone-camera/pc/build/Release/MyCamVCam.dll') `
  --dshow (Resolve-Path 'native/phone-camera/pc/build/Release/MyCamVCamDShow.dll')
```

To check an older MyCam build, pass its two DLL paths with `--mode red`; the
test succeeds only when both old DLLs load and reject the PHONE_CAMERA CLSIDs.
