# obs-rist-media-source

Native OBS Studio input source for receiving MPEG-TS over RIST. The plugin uses its own statically
linked libRIST 0.2.20 and hands the received transport stream to OBS for demuxing, audio/video
decoding, synchronization, and optional hardware decoding.

## Why does this plugin exist?

I created this because OBS Studio does not currently seem particularly interested in fixing this
fairly straightforward problem at its source: keeping its bundled libRIST integration up to date.
If you would rather not need a workaround plugin for this, feel free to open a ticket at
[OBS Studio](https://github.com/obsproject/obs-studio) and politely ask them to update the library.
It really should be that simple.

## Features

- Dedicated **RIST Media Source** in OBS
- Audio output through the OBS mixer with normal monitoring and track routing
- Simple, Main, and Advanced RIST profiles
- Optional SRP authentication, enabled through a single **Authentication** checkbox
- Configurable recovery buffer and reconnect delay
- Last-frame timeout in milliseconds; `0` disables it
- Transparent output after a timeout and automatic recovery on the next decoded frame
- Optional hardware decoding through OBS
- Optional encryption, enabled through a single **Encryption** checkbox, with AES-128 or AES-256
- No verbose status logging in the source properties

## Download

Download the latest version from [GitHub Releases](https://github.com/romestylez/obs-rist-media-source/releases/latest).

## Windows installation

The Windows x64 installer and ZIP are built against OBS Studio 32.2.1 and support the OBS 32.2.x
series, including the tested OBS 32.2.2 release. The installer automatically suggests a detected
OBS installation, but always lets you browse to a different OBS root directory. It
validates the selected directory by checking for `bin\64bit\obs64.exe` before copying any files.

For manual installation, extract the ZIP's `obs-studio` directory over the OBS Studio installation
directory. The default destination is:

```text
C:\Program Files\obs-studio
```

No separate libRIST DLL is required. The installer's uninstaller removes only files owned by
obs-rist-media-source.

## Linux installation

The Linux x86_64 archives target a natively installed OBS Studio 32.2.1. Extract the archive and
copy its `lib` and `share` directories into the matching installation prefix, normally `/usr` or
`/usr/local`:

```text
lib/obs-plugins/obs-rist-media-source.so
share/obs/obs-plugins/obs-rist-media-source/locale/
```

The Linux build is intended for native installations. macOS and Flatpak are not currently
supported.

## Use

1. Add a new **RIST Media Source** to a scene.
2. Enter the RIST URL and choose the required profile.
3. Configure the recovery buffer, last-frame timeout, reconnect delay, and hardware decoding.
4. For Main or Advanced profile, enable **Authentication** to reveal username and password.
5. Enable **Encryption** to reveal the encryption secret and AES-128/AES-256 selection.
6. Assign the source to the required recording or streaming tracks in **Advanced Audio Properties**.

Audio is sent through the normal OBS mixer. Track assignment controls recordings and streams.
To hear the source locally, set **Audio Monitoring** to **Monitor and Output** in OBS.


Authentication and encryption are disabled by default. Simple profile does not support either
option, so their controls and detail fields are disabled or hidden. If the sender stops delivering
decodable video, the source becomes transparent after the configured timeout and becomes visible
again automatically with the next decoded video frame.

## Credits

Thanks to [moo-the-cow](https://github.com/moo-the-cow/moo-rist-hosting-native/tree/main/obs) for
documenting the underlying OBS/libRIST problem and publishing an updated libRIST DLL workaround,
which helped inspire this project. This plugin takes a separate approach by bundling libRIST
privately instead of replacing OBS Studio's own `librist.dll`, which may simply be overwritten
again by the next OBS update.

## Support

For help and support, join the [Discord server](https://discord.gg/WwRCPCTez9).
