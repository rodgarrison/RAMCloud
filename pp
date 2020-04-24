#!/bin/bash

FILES="CoordinatorClusterClock.proto CoordinatorUpdateInfo.proto EnumerationIterator.proto Histogram.proto Indexlet.proto LogMetrics.proto MasterRecoveryInfo.proto MetricList.proto ProtoBufTest.proto RecoveryPartition.proto ServerConfig.proto ServerListEntry.proto ServerList.proto ServerStatistics.proto SpinLockStatistics.proto TableConfig.proto TableManager.proto Table.proto Tablets.proto"

cd src
for f in $FILES
do
	protoc $f --cpp_out=../obj.master
done
