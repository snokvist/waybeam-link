/* SPDX-License-Identifier: GPL-2.0-or-later
 * Forwarding header: devourer's desktop-Linux branch includes
 * <libusb-1.0/libusb.h> (the distro layout); the vendored libusb-cmake tree
 * ships the header as plain libusb.h. cmake/pkgconf-libusb.sh puts both this
 * directory and the real header directory on the include path, so this
 * resolves to the vendored header without patching vendored code. */
#include <libusb.h>
