#!/bin/sh
# Wi-Fi hotspot (AP-only): stops the wlan0 station, runs AP "trimui-ap" on interface ap0
# (WPA2, HT40+ on channel 1, DHCP via dnsmasq on 10.66.0.0/24, no NAT/internet sharing).
# `ap-up.sh stop` removes the AP and reconnects the station with /root/wifi-up.sh.
# The passphrase is generated once into /etc/hostapd/ap.pass. apk: hostapd dnsmasq
#
# Station + AP at the same time is advertised by the driver (1 managed + 1 AP, one
# channel) but stalls the station's traffic on this firmware/driver; see
# docs/wifi-throughput.md. Hence AP-only for now.
CONF=/etc/hostapd/ap0.conf
PASS=/etc/hostapd/ap.pass
HPID=/run/hostapd-ap0.pid
DPID=/run/dnsmasq-ap0.pid

# Stop by pid file: `pkill -f` patterns also match unrelated command lines
# (e.g. the ssh shell that runs this script).
stop_pid() { [ -s "$1" ] && kill "$(cat "$1")" 2>/dev/null; rm -f "$1"; return 0; }

stop_station() {
	for p in $(pidof wpa_supplicant) $(pidof udhcpc); do kill $p; done
	sleep 1
	ip addr flush dev wlan0
	ip link set wlan0 down
}

if [ "${1:-start}" = stop ]; then
	stop_pid $HPID
	stop_pid $DPID
	sleep 1
	iw dev ap0 del 2>/dev/null
	ip link set wlan0 up
	timeout 70 /root/wifi-up.sh
	exit 0
fi

mkdir -p /etc/hostapd /var/lib/misc
umask 077
[ -s $PASS ] || tr -dc 'a-km-np-z2-9' < /dev/urandom | head -c 16 > $PASS
umask 022

stop_station
iw dev ap0 info >/dev/null 2>&1 || iw phy phy0 interface add ap0 type __ap
# xradio only accepts its two registered addresses: wlan0's MAC and MAC + 1.
M=$(cat /sys/class/net/wlan0/address)
ip link set ap0 address "$(printf '%s:%02x' "${M%:*}" $(( (0x${M##*:} + 1) % 256 )))"

cat > $CONF <<EOF
interface=ap0
driver=nl80211
ssid=trimui-ap
hw_mode=g
channel=1
ieee80211n=1
ht_capab=[HT40+][SHORT-GI-20][SHORT-GI-40]
wmm_enabled=1
auth_algs=1
wpa=2
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
wpa_passphrase=$(cat $PASS)
EOF
chmod 600 $CONF

ip addr flush dev ap0
ip addr add 10.66.0.1/24 dev ap0

stop_pid $HPID
hostapd -B -P $HPID -f /tmp/hostapd-ap0.log $CONF
for i in 1 2 3 4 5 6 7 8 9 10; do
	grep -q AP-ENABLED /tmp/hostapd-ap0.log && break
	sleep 1
done

# DHCP only (port=0 disables DNS).
stop_pid $DPID
dnsmasq --interface=ap0 --bind-interfaces --port=0 --except-interface=lo \
	--dhcp-range=10.66.0.10,10.66.0.50,12h --dhcp-option=3,10.66.0.1 \
	--dhcp-leasefile=/var/lib/misc/dnsmasq-ap0.leases --pid-file=$DPID \
	--log-facility=/tmp/dnsmasq-ap0.log

grep -E "AP-ENABLED|AP-DISABLED|Could not|failed" /tmp/hostapd-ap0.log | head -3
exit 0
