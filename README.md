# surf

A streaming music server for your personal collection.

This tool is retired. I switched to a subscription service and don't maintain a music library anymore.

## Build Instructions

Please install the following dependencies:

 * ffmpeg development libraries
 * a platform-specific implementation of [highwayhash](https://github.com/google/highwayhash)
 * CMake 3.11 or higher
 All other dependencies will be downloaded and built locally by CMake.

After setting this up, you can launch a CMake build that will generate an executable target called `surf`.

## Usage
Just launch the executable from a terminal window. You can set options in a configuration file, which can be found at one of the following locations:
 * Windows: `%APPDATA%\trao1011\surf\config.ini`
 * Linux: `~/config/trao1011/surf/config.ini`

Below is a list of settable properties. You may only need to care about the first couple settings.
 * The media directory (default: your platform-specific Music folder), in the configuration file at `[media].path` or the environment variable `SURF_MEDIA`
 * The server port (default: 27440), in the configuration file at `[net].port` or the environment variable `SURF_PORT`
 * The maximum cache size (default: 64), in the configuration file at `[media].cache_size` or the environment variable `SURF_MAX_CACHE`

For now, look at `spec.txt` for an unpolished description of the endpoints supported by the server.

## Recommendations
 * Your music library should be tagged with [MusicBrainz Picard](https://picard.musicbrainz.org/) for best results, as this application will scan the tags generated by this application.
 * Please avoid embedding cover art in music files; instead, place it in the media files' folder with the name `coverart` or `folder`.
