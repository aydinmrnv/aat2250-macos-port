# ocvalidate run
Binary: src/dl-opencore/1.0.8/extracted/Utilities/ocvalidate/ocvalidate.exe sha256 f459053cefdf1b2ff8c1d74d61d9dc5bb30ce72a4fcbf15bb29333e77d8d045f
From OpenCore-1.0.8-RELEASE.zip sha256 2011e8b7216ecb2645d97ea710df965947edd406846e92050b9bc4190be6d27b (matches GitHub release API digest)
Control: stock Docs/Sample.plist -> 'No issues found', exit 0
Run date 2026-11-02, cwd deliverables/a2-platform, command: ocvalidate.exe config.draft.plist
```

NOTE: This version of ocvalidate is only compatible with OpenCore version 1.0.8!


PlatformInfo->Generic->SystemUUID is borked (Can only be empty, special string OEM or valid UUID)!
CheckPlatformInfo returns 1 error!

Completed validating config.draft.plist in 1 ms. Found 1 issue requiring attention.
exit=1
```
