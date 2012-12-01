#Load the module and creates the device
#!/bin/bash

module="xen_shm"
device="xen_shm"
mode="664"


/sbin/insmod ./$module.ko $* || exit 1

rm -f /dev/${device}

major=`awk "\\$2==\"$module\" {print \\$1}" /proc/devices`

mknod /dev/${device} c $major 0

group="staff"
grep -q '^staff:' /etc/group || group="wheel"

chgrp $group /dev/${device}
chmod $mode /dev/${device}

