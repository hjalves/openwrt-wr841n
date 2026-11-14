# SSH read-only inventory

Collected 2026-11-14 through a temporary Docker-based legacy SSH client. This snapshot contains sensitive network identifiers. Credentials, wireless keys, private keys, and password hashes are intentionally omitted; configuration fields with credential-like names are redacted. Dynamic values change. Some command errors may reflect unavailable utilities rather than absent features.

This is a historical snapshot: `luci-app-ddns` and `ddns-scripts` were removed later at the user's request. See [overlay-audit.md](overlay-audit.md).

The router lacked the `ip` and standalone `iwinfo` commands; `ifconfig`, `route`, `swconfig`, and `iw` provided the output below. SSH was subsequently verified with both password authentication and the RSA host key. The observed, independently unverified SHA-256 fingerprints are RSA 2048: `jjM7WpR5nlKuqqVZKJaW3YSbk3cZWBOHqVBxKPrSmis` and DSA 1024: `8H8LNcftNm2i0zeDd3dr9CjqYa4S/gp382NKdcvitb8`.

## Identity and firmware

```text
uid=0(root) gid=0(root)
Linux OpenWrt 3.18.23 #1 Sun Jan 31 18:39:35 CET 2016 mips GNU/Linux
DISTRIB_ID='OpenWrt'
DISTRIB_RELEASE='15.05.1'
DISTRIB_REVISION='r48532'
DISTRIB_CODENAME='chaos_calmer'
DISTRIB_TARGET='ar71xx/generic'
DISTRIB_DESCRIPTION='OpenWrt Chaos Calmer 15.05.1'
DISTRIB_TAINTS=''
{
	"kernel": "3.18.23",
	"hostname": "OpenWrt",
	"system": "Qualcomm Atheros QCA9533 ver 1 rev 1",
	"model": "TP-Link TL-WR841N\/ND v9",
	"release": {
		"distribution": "OpenWrt",
		"version": "15.05.1",
		"revision": "r48532",
		"codename": "chaos_calmer",
		"target": "ar71xx\/generic",
		"description": "OpenWrt Chaos Calmer 15.05.1"
	}
}
```

## Runtime resources

```text
Sat Nov 14 14:01:57 WET 2026
 14:01:57 up 21 days, 16:58,  load average: 0.01, 0.07, 0.07
{
	"uptime": 1875527,
	"localtime": 1794664917,
	"load": [
		864,
		4800,
		4896
	],
	"memory": {
		"total": 29265920,
		"free": 6361088,
		"shared": 618496,
		"buffered": 2199552
	},
	"swap": {
		"total": 0,
		"free": 0
	}
}
             total         used         free       shared      buffers
Mem:         28580        22296         6284          604         2148
-/+ buffers:              20148         8432
Swap:            0            0            0
Filesystem                Size      Used Available Use% Mounted on
rootfs                  576.0K    324.0K    252.0K  56% /
/dev/root                 2.3M      2.3M         0 100% /rom
tmpfs                    14.0M    604.0K     13.4M   4% /tmp
/dev/mtdblock3          576.0K    324.0K    252.0K  56% /overlay
overlayfs:/overlay      576.0K    324.0K    252.0K  56% /
tmpfs                   512.0K         0    512.0K   0% /dev
1875526.87 1849420.29
```

## CPU and flash

```text
system type		: Qualcomm Atheros QCA9533 ver 1 rev 1
machine			: TP-LINK TL-WR841N/ND v9
processor		: 0
cpu model		: MIPS 24Kc V7.4
BogoMIPS		: 366.18
wait instruction	: yes
microsecond timers	: yes
tlb_entries		: 16
extra interrupt vector	: yes
hardware watchpoint	: yes, count: 4, address/irw mask: [0x0ffc, 0x0ffc, 0x0ffb, 0x0ffb]
isa			: mips1 mips2 mips32r1 mips32r2
ASEs implemented	: mips16
shadow register sets	: 1
kscratch registers	: 0
package			: 0
core			: 0
VCED exceptions		: not available
VCEI exceptions		: not available

dev:    size   erasesize  name
mtd0: 00020000 00010000 "u-boot"
mtd1: 0011f620 00010000 "kernel"
mtd2: 002b09e0 00010000 "rootfs"
mtd3: 00090000 00010000 "rootfs_data"
mtd4: 00010000 00010000 "art"
mtd5: 003d0000 00010000 "firmware"
```

## Interfaces and routes

