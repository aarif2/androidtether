# android-usb-eth

USB tethering for Android phones on macOS. macOS has no RNDIS driver, so an Android phone in USB tethering mode never shows up as a network. This is a small root daemon that fixes that.

Plug in the phone and turn on USB tethering. The daemon sets up a network interface (`feth0`, `feth2`, ...), gets an address over DHCP, and removes the interface when you unplug. With no phone attached it does nothing.

## How it works

- Polls USB every 2 seconds for an RNDIS interface (works with any Android phone).
- Does the RNDIS handshake over libusb in userspace. No kernel extension.
- Creates a `feth` virtual Ethernet pair and bridges frames between the phone and the pair using `/dev/bpf`.
- Runs `ipconfig set <if> DHCP`, which creates a temporary network service.
- Tears everything down when the phone disconnects.

## Requirements

- macOS 13 or newer (Apple Silicon tested)
- Xcode command line tools
- libusb: `brew install libusb`

## Install

```
git clone https://github.com/aarif2/android-usb-eth
cd android-usb-eth
make
sudo ./awp_install.sh
```

Log: `/var/log/awp_rndisd.log`

## Uninstall

```
sudo ./awp_uninstall.sh
```

## Notes

- The interface is named `fethN`. macOS does not allow renaming interfaces to `eth0`.
- The service is created by DHCP and does not appear in System Settings > Network.
- Routing priority is left to macOS. Wi-Fi stays preferred; the phone link carries traffic when other routes drop.
- The daemon runs as root because creating `feth` interfaces and opening BPF devices require it. The code is one file, `awp_rndisd.c`.
- Only RNDIS is handled. Phones that expose CDC-NCM or ECM work natively on macOS and do not need this.

## License

MIT
