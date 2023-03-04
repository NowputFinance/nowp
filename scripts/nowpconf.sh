#!/bin/bash -ev

mkdir -p ~/.nowp
echo "rpcuser=username" >>~/.nowp/nowp.conf
echo "rpcpassword=`head -c 32 /dev/urandom | base64`" >>~/.nowp/nowp.conf

