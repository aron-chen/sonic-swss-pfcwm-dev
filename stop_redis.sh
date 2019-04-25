#/bin/bash

redis-cli -s %redis_unix_socket% shutdown nosave
