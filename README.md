# RIST Stale Frame Fix for OBS

Standalone OBS Studio video filter for network media sources that retain their last decoded
frame after the sender disappears. The filter becomes transparent after a configurable period
without a genuinely new asynchronous video frame and passes the source through immediately when
fresh frames resume.

## Installation

Copy the contents of the packaged `obs-studio` directory into the OBS Studio installation
directory. With the default Windows installation, the destination is:

```text
C:\Program Files\obs-studio
```

The package adds only these paths:

```text
obs-plugins\64bit\obs-rist-stale-frame-filter.dll
data\obs-plugins\obs-rist-stale-frame-filter\locale\en-US.ini
data\obs-plugins\obs-rist-stale-frame-filter\locale\de-DE.ini
```

## Use

1. Keep the existing OBS Media Source with its `rist://` input.
2. Open that source's **Filters** dialog.
3. Add the **RIST Stale Frame Fix** effect filter.
4. Leave the delay at 1000 ms or choose another value. A value of 0 disables hiding.

The filter does not restart or disconnect the Media Source. It only stops drawing an obsolete
frame, so sources below it in the scene remain visible. The first fresh frame makes it visible
again.

## Compatibility

The packaged Windows x64 build targets OBS Studio 32.2.1. Rebuild the plugin against the matching
libobs SDK after an incompatible OBS ABI change.
