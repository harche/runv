#!/bin/bash

ARCH=`uname -m`
ARCHPATH=arch/$ARCH

cp $ARCHPATH/kernel_config ./kernel_config
cp Dockerfile.$ARCH Dockerfile

# Till the ppc64le gets hyperctl ported it has to use 
# stardard docker to generate kernel binary
if [ $ARCH == "ppc64le" ] 
then 
	docker build -t hyperstart-dev-$ARCH:latest -f Dockerfile . 
	docker run --rm hyperstart-dev-$ARCH:latest cat /root/build/result/kernel >kernel.new && mv -f kernel.new $ARCHPATH/kernel 
else
	hyperctl build -t hyperstart-dev-$ARCH:latest -f Dockerfile . 
	hyperctl run --rm hyperstart-dev-$ARCH:latest cat /root/build/result/kernel >kernel.new && mv -f kernel.new $ARCHPATH/kernel 
	hyperctl run --rm hyperstart-dev-$ARCH:latest cat /root/build/result/modules.tar > $ARCHPATH/modules.tar 
fi

mv $ARCHPATH/kernel_config $ARCHPATH/kernel_config.old

if [ $ARCH == "ppc64le" ]
then 
	docker run --rm hyperstart-dev-$ARCH:latest cat /root/build/result/kernel_config > $ARCHPATH/kernel_config 
else 
	hyperctl run --rm hyperstart-dev-$ARCH:latest cat /root/build/result/kernel_config > $ARCHPATH/kernel_config 
fi
rm ./kernel_config

