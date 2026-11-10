# Pinned sources of the Chinese conversion engines and their build
# dependencies. Sourced by tools/fetch-chinese.sh and tools/build-chinese.sh;
# docs/upkeep.md says how to move a pin.
#
# Each git pin is NAME_URL, NAME_TAG and NAME_COMMIT (the tag's commit, not an
# annotated tag object). The fetch checks out the commit and refuses any other.

# Compression used by libime's dictionaries and model (BSD-3-Clause).
zstd_URL=https://github.com/facebook/zstd.git
zstd_TAG=v1.5.7
zstd_COMMIT=f8745da6ff1ad1e7bab384bd1f9d742439278e99

# Boost's CMake source release; libime needs headers and Boost.Iostreams (BSL-1.0).
boost_URL=https://github.com/boostorg/boost/releases/download/boost-1.92.0/boost-1.92.0-cmake.tar.xz
boost_SHA256=9bed76128d4e46755dbe818487788c6fceb6f72b378f4daa49b7e1e600d9088d
boost_DIR=boost-1.92.0

# KDE's CMake modules, used only while configuring fcitx5 and libime (BSD-3-Clause).
ecm_URL=https://github.com/KDE/extra-cmake-modules.git
ecm_TAG=v6.31.0
ecm_COMMIT=2958379fb49c71129f119ec17f88df0b5ea21550

# Only fcitx5's utility library is built; libime links it (LGPL-2.1-or-later).
fcitx5_URL=https://github.com/fcitx/fcitx5.git
fcitx5_TAG=5.1.23
fcitx5_COMMIT=27cca6e0239938b2d53937061d0980c087856b22

# Pinyin conversion with KenLM (LGPL-2.1-or-later). Its build downloads the
# dictionary and language model and checks them against SHA-256 values pinned
# in its data/CMakeLists.txt.
libime_URL=https://github.com/fcitx/libime.git
libime_TAG=1.1.17
libime_COMMIT=171edcf137001e8eb8274f53ec010058cda70b09
libime_SUBMODULES="src/libime/core/kenlm=4cb443e60b7bf2c0ddf3c745378f76cb59e254e5"

# Zhuyin: McBopomofo's portable engine and Taiwan data (MIT). Only src/Engine is built.
mcbopomofo_URL=https://github.com/openvanilla/fcitx5-mcbopomofo.git
mcbopomofo_TAG=3.1.1
mcbopomofo_COMMIT=a84bdd5c741e1696b270159ebda675946dc73d49

# Simplified-to-Traditional conversion for Traditional Pinyin (Apache-2.0).
opencc_URL=https://github.com/BYVoid/OpenCC.git
opencc_TAG=ver.1.4.2
opencc_COMMIT=025f371dc76b598d77384fbdab90c937471844d8

chinese_git_sources=(zstd ecm fcitx5 libime mcbopomofo opencc)
