# sonic-swss-pfcwm-dev
Unit test environment for PFC WaterMarks

## Getting Started

```
git clone https://github.com/ezio-chen/sonic-swss-acl-dev
cd sonic-swss-acl-dev/
git checkout remotes/origin/package_install

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