```text
br-lan    Link encap:Ethernet  HWaddr 30:B5:C2:80:F4:BE  
          inet addr:192.168.0.254  Bcast:192.168.0.255  Mask:255.255.255.0
          inet6 addr: fe80::32b5:c2ff:fe80:f4be/64 Scope:Link
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          RX packets:1784221 errors:0 dropped:0 overruns:0 frame:0
          TX packets:54034 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:0 
          RX bytes:251264568 (239.6 MiB)  TX bytes:6716670 (6.4 MiB)

eth0      Link encap:Ethernet  HWaddr 30:B5:C2:80:F4:BE  
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          RX packets:149966544 errors:0 dropped:41 overruns:0 frame:0
          TX packets:52795092 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:1000 
          RX bytes:366736637 (349.7 MiB)  TX bytes:419004090 (399.5 MiB)
          Interrupt:5 

eth1      Link encap:Ethernet  HWaddr 30:B5:C2:80:F4:BF  
          UP BROADCAST MULTICAST  MTU:1500  Metric:1
          RX packets:0 errors:0 dropped:0 overruns:0 frame:0
          TX packets:0 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:1000 
          RX bytes:0 (0.0 B)  TX bytes:0 (0.0 B)
          Interrupt:4 

lo        Link encap:Local Loopback  
          inet addr:127.0.0.1  Mask:255.0.0.0
          inet6 addr: ::1/128 Scope:Host
          UP LOOPBACK RUNNING  MTU:65536  Metric:1
          RX packets:1000 errors:0 dropped:0 overruns:0 frame:0
          TX packets:1000 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:0 
          RX bytes:79239 (77.3 KiB)  TX bytes:79239 (77.3 KiB)

wlan0     Link encap:Ethernet  HWaddr 30:B5:C2:80:F4:BE  
          inet6 addr: fe80::32b5:c2ff:fe80:f4be/64 Scope:Link
          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1
          RX packets:56445323 errors:0 dropped:0 overruns:0 frame:0
          TX packets:153138556 errors:0 dropped:0 overruns:0 carrier:0
          collisions:0 txqueuelen:1000 
          RX bytes:1493436706 (1.3 GiB)  TX bytes:4243454701 (3.9 GiB)

Kernel IP routing table
Destination     Gateway         Genmask         Flags Metric Ref    Use Iface
0.0.0.0         192.168.0.1     0.0.0.0         UG    0      0        0 br-lan
192.168.0.0     0.0.0.0         255.255.255.0   U     0      0        0 br-lan
IP address       HW type     Flags       HW address            Mask     Device
192.168.0.201    0x1         0x2         90:ec:ea:e5:c4:b2     *        br-lan
192.168.0.2      0x1         0x2         c8:60:00:1f:5f:f8     *        br-lan
192.168.0.204    0x1         0x2         54:60:09:49:8f:ee     *        br-lan
192.168.0.1      0x1         0x2         24:df:6a:13:ff:a5     *        br-lan
```

## Switch configuration and link state

```text
Global attributes:
	enable_vlan: 1
Port 0:
	pvid: 1
	link: port:0 link:up speed:1000baseT full-duplex txflow rxflow 
Port 1:
	pvid: 1
	link: port:1 link:down
Port 2:
	pvid: 1
	link: port:2 link:up speed:100baseT full-duplex auto
Port 3:
	pvid: 1
	link: port:3 link:down
Port 4:
	pvid: 1
	link: port:4 link:up speed:100baseT full-duplex auto
VLAN 1:
	vid: 1
	ports: 0 1 2 3 4
```

## Wireless status

