QMapShack (QMS) build process for Mac

The build process relies on a fixed local build environment.
The root directory of the local build environment must be empty and referenced as $QMSDEVDIR

This directory contains sub-directories while building:

- _routino\*_ : for downloading and building Routino
- _proj_ : for downloading and building PROJ
- _gdal_ : for downloading and building GDAL
- _local_ : contains all files and directories installed by locally built packages
- _qmapshack_ : QMS source downloaded from git
- _build_qmapshack_ : directory were the QMS build process itself happens
- _release_ : contains the created app bundles

Local packages to be built are:

- Routino
- GDAL : if _config.sh_ contains `BUILD_GDAL="x"`
- PROJ : if _config.sh_ contains `BUILD_PROJ="x"`


All other packages are taken from _HomeBrew_ package manager.  
The _HomeBrew_ package manager is the package manager of choice.

The package manager _MacPorts_ is also supported.  
All packages are taken from _MacPorts_ and no local packages are built.

Important script variables:

- QMSDEVDIR (mandatory)  
the build environment
- BUILD_RELEASE_DIR (optional)  
the location where the app bundles are created

Parameters to configure build:

- XCODE_PROJECT (optional) : Can be set with `-x` on the command line  
If set, create an XCode project instead of building QMS
- BREW_PACKAGE_BUILD : Can be set with `-b` on the command line  
If set, creates QMS as an app relying on _HomeBrew_ packages on runtime  
_HomeBrew_ packages are listed in _install-brew-packages.sh_
- MACPORTS_BUILD : Can be set with `-m` on the command line  
If set, all packages are taken from _MacPorts_ and no local packages are built
- BUILD_GDAL (optional) : Can be set with `-g` on command line  
If set, build GDAL instead of using the _HomeBrew_ package

To run the complete build process:

1. Create a directory and cd into this directory.  
This directory will be referenced as $QMSDEVDIR
2. Clone git repository https://github.com/Maproom/qmapshack.git
3. Check build parameters in _./qmapshack/MacOSX/config.sh_
4. Run `sh ./qmapshack/MacOSX/build-all.sh | tee log.txt`
5. ATTENTION: Manual intervention is needed for applying _admin_ password while changing dylibs (Apple requirement)
6. Check _log.txt_ if an error occurred
7. After successful built, the app bundles are located in the _release_ folder
8. Check _brew\*.diff_ for packages installed by _HomeBrew_ during build process  
and uninstall them if not needed anymore.

---

## Experimental native MTP transfers

Native Garmin MTP access can be enabled with the CMake option `-DUSE_LIBMTP=ON`.
Install `libmtp` and `pkg-config` with Homebrew before configuring. The option is
off by default and does not require GNOME, KDE, D-Bus, a filesystem mount, or a
kernel extension.

Connected Garmin devices appear in the workspace with an `[MTP, GPX upload]`
suffix. Activities and courses are downloaded asynchronously and can be copied
into workspace projects using the existing device interface.

To export a track or route, drag its workspace project onto the device or use
the project's `Send to Devices` action. Each upload requires confirmation.
Only GPX files directly inside `Garmin/NewFiles` are permitted, with a 16 MiB
upload limit and at least two valid track or route points. Filenames are limited
to 124 ASCII letters, digits, underscores, dots and hyphens, including `.gpx`.
Directory contents are checked afresh; existing names are rejected without
regard to case. Uploaded files are downloaded again and checked with SHA-256
before success is reported. Disconnect the device afterward and check its
Courses list once it has processed the GPX.

Automatic synchronization, replacement and device-side deletion are disabled
for the native backend. Use a new workspace project with a unique name to send
another copy of a project already on the device. A failed or interrupted transfer
may leave an incomplete file in NewFiles; inspect it before retrying. The backend
does not delete files automatically.

USB devices are polled every five seconds. Close Garmin Express, OpenMTP and
other MTP clients before starting QMapShack. Disconnecting cancels pending reads
and removes the device entry. Reconnecting starts a new session and refreshes
the directory cache. Only the first storage containing `GarminDevice.xml` is
currently supported, with a 256 MiB limit per downloaded file.

Homebrew-linked local bundles need `libmtp` and `libusb` installed at runtime.
Standalone distributions must additionally deploy those libraries and their
dependencies; the standard standalone bundling scripts are not yet extended.

Contents of this folder _MacOSX_

Folders:

- _archive_ : Outdated scripts to be deleted soon
- _resources_ : Resources like icons, info.plist specifically needed for macOS

Scripts for the overall build process:

- _build-all.sh_ : Automatically builds QMapShack. (calls sub build scripts, more modular)  
The only manual intervention is to pass the _admin_ password for changing paths in dylibs

Scripts for partial steps of the build process:

- _install-brew-packages.sh_ : Installs _HomeBrew_ (if needed) and packages for the build process.  
Package files are also copied to local environment
- _build-routino.sh_ : Builds Routino
- _build-gdal.sh_ : Builds GDAL
- _build-proj.sh_ : Builds PROJ
- _build-otoolrecursive.sh_ : Builds _otoolrecursive_, a tool for recursively chasing down dylibs dependencies
- _build-QMS.sh_ : Compiles QMapShack
- _bundle-all.sh_ : The complete bundling process (calls (in)directly the other bundle scripts).  
Should be called, when everything is built but not yet bundled (_build-all.sh_ includes this script).
- _bundle-qmapshack.sh_ : Bundles the app QMapShack
- _bundle-qmaptool.sh_ : Bundles the app QMapTool
- _bundle.sh_ : Bundles and signs QMapShack
- _config.sh_ : Checks for a valid build directory and contains the variables driving the build process
- _clean.sh_ : Cleans all build artifacts, except for _brew\*diff.txt_ (which lists _HomeBrew_ packages installed for the build process)
- _create_local_env_ : Creates a local environment where all external libs/packages can be saved  
Idea: The libs can be downloaded via package managers and copied
  or directly downloaded from the internet.
From now on, subsequent build processes are based on independent libraries distributed across the file system. Build processes, like bundling still need to be adapted (WiP).
- _install-packages.sh_ : Installs packages for the build process
