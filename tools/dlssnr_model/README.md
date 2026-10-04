# DLSS 5 neural rendering model

`nvngx_dlssnr.dll` (NGX feature 18, from NVIDIA driver packages) is 165 MB, over GitHub's 100 MB file limit, and this repository does not use LFS. It is stored here as a multi-volume 7-Zip archive, each volume under 50 MB.

`cmake/DlssNRModel.cmake` extracts it at configure time into `features/Upscaling/Shaders/Upscaling/Streamline/`, where packaging picks it up. The extracted DLL is git-ignored.

To update the model:

```
cd features/Upscaling/Shaders/Upscaling/Streamline
7z a -t7z -mx=5 -v45m ../../../../../tools/dlssnr_model/nvngx_dlssnr.7z nvngx_dlssnr.dll
```

Delete the old volumes first if the new archive has fewer of them.
