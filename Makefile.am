# Main Makefile.am for utun - root directory configuration

# Order matters: lib -> src -> tests (tests depend on src objects)
SUBDIRS = lib lib/libopus src tests

# Подготовка тестов перед запуском: сначала актуальное ядро, затем тестовые программы и конфиги.
.PHONY: check-build
check-build: all
	$(MAKE) $(AM_MAKEFLAGS) -C tests check-build

# Полный комплект лицензий устанавливается вместе с бинарником и входит в make dist.
licensedir = $(datadir)/licenses/utun
dist_license_DATA = LICENSE LICENSING THIRD_PARTY_NOTICES

EXTRA_DIST = readme.md \
	build.bat \
	build_full.bat \
	archive_sources.sh \
	proxy.sh.example \
	tools/build_windows.ps1 \
	tools/chatgui/build.bat \
	tools/chatgui/install_deps_win.sh \
	tests/test_windows_bootstrap.ps1 \
	doc/dependencies.txt \
	tools/prepare_dependencies.py \
	tools/sodium.cmake \
	tools/build_openssl.sh \
	tools/prepare_android.py \
	tools/chatgui-android/prepare_build.sh \
	tools/dependency_patches/miniaudio.patch \
	tools/dependency_patches/rlottie.patch \
	tools/cgathui_common/tests/test_ble_etcp.c \
	tools/cgathui_common/tests/test_ble_join.c \
	doc/nat_detection.txt \
	doc/stun_nat.md \
	doc/node_snapshot.md \
	tools/chatgui/zxing-cpp/LICENSE \
	tools/chatgui/zxing-cpp/core/src/libzueci/LICENSE \
	tools/chatgui/third_party/qhotkey/LICENSE \
	tools/chatgui-android/app/src/main/cpp/onnxruntime/LICENSE \
	tools/chatgui-android/app/src/main/cpp/onnxruntime/ThirdPartyNotices.txt \
	tools/chatgui-android/app/src/main/cpp/openssl/LICENSE.txt

# Clean build directory
clean-local:
	-rm -rf build/
