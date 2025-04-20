#!/bin/bash
set -e

# cat <<EOT >> /etc/apt/sources.list
# deb-src http://archive.ubuntu.com/ubuntu noble main
# deb-src http://archive.ubuntu.com/ubuntu noble universe
# EOT

# apt-get update -y
# apt-get build-dep -y sway libinput


git clone https://gitlab.freedesktop.org/libinput/libinput.git
cd libinput
meson setup --prefix=/usr/local build/
ninja -C build/
ninja -C build/ install

cd ..

# Currently ext-data-control protocol is only available in latest sway
# Once they do a new release, switch to that instead of the git version
# Probably will need to switch the CI to Ubuntu 25.04 though.
git clone https://github.com/swaywm/sway.git
cd sway
git clone https://gitlab.freedesktop.org/wlroots/wlroots.git subprojects/wlroots
git clone https://gitlab.freedesktop.org/wayland/wayland.git subprojects/wayland
git clone https://gitlab.freedesktop.org/wayland/wayland-protocols.git subprojects/wayland-protocols
git clone https://gitlab.freedesktop.org/emersion/libdisplay-info.git subprojects/libdisplay-info
git clone https://gitlab.freedesktop.org/emersion/libliftoff.git subprojects/libliftoff
git clone https://gitlab.freedesktop.org/mesa/drm.git subprojects/libdrm
git clone https://git.sr.ht/~kennylevinsen/seatd subprojects/seatd
meson setup --prefix=/usr/local build/
ninja -C build/
ninja -C build/ install

cat <<EOT >/etc/systemd/system/sway.service
[Unit]
Description=Headless sway
After=network.target
[Service]
ExecStart=/usr/local/bin/sway
Environment=WLR_BACKENDS=headless

[Install]
WantedBy=multi-user.target
EOT

systemctl enable sway.service
systemctl start sway.service