```text
Interface wlan0
	ifindex 6
	wdev 0x2
	addr 30:b5:c2:80:f4:be
	ssid wifi2
	type AP
	wiphy 0
	channel 11 (2462 MHz), width: 20 MHz, center1: 2462 MHz
Station 54:60:09:49:8f:ee (on wlan0)
	inactive time:	50 ms
	rx bytes:	89057872
	rx packets:	246621
	tx bytes:	71410520
	tx packets:	206246
	tx retries:	6873
	tx failed:	987
	signal:  	-46 [-51, -48] dBm
	signal avg:	-38 [-40, -43] dBm
	tx bitrate:	72.2 MBit/s MCS 7 short GI
	rx bitrate:	1.0 MBit/s
	expected throughput:	34.84Mbps
	authorized:	yes
	authenticated:	yes
	preamble:	short
	WMM/WME:	yes
	MFP:		no
	TDLS peer:	no
Station 92:69:c8:d9:cf:32 (on wlan0)
	inactive time:	6140 ms
	rx bytes:	2211804
	rx packets:	12245
	tx bytes:	21710041
	tx packets:	17859
	tx retries:	404
	tx failed:	17
	signal:  	-57 [-58, -67] dBm
	signal avg:	-53 [-56, -59] dBm
	tx bitrate:	72.2 MBit/s MCS 7 short GI
	rx bitrate:	1.0 MBit/s
	expected throughput:	34.84Mbps
	authorized:	yes
	authenticated:	yes
	preamble:	short
	WMM/WME:	yes
	MFP:		no
	TDLS peer:	no
Station 90:ec:ea:e5:c4:b2 (on wlan0)
	inactive time:	450 ms
	rx bytes:	10349605
	rx packets:	62422
	tx bytes:	303759026
	tx packets:	241413
	tx retries:	7156
	tx failed:	20
	signal:  	-55 [-56, -63] dBm
	signal avg:	-54 [-54, -62] dBm
	tx bitrate:	86.7 MBit/s MCS 12 short GI
	rx bitrate:	24.0 MBit/s
	expected throughput:	37.573Mbps
	authorized:	yes
	authenticated:	yes
	preamble:	short
	WMM/WME:	yes
	MFP:		no
	TDLS peer:	no
```

## Processes

```text
PID USER       VSZ STAT COMMAND
    1 root      1408 S    /sbin/procd
    2 root         0 SW   [kthreadd]
    3 root         0 SW   [ksoftirqd/0]
    5 root         0 SW<  [kworker/0:0H]
    6 root         0 SW   [kworker/u2:0]
    7 root         0 SW<  [khelper]
   27 root         0 SW<  [writeback]
   29 root         0 SW<  [bioset]
   30 root         0 SW<  [kblockd]
   31 root         0 SW   [kswapd0]
   32 root         0 SW   [kworker/0:1]
   99 root         0 SW   [fsnotify_mark]
  149 root         0 SW   [spi0]
  265 root         0 SW<  [deferwq]
  266 root         0 SW   [kworker/u2:2]
  316 root         0 SW   [kworker/0:2]
  346 root         0 SWN  [jffs2_gcd_mtd3]
  430 root       892 S    /sbin/ubusd
  431 root       772 S    /sbin/askfirst /bin/ash --login
  561 root         0 SW<  [ipv6_addrconf]
  616 root         0 SW<  [cfg80211]
  722 root      1044 S    /sbin/logd -S 16
  731 root      1532 S    /sbin/rpcd
  765 root      1504 S    /sbin/netifd
  789 root      1160 S    /usr/sbin/odhcpd
  867 root      1152 S    /usr/sbin/dropbear -F -P /var/run/dropbear.1.pid -p 192.168.0.254:22 -K 300
  949 root      1628 S    /usr/sbin/uhttpd -f -h /www -r OpenWrt -x /cgi-bin -u /ubus -t 60 -T 30 -k 20 -A 1 -n 3 -N 100 -R -p 0.0.
 1017 nobody     960 S    /usr/sbin/dnsmasq -C /var/etc/dnsmasq.conf -k -x /var/run/dnsmasq/dnsmasq.pid
 1065 root      1364 S    /usr/sbin/ntpd -n -S /usr/sbin/ntpd-hotplug -p 0.openwrt.pool.ntp.org -p 1.openwrt.pool.ntp.org -p 2.open
 1115 root      1664 S    /usr/sbin/hostapd -P /var/run/wifi-phy0.pid -B /var/run/hostapd-phy0.conf
 8961 root      1220 S    /usr/sbin/dropbear -F -P /var/run/dropbear.1.pid -p 192.168.0.254:22 -K 300
 8968 root      1360 S    ash -c sh -s
 8969 root      1360 S    sh -s
 8988 root      1360 R    ps w
```

## Listeners

```text
Active Internet connections (only servers)
Proto Recv-Q Send-Q Local Address           Foreign Address         State       PID/Program name    
tcp        0      0 0.0.0.0:80              0.0.0.0:*               LISTEN      949/uhttpd
tcp        0      0 0.0.0.0:53              0.0.0.0:*               LISTEN      1017/dnsmasq
tcp        0      0 192.168.0.254:22        0.0.0.0:*               LISTEN      867/dropbear
tcp        0      0 :::80                   :::*                    LISTEN      949/uhttpd
tcp        0      0 :::53                   :::*                    LISTEN      1017/dnsmasq
udp        0      0 0.0.0.0:53              0.0.0.0:*                           1017/dnsmasq
udp        0      0 :::53                   :::*                                1017/dnsmasq
```

