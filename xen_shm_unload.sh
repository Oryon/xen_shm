#Unload the module and removes the devices
#!/bin/bash

module="xen_shm"
device="xen_shm"

/sbin/rmmod $module $* || exit 1

rm -f /dev/${device}

