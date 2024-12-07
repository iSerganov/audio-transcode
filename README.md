# Audio transcoding
---
A basic example of audio transcoding using libAV.

### Usage

1. Build: `g++ transcode.cpp -I<path_to_ffmpeg_include_folder> -L<path_to_ffmpeg_lig_folder> -lavformat -lavcodec -lavutil`
2. Run `./transcode` binary.