TCP EVIL
========

TCP-EVIL Congestion control algorithm, based on very few modifications
to tcp_hybla

This module is for Linux 3.2, Debian 7. Use `apt-get source` your kernel
on Debian 7 to get the kernel source for the same kernel version.

Modifications are simple:

1. in slow start phase, increase speed more quickly
2. in congestion avoidance phase, increase speed more quickly
3. when entering loss state, set ssthresh to 0.9 cwnd instead of 0.5 cwnd

This is a demostration of how easily we can increase TCP sending speed
from the server side if you ignore fairness and friendliness.


WARNING: DO NOT USE THIS MODULE!

REPEAT: DO NOT USE THIS MODULE!