## Startup enablement

```text
enabled boot
enabled cron
enabled ddns
enabled dnsmasq
enabled done
enabled dropbear
enabled etherwake
enabled firewall
enabled led
enabled log
enabled network
enabled odhcpd
enabled rpcd
enabled sysctl
enabled sysfixtime
enabled sysntpd
enabled system
enabled telnet
enabled uhttpd
disabled umount
```

## SSH host-key files and fingerprints

```text
drwx------    1 root     root             0 Jan 31  2016 .
drwxr-xr-x    1 root     root             0 Jan 31  2016 ..
-rw-------    1 root     root           459 Jan 31  2016 dropbear_dss_host_key
-rw-------    1 root     root           805 Jan 31  2016 dropbear_rsa_host_key
/etc/dropbear/dropbear_dss_host_key: Fingerprint: md5 fc:e9:8a:ca:3a:98:e9:57:d6:50:e5:a9:a7:53:62:44
/etc/dropbear/dropbear_rsa_host_key: Fingerprint: md5 f9:d0:1d:de:d1:10:52:86:cf:2d:9c:c2:3d:b7:fc:d1
```

## DNS resolver

```text
# Interface lan
nameserver 1.1.1.1
nameserver 1.0.0.1
```

## Pending UCI changes

```text

```

## Redacted configuration

