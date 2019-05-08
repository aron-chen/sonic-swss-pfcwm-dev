# sonic-swss-pfcwm-dev
Unit test environment for PFC WaterMarks

## Getting Started

```
git clone https://github.com/aron-chen/sonic-swss-pfcwm-dev
cd sonic-swss-pfcwm-dev/

sudo bash -x install-pkg.sh -g

cd ..
mkdir <build-dir> && cd <build-dir>
bash -x <source-dir>/build.sh
source packages/.env
```

## Starting redis-server and open UNIX socket
```
sudo apt install -y redis-server
sudo mkdir -p /var/run/redis/
echo "unixsocket /var/run/redis/redis.sock" | sudo tee --append  /etc/redis/redis.conf
echo "unixsocketperm 777" | sudo tee --append  /etc/redis/redis.conf
sudo service redis-server restart
```
