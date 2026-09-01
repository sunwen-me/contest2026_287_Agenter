#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Keep an ICMP echo request in flight towards the K1 board for a whole window.
#
# Why this exists.  The resident window on the board is a few seconds long and
# the interesting counters (icmp serve requests/sent, dup dropped) only move
# while something outside is actually asking.  A plain
#
#     ping -i 0.3 -w 900 192.168.1.206
#
# does NOT stay alive for those seconds: until the board has taken its DHCP
# lease and answered an ARP request, the host cannot resolve the address, the
# kernel hands ping EHOSTUNREACH for every probe, and iputils ping gives up
# after a handful of them -- it prints its statistics block and exits with
# "+5 errors ... pipe 5" about three seconds in, no matter how large -w is.
# Run 70 lost its whole echo-serve reading that way (requests=0x0), while run
# 68 only survived because the board's address happened to be in the host
# neighbour table already from the run before it.
#
# So restart ping whenever it exits, until the deadline.  Once the board is up
# the running invocation simply stays up and no further restart happens; before
# that, each restart costs one round of unreachable errors and nothing else.
#
# Usage: tools/k1_host_ping.sh [IP] [SECONDS] [INTERVAL]
#        tools/k1_host_ping.sh 192.168.1.206 900 0.3 > /tmp/k1-ping.txt 2>&1
#
# Run it detached (setsid nohup ... &) so it outlives the shell that starts it.

set -u

ip=${1:-192.168.1.206}
seconds=${2:-900}
interval=${3:-0.3}

# -D timestamps each reply so the log can be lined up against the serial log,
# -W 1 keeps a lost reply from stalling the sequence, and -c is large enough
# that a healthy invocation is never the thing that ends the run.
count=$(awk -v s="$seconds" -v i="$interval" 'BEGIN { c = s / i; if (c < 1) c = 1;
                                                     printf "%d", c + 1 }')

end=$(awk -v s="$seconds" 'BEGIN { printf "%d", s }')
start=$(date +%s)

echo "k1_host_ping: target=$ip window=${seconds}s interval=${interval}s"

while :; do
  now=$(date +%s)
  left=$((end - (now - start)))
  [ "$left" -gt 0 ] || break

  ping -D -i "$interval" -W 1 -c "$count" -w "$left" "$ip" || true

  now=$(date +%s)
  [ $((end - (now - start))) -gt 0 ] || break
  echo "k1_host_ping: ping exited early ($(( now - start ))s in), restarting"
done

echo "k1_host_ping: window over"
