# Snow

Snow is a stochastic spectral masking effect from NeutronStar714's Pneutrino series. It changes which frequency bins are suppressed over time, creating moving textures from guitar, synths, or other audio. Version 0.0.1 builds as a VST3 plugin and a standalone app.

- **Amount, Mask Width, Cutoff and Grid Size** set the strength, duration, frequency range and smoothness of the mask.
- **Phase Offset and Stereo Decouple** change the character of the mask and how independently it moves in the left and right channels.
- **Dry, Wet and Makeup Gain** set the blend and output level. A live spectrum shows the signal and the mask.

## Build on Windows

Use CMake 3.22 or later and Visual Studio 2022 with the Desktop development with C++ workload.

```powershell
git clone --recurse-submodules https://github.com/NeutronStar714/pneutrino-snow.git
cd pneutrino-snow
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target Snow_VST3 Snow_Standalone
```

Builds are under `build/src/Plugin/Snow_artefacts/Release/`. JUCE 8 and [Signalsmith FFT](https://github.com/Signalsmith-Audio/fft) are pinned submodules. CMake applies two explicit casts to a generated copy of the FFT header for MSVC compatibility. To reuse a matching JUCE checkout, pass `-DPNEUTRINO_JUCE_SOURCE_DIR="C:/path/to/JUCE"` at configure time.
