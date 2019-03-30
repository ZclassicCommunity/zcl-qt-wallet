ZclWallet is a z-Addr first, Sapling compatible wallet and full node for zclassicd that runs on Linux, Windows and macOS.

![Screenshot](docs/screenshot-main.png?raw=true)
![Screenshots](docs/screenshot-sub.png?raw=true)
# Installation

Head over to the releases page and grab the latest installers or binary. https://github.com/ZClassicFoundation/zclwallet/releases

### Linux

If you are on Debian/Ubuntu, please download the `.deb` package and install it.
```
sudo dpkg -i linux-deb-zclwallet-v0.6.3.deb
sudo apt install -f
```

Or you can download and run the binaries directly.
```
tar -xvf zclwallet-v0.6.3.tar.gz
./zclwallet-v0.6.3/zclwallet
```

### Windows
Download and run the `.msi` installer and follow the prompts. Alternately, you can download the release binary, unzip it and double click on `zclwallet.exe` to start.

### macOS
Double-click on the `.dmg` file to open it, and drag `zclwallet` on to the Applications link to install.

## zclassicd
ZclWallet needs a ZClassic node running zclassicd. If you already have a zclassicd node running, ZclWallet will connect to it. 

If you don't have one, ZclWallet will start its embedded zclassicd node. 

Additionally, if this is the first time you're running ZclWallet or a zclassicd daemon, ZclWallet will download the zclassic params (~1.7 GB) and configure `zclassic.conf` for you. 

Pass `--no-embedded` to disable the embedded zclassicd and force ZclWallet to connect to an external node.

## Compiling from source
ZclWallet is written in C++ 14, and can be compiled with g++/clang++/visual c++. It also depends on Qt5, which you can get from [here](https://www.qt.io/download). Note that if you are compiling from source, you won't get the embedded zclassicd by default. You can either run an external zclassicd, or compile zclassicd as well. 

See detailed build instructions [on the wiki](https://github.com/ZClassicFoundation/zclwallet/wiki/Compiling-from-source-code)

### Building on Linux

```
git clone https://github.com/ZClassicFoundation/zclwallet.git
cd zclwallet
/path/to/qt5/bin/qmake zcl-qt-wallet.pro CONFIG+=debug
make -j$(nproc)

./zclwallet
```

### Building on Windows
You need Visual Studio 2017 (The free C++ Community Edition works just fine). 

From the VS Tools command prompt
```
git clone  https://github.com/ZClassicFoundation/zclwallet.git
cd zclwallet
c:\Qt5\bin\qmake.exe zcl-qt-wallet.pro -spec win32-msvc CONFIG+=debug
nmake

debug\zclwallet.exe
```

To create the Visual Studio project files so you can compile and run from Visual Studio:
```
c:\Qt5\bin\qmake.exe zcl-qt-wallet.pro -tp vc CONFIG+=debug
```

### Building on macOS
You need to install the Xcode app or the Xcode command line tools first, and then install Qt. 

```
git clone https://github.com/ZClassicFoundation/zclwallet.git
cd zclwallet
/path/to/qt5/bin/qmake zcl-qt-wallet.pro CONFIG+=debug
make

./zclwallet.app/Contents/MacOS/zclwallet
```

### [Troubleshooting Guide & FAQ](https://github.com/ZClassicFoundation/zclwallet/wiki/Troubleshooting-&-FAQ)
Please read the [troubleshooting guide](https://github.com/ZClassicFoundation/zclwallet/wiki/Troubleshooting-&-FAQ) for common problems and solutions.
For support or other questions, tweet at [@zclwallet](https://twitter.com/zclwallet) or [file an issue](https://github.com/ZClassicFoundation/zclwallet/issues).

_PS: ZclWallet is NOT an official wallet, and is not affiliated with the Electric Coin Company in any way._
