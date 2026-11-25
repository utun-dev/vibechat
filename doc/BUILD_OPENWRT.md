# Сборка uTun под OpenWRT

## Быстрый старт

```bash
cd ~/utun2
./build_openwrt.sh                     # ASUS RT-AX53U (по умолчанию)
```

Первый запуск соберёт toolchain (30-120 минут), последующие — только пакет.

## Другие платформы

```bash
./build_openwrt.sh ramips/mt7621 asus_rt-ax54       # ASUS RT-AX54
./build_openwrt.sh ramips/mt7621 xiaomi_mi-router-4a-gigabit
./build_openwrt.sh ath79/generic tplink_archer-c7-v2
./build_openwrt.sh x86/64                            # x86_64 PC
./build_openwrt.sh --list                            # список целей
```

## Ручная сборка (menuconfig)

```bash
cd /home/user/openwrt
make menuconfig          # Target System → MediaTek Ralink MIPS
                         # Subtarget → MT7621 based boards
                         # Target Profile → ASUS RT-AX53U
                         # Network → utun (M)
make -j$(nproc)          # полная сборка прошивки
# или только пакет:
make package/utun2/compile -j$(nproc) V=s
```

Пакет: `bin/packages/*/base/utun_*.ipk`

## Структура

- `~/openwrt/package/utun2/Makefile` — OpenWRT-пакет
- `~/openwrt/` — клон buildroot (main, https://github.com/openwrt/openwrt)
- `~/utun2/build_openwrt.sh` — скрипт сборки

## Зависимости (host)

```bash
apt-get install -y build-essential git subversion libncurses-dev \
    libssl-dev ccache xsltproc gawk gettext unzip python3 libtool
```

## Примечания

- `configure` генерируется через `autoreconf -fi` (уже сделан)
- Пакет использует `USE_SOURCE_DIR` — билд из /home/user/utun2 напрямую
- Для чистой пересборки: `make package/utun2/clean && make package/utun2/compile -j$(nproc)`