```text
[network]
network.loopback=interface
network.loopback.ifname='lo'
network.loopback.proto='static'
network.loopback.ipaddr='127.0.0.1'
network.loopback.netmask='255.0.0.0'
network.globals=globals
network.globals.ula_prefix='fda2:32bc:5e0a::/48'
network.lan=interface
network.lan.ifname='eth0'
network.lan.force_link='1'
network.lan.type='bridge'
network.lan.proto='static'
network.lan.netmask='255.255.255.0'
network.lan.delegate='0'
network.lan.ipaddr='192.168.0.254'
network.lan.gateway='192.168.0.1'
network.lan.dns='1.1.1.1 1.0.0.1'
network.wan=interface
network.wan.ifname='eth1'
network.wan.proto='dhcp'
network.wan.peerdns='0'
network.@switch[0]=switch
network.@switch[0].name='switch0'
network.@switch[0].reset='1'
network.@switch[0].enable_vlan='1'
network.@switch_vlan[0]=switch_vlan
network.@switch_vlan[0].device='switch0'
network.@switch_vlan[0].vlan='1'
network.@switch_vlan[0].ports='0 1 2 3 4'
network.@route[0]=route
network.@route[0].interface='wan'
network.@route[0].target='192.168.100.1'
network.@route[1]=route

[wireless]
wireless.radio0=wifi-device
wireless.radio0.type='mac80211'
wireless.radio0.hwmode='11g'
wireless.radio0.path='platform/qca953x_wmac'
wireless.radio0.noscan='1'
wireless.radio0.htmode='HT20'
wireless.radio0.disabled='0'
wireless.radio0.country='CN'
wireless.radio0.txpower='19'
wireless.radio0.channel='auto'
wireless.@wifi-iface[0]=wifi-iface
wireless.@wifi-iface[0].device='radio0'
wireless.@wifi-iface[0].mode='ap'
wireless.@wifi-iface[0].network='lan'
wireless.@wifi-iface[0].encryption='psk2'
wireless.@wifi-iface[0].ssid='wifi2'
wireless.@wifi-iface[0].key=[REDACTED]

[dhcp]
dhcp.@dnsmasq[0]=dnsmasq
dhcp.@dnsmasq[0].domainneeded='1'
dhcp.@dnsmasq[0].boguspriv='1'
dhcp.@dnsmasq[0].localise_queries='1'
dhcp.@dnsmasq[0].rebind_protection='1'
dhcp.@dnsmasq[0].rebind_localhost='1'
dhcp.@dnsmasq[0].local='/lan/'
dhcp.@dnsmasq[0].domain='lan'
dhcp.@dnsmasq[0].expandhosts='1'
dhcp.@dnsmasq[0].authoritative='1'
dhcp.@dnsmasq[0].readethers='1'
dhcp.@dnsmasq[0].leasefile='/tmp/dhcp.leases'
dhcp.@dnsmasq[0].resolvfile='/tmp/resolv.conf.auto'
dhcp.@dnsmasq[0].localservice='1'
dhcp.@dnsmasq[0].cachesize='500'
dhcp.@dnsmasq[0].server='1.1.1.1' '1.0.0.1'
dhcp.lan=dhcp
dhcp.lan.interface='lan'
dhcp.lan.ignore='1'
dhcp.wan=dhcp
dhcp.wan.interface='wan'
dhcp.wan.ignore='1'
dhcp.odhcpd=odhcpd
dhcp.odhcpd.maindhcp='0'
dhcp.odhcpd.leasefile='/tmp/hosts/odhcpd'
dhcp.odhcpd.leasetrigger='/usr/sbin/odhcpd-update'
dhcp.@host[0]=host
dhcp.@host[0].name='atom'
dhcp.@host[0].ip='192.168.10.254'
dhcp.@host[0].mac='f0:79:59:25:d0:71'
dhcp.@host[1]=host
dhcp.@host[1].name='thinkpad'
dhcp.@host[1].mac='58:00:e3:ca:ad:b9'
dhcp.@host[1].ip='192.168.10.250'
dhcp.@host[2]=host
dhcp.@host[2].name='atom-wlan'
dhcp.@host[2].mac='40:e2:30:5a:5b:15'
dhcp.@host[2].ip='192.168.10.253'
dhcp.@host[3]=host
dhcp.@host[3].name='desktop'
dhcp.@host[3].ip='192.168.10.10'
dhcp.@host[3].mac='54:04:a6:3c:a2:99'
dhcp.@host[4]=host
dhcp.@host[4].name='rpi'
dhcp.@host[4].mac='b8:27:eb:6a:6c:9f'
dhcp.@host[4].ip='192.168.10.20'

[firewall]
firewall.@defaults[0]=defaults
firewall.@defaults[0].syn_flood='1'
firewall.@defaults[0].input='ACCEPT'
firewall.@defaults[0].output='ACCEPT'
firewall.@defaults[0].forward='REJECT'
firewall.@zone[0]=zone
firewall.@zone[0].name='lan'
firewall.@zone[0].input='ACCEPT'
firewall.@zone[0].output='ACCEPT'
firewall.@zone[0].forward='ACCEPT'
firewall.@zone[0].network='lan'
firewall.@zone[1]=zone
firewall.@zone[1].name='wan'
firewall.@zone[1].input='REJECT'
firewall.@zone[1].output='ACCEPT'
firewall.@zone[1].forward='REJECT'
firewall.@zone[1].masq='1'
firewall.@zone[1].mtu_fix='1'
firewall.@zone[1].network='wan wan6 wwan'
firewall.@forwarding[0]=forwarding
firewall.@forwarding[0].src='lan'
firewall.@forwarding[0].dest='wan'
firewall.@rule[0]=rule
firewall.@rule[0].name='Allow-DHCP-Renew'
firewall.@rule[0].src='wan'
firewall.@rule[0].proto='udp'
firewall.@rule[0].dest_port='68'
firewall.@rule[0].target='ACCEPT'
firewall.@rule[0].family='ipv4'
firewall.@rule[1]=rule
firewall.@rule[1].name='Allow-Ping'
firewall.@rule[1].src='wan'
firewall.@rule[1].proto='icmp'
firewall.@rule[1].icmp_type='echo-request'
firewall.@rule[1].family='ipv4'
firewall.@rule[1].target='ACCEPT'
firewall.@rule[2]=rule
firewall.@rule[2].name='Allow-IGMP'
firewall.@rule[2].src='wan'
firewall.@rule[2].proto='igmp'
firewall.@rule[2].family='ipv4'
firewall.@rule[2].target='ACCEPT'
firewall.@rule[3]=rule
firewall.@rule[3].name='Allow-DHCPv6'
firewall.@rule[3].src='wan'
firewall.@rule[3].proto='udp'
firewall.@rule[3].src_ip='fe80::/10'
firewall.@rule[3].src_port='547'
firewall.@rule[3].dest_ip='fe80::/10'
firewall.@rule[3].dest_port='546'
firewall.@rule[3].family='ipv6'
firewall.@rule[3].target='ACCEPT'
firewall.@rule[4]=rule
firewall.@rule[4].name='Allow-MLD'
firewall.@rule[4].src='wan'
firewall.@rule[4].proto='icmp'
firewall.@rule[4].src_ip='fe80::/10'
firewall.@rule[4].icmp_type='130/0' '131/0' '132/0' '143/0'
firewall.@rule[4].family='ipv6'
firewall.@rule[4].target='ACCEPT'
firewall.@rule[5]=rule
firewall.@rule[5].name='Allow-ICMPv6-Input'
firewall.@rule[5].src='wan'
firewall.@rule[5].proto='icmp'
firewall.@rule[5].icmp_type='echo-request' 'echo-reply' 'destination-unreachable' 'packet-too-big' 'time-exceeded' 'bad-header' 'unknown-header-type' 'router-solicitation' 'neighbour-solicitation' 'router-advertisement' 'neighbour-advertisement'
firewall.@rule[5].limit='1000/sec'
firewall.@rule[5].family='ipv6'
firewall.@rule[5].target='ACCEPT'
firewall.@rule[6]=rule
firewall.@rule[6].name='Allow-ICMPv6-Forward'
firewall.@rule[6].src='wan'
firewall.@rule[6].dest='*'
firewall.@rule[6].proto='icmp'
firewall.@rule[6].icmp_type='echo-request' 'echo-reply' 'destination-unreachable' 'packet-too-big' 'time-exceeded' 'bad-header' 'unknown-header-type'
firewall.@rule[6].limit='1000/sec'
firewall.@rule[6].family='ipv6'
firewall.@rule[6].target='ACCEPT'
firewall.@include[0]=include
firewall.@include[0].path='/etc/firewall.user'
firewall.@rule[7]=rule
firewall.@rule[7].src='wan'
firewall.@rule[7].dest='lan'
firewall.@rule[7].proto='esp'
firewall.@rule[7].target='ACCEPT'
firewall.@rule[8]=rule
firewall.@rule[8].src='wan'
firewall.@rule[8].dest='lan'
firewall.@rule[8].dest_port='500'
firewall.@rule[8].proto='udp'
firewall.@rule[8].target='ACCEPT'
firewall.@redirect[0]=redirect
firewall.@redirect[0].target='DNAT'
firewall.@redirect[0].src='wan'
firewall.@redirect[0].dest='lan'
firewall.@redirect[0].proto='tcp'
firewall.@redirect[0].src_dport='22'
firewall.@redirect[0].dest_ip='192.168.10.254'
firewall.@redirect[0].dest_port='22'
firewall.@redirect[0].name='ssh'
firewall.@redirect[1]=redirect
firewall.@redirect[1].target='DNAT'
firewall.@redirect[1].src='wan'
firewall.@redirect[1].dest='lan'
firewall.@redirect[1].proto='tcp'
firewall.@redirect[1].src_dport='80'
firewall.@redirect[1].dest_ip='192.168.10.254'
firewall.@redirect[1].dest_port='80'
firewall.@redirect[1].name='web'
firewall.miniupnpd=include
firewall.miniupnpd.type='script'
firewall.miniupnpd.path='/usr/share/miniupnpd/firewall.include'
firewall.miniupnpd.family='any'
firewall.miniupnpd.reload='1'

[system]
system.@system[0]=system
system.@system[0].hostname='OpenWrt'
system.@system[0].zonename='Europe/Lisbon'
system.@system[0].timezone='WET0WEST,M3.5.0/1,M10.5.0'
system.@system[0].conloglevel='8'
system.@system[0].cronloglevel='8'
system.ntp=timeserver
system.ntp.server='0.openwrt.pool.ntp.org' '1.openwrt.pool.ntp.org' '2.openwrt.pool.ntp.org' '3.openwrt.pool.ntp.org'
system.ntp.enabled='1'
system.led_wan=led
system.led_wan.name='WAN'
system.led_wan.sysfs='tp-link:green:wan'
system.led_wan.trigger='netdev'
system.led_wan.dev='eth1'
system.led_wan.mode='link tx rx'
system.led_lan1=led
system.led_lan1.name='LAN1'
system.led_lan1.sysfs='tp-link:green:lan1'
system.led_lan1.trigger='switch0'
system.led_lan1.port_mask='0x10'
system.led_lan2=led
system.led_lan2.name='LAN2'
system.led_lan2.sysfs='tp-link:green:lan2'
system.led_lan2.trigger='switch0'
system.led_lan2.port_mask='0x08'
system.led_lan3=led
system.led_lan3.name='LAN3'
system.led_lan3.sysfs='tp-link:green:lan3'
system.led_lan3.trigger='switch0'
system.led_lan3.port_mask='0x04'
system.led_lan4=led
system.led_lan4.name='LAN4'
system.led_lan4.sysfs='tp-link:green:lan4'
system.led_lan4.trigger='switch0'
system.led_lan4.port_mask='0x02'
system.led_wlan=led
system.led_wlan.name='WLAN'
system.led_wlan.sysfs='tp-link:green:wlan'
system.led_wlan.trigger='phy0tpt'

[dropbear]
dropbear.@dropbear[0]=dropbear
dropbear.@dropbear[0].PasswordAuth=[REDACTED]
dropbear.@dropbear[0].Port='22'
dropbear.@dropbear[0].Interface='lan'

[uhttpd]
uhttpd.main=uhttpd
uhttpd.main.listen_http='0.0.0.0:80' '[::]:80'
uhttpd.main.listen_https='0.0.0.0:443' '[::]:443'
uhttpd.main.redirect_https='1'
uhttpd.main.home='/www'
uhttpd.main.rfc1918_filter='1'
uhttpd.main.max_requests='3'
uhttpd.main.max_connections='100'
uhttpd.main.cert='/etc/uhttpd.crt'
uhttpd.main.key=[REDACTED]
uhttpd.main.cgi_prefix='/cgi-bin'
uhttpd.main.script_timeout='60'
uhttpd.main.network_timeout='30'
uhttpd.main.http_keepalive='20'
uhttpd.main.tcp_keepalive='1'
uhttpd.main.ubus_prefix='/ubus'
uhttpd.px5g=cert
uhttpd.px5g.days='730'
uhttpd.px5g.bits='1024'
uhttpd.px5g.country='ZZ'
uhttpd.px5g.state='Somewhere'
uhttpd.px5g.location='Uknown'
uhttpd.px5g.commonname='OpenWrt'

[ddns]
ddns.global=ddns
ddns.global.date_format='%F %R'
ddns.global.log_lines='250'
ddns.global.allow_local_ip='0'
ddns.myddns_ipv4=service
ddns.myddns_ipv4.interface='wan'
ddns.myddns_ipv4.update_url='http://[USERNAME]:[PASSWORD]@ipv4.nsupdate.info/nic/update?hostname=[DOMAIN]&myip=[REDACTED]
ddns.myddns_ipv4.use_syslog='2'
ddns.myddns_ipv4.use_logfile='1'
ddns.myddns_ipv4.enabled='1'
ddns.myddns_ipv4.ip_source='web'
ddns.myddns_ipv4.ip_url='http://checkip.dyndns.com'
ddns.myddns_ipv4.domain='galisteu.nsupdate.info'
ddns.myddns_ipv4.username=[REDACTED]
ddns.myddns_ipv4.password=[REDACTED]

[luci]
luci.main=core
luci.main.lang='auto'
luci.main.mediaurlbase='/luci-static/bootstrap'
luci.main.resourcebase='/luci-static/resources'
luci.flash_keep=extern
luci.flash_keep.uci='/etc/config/'
luci.flash_keep.dropbear='/etc/dropbear/'
luci.flash_keep.openvpn='/etc/openvpn/'
luci.flash_keep.passwd=[REDACTED]
luci.flash_keep.opkg='/etc/opkg.conf'
luci.flash_keep.firewall='/etc/firewall.user'
luci.flash_keep.uploads='/lib/uci/upload/'
luci.languages=internal
luci.sauth=internal
luci.sauth.sessionpath='/tmp/luci-sessions'
luci.sauth.sessiontime='3600'
luci.ccache=internal
luci.ccache.enable='1'
luci.themes=internal
luci.themes.Bootstrap='/luci-static/bootstrap'
```

## Installed package versions

```text
base-files - 157.2-r48532
busybox - 1.23.2-1
ddns-scripts - 2.4.3-2
dnsmasq - 2.73-1
dropbear - 2015.67-1
etherwake - 1.09-3
firewall - 2015-07-27
fstools - 2016-01-10-96415afecef35766332067f4205ef3b2c7561d21
hostapd-common - 2015-03-25-1
ip6tables - 1.4.21-1
iptables - 1.4.21-1
iw - 3.17-1
jshn - 2015-11-08-10429bccd0dc5d204635e110a7a8fae7b80d16cb
jsonfilter - 2014-06-19-cdc760c58077f44fc40adbbe41e1556a67c1b9a9
kernel - 3.18.23-1-b2f200610f46d20ef52d269421369d0c
kmod-ath - 3.18.23+2015-03-09-3
kmod-ath9k - 3.18.23+2015-03-09-3
kmod-ath9k-common - 3.18.23+2015-03-09-3
kmod-cfg80211 - 3.18.23+2015-03-09-3
kmod-crypto-aes - 3.18.23-1
kmod-crypto-arc4 - 3.18.23-1
kmod-crypto-core - 3.18.23-1
kmod-gpio-button-hotplug - 3.18.23-1
kmod-ip6tables - 3.18.23-1
kmod-ipt-conntrack - 3.18.23-1
kmod-ipt-core - 3.18.23-1
kmod-ipt-nat - 3.18.23-1
kmod-ipv6 - 3.18.23-1
kmod-lib-crc-ccitt - 3.18.23-1
kmod-mac80211 - 3.18.23+2015-03-09-3
kmod-nf-conntrack - 3.18.23-1
kmod-nf-conntrack6 - 3.18.23-1
kmod-nf-ipt - 3.18.23-1
kmod-nf-ipt6 - 3.18.23-1
kmod-nf-nat - 3.18.23-1
kmod-nf-nathelper - 3.18.23-1
kmod-ppp - 3.18.23-1
kmod-pppoe - 3.18.23-1
kmod-pppox - 3.18.23-1
kmod-slhc - 3.18.23-1
libblobmsg-json - 2015-11-08-10429bccd0dc5d204635e110a7a8fae7b80d16cb
libc - 0.9.33.2-1
libgcc - 4.8-linaro-1
libip4tc - 1.4.21-1
libip6tc - 1.4.21-1
libiwinfo - 2015-06-01-ade8b1b299cbd5748db1acf80dd3e9f567938371
libiwinfo-lua - 2015-06-01-ade8b1b299cbd5748db1acf80dd3e9f567938371
libjson-c - 0.12-1
libjson-script - 2015-11-08-10429bccd0dc5d204635e110a7a8fae7b80d16cb
liblua - 5.1.5-1
libnl-tiny - 0.1-4
libubox - 2015-11-08-10429bccd0dc5d204635e110a7a8fae7b80d16cb
libubus - 2015-05-25-f361bfa5fcb2daadf3b160583ce665024f8d108e
libubus-lua - 2015-05-25-f361bfa5fcb2daadf3b160583ce665024f8d108e
libuci - 2015-08-27.1-1
libuci-lua - 2015-08-27.1-1
libxtables - 1.4.21-1
lua - 5.1.5-1
luci - git-16.018.33482-3201903-1
luci-app-ddns - 2.2.4-1
luci-app-firewall - git-16.018.33482-3201903-1
luci-app-wol - git-16.018.33482-3201903-1
luci-base - git-16.018.33482-3201903-1
luci-lib-ip - git-16.018.33482-3201903-1
luci-lib-nixio - git-16.018.33482-3201903-1
luci-mod-admin-full - git-16.018.33482-3201903-1
luci-proto-ipv6 - git-16.018.33482-3201903-1
luci-proto-ppp - git-16.018.33482-3201903-1
luci-theme-bootstrap - git-16.018.33482-3201903-1
mtd - 21
netifd - 2015-12-16-245527193e90906451be35c2b8e972b8712ea6ab
odhcp6c - 2015-07-13-024525798c5f6aba3af9b2ef7b3af2f3c14f1db8
odhcpd - 2015-11-19-01d3f9d64486ac1daa144848944e877e7f0cb762
opkg - 9c97d5ecd795709c8584e972bfdf3aee3a5b846d-9
ppp - 2.4.7-6
ppp-mod-pppoe - 2.4.7-6
procd - 2015-10-29.1-d5fddd91b966424bb63e943e789704d52382cc18
rpcd - 2015-01-10-f00890cd6eb47ad9bb5da0fb6c50aedc8406e7c5
swconfig - 10
uboot-envtools - 2014.10-2
ubox - 2015-11-22-c086167a0154745c677f8730a336ea9cf7d71031
ubus - 2015-05-25-f361bfa5fcb2daadf3b160583ce665024f8d108e
ubusd - 2015-05-25-f361bfa5fcb2daadf3b160583ce665024f8d108e
uci - 2015-08-27.1-1
uhttpd - 2015-11-08-fe01ef3f52adae9da38ef47926cd50974af5d6b7
uhttpd-mod-ubus - 2015-11-08-fe01ef3f52adae9da38ef47926cd50974af5d6b7
usign - 2015-05-08-cf8dcdb8a4e874c77f3e9a8e9b643e8c17b19131
wpad-mini - 2015-03-25-1
```
