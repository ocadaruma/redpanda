# Copyright 2022 Redpanda Data, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0
import time
from typing import NamedTuple, Optional
from rptest.services.cluster import cluster

from rptest.clients.default import DefaultClient
from rptest.services.redpanda import SISettings
from rptest.clients.rpk import RpkTool, RpkException
from rptest.clients.types import TopicSpec
from rptest.util import expect_exception
from ducktape.mark import matrix
from ducktape.tests.test import TestContext

import json

from rptest.services.redpanda import RedpandaService
from rptest.tests.end_to_end import EndToEndTest
from rptest.utils.expect_rate import ExpectRate, RateTarget
from rptest.services.verifiable_producer import VerifiableProducer, is_int_with_prefix
from rptest.services.verifiable_consumer import VerifiableConsumer
from rptest.util import (
    wait_until, )


class BucketUsage(NamedTuple):
    num_objects: int
    total_bytes: int
    keys: set[str]


class TestReadReplicaService(EndToEndTest):
    log_segment_size = 1048576  # 5MB
    topic_name = "panda-topic"
    si_settings = SISettings(
        cloud_storage_reconciliation_interval_ms=500,
        cloud_storage_max_connections=5,
        log_segment_size=log_segment_size,
        cloud_storage_readreplica_manifest_sync_timeout_ms=500,
        cloud_storage_segment_max_upload_interval_sec=5)

    def __init__(self, test_context: TestContext):
        super(TestReadReplicaService, self).__init__(test_context=test_context)
        self.second_cluster = None

    def start_second_cluster(self) -> None:
        self.second_cluster = RedpandaService(self.test_context,
                                              num_brokers=3,
                                              si_settings=self.si_settings)
        self.second_cluster.start(start_si=False)

    def create_read_replica_topic(self) -> None:
        rpk_second_cluster = RpkTool(self.second_cluster)
        conf = {
            'redpanda.remote.readreplica':
            self.si_settings.cloud_storage_bucket,
        }
        rpk_second_cluster.create_topic(self.topic_name, config=conf)

    def start_consumer(self) -> None:
        # important side effect for superclass; we will use the replica
        # consumer from here on.
        self.consumer = VerifiableConsumer(
            self.test_context,
            num_nodes=1,
            redpanda=self.second_cluster,
            topic=self.topic_name,
            group_id='consumer_test_group',
            on_record_consumed=self.on_record_consumed)
        self.consumer.start()

    def start_producer(self) -> None:
        self.producer = VerifiableProducer(
            self.test_context,
            num_nodes=1,
            redpanda=self.redpanda,
            topic=self.topic_name,
            throughput=1000,
            message_validator=is_int_with_prefix)
        self.producer.start()

    def create_read_replica_topic_success(self) -> bool:
        try:
            self.create_read_replica_topic()
        except RpkException as e:
            if "The server experienced an unexpected error when processing the request" in str(
                    e):
                return False
            else:
                raise
        else:
            return True

    def _setup_read_replica(self, num_messages=0, partition_count=3) -> None:
        self.logger.info(f"Setup read replica \"{self.topic_name}\", : "
                         f"{num_messages} msg, {partition_count} "
                         "partitions.")
        # Create original topic
        self.start_redpanda(3, si_settings=self.si_settings)
        spec = TopicSpec(name=self.topic_name,
                         partition_count=partition_count,
                         replication_factor=3)

        DefaultClient(self.redpanda).create_topic(spec)

        if num_messages > 0:
            self.start_producer()
            wait_until(lambda: self.producer.num_acked > num_messages,
                           timeout_sec=30,
                           err_msg="Producer failed to produce messages for %ds." %\
                           30)
            self.logger.info("Stopping producer after writing up to offsets %s" %\
                            str(self.producer.last_acked_offsets))
            self.producer.stop()

        # Make original topic upload data to S3
        rpk = RpkTool(self.redpanda)
        rpk.alter_topic_config(spec.name, 'redpanda.remote.write', 'true')

        self.start_second_cluster()
        # wait until the read replica topic creation succeeds
        wait_until(
            self.create_read_replica_topic_success,
            timeout_sec=300,
            backoff_sec=5,
            err_msg="Could not create read replica topic. Most likely " +
            "because topic manifest is not in S3.")

    def _bucket_usage(self) -> BucketUsage:
        assert self.redpanda and self.redpanda.s3_client
        keys: set[str] = set()
        s3 = self.redpanda.s3_client
        num_objects = total_bytes = 0
        bucket = self.si_settings.cloud_storage_bucket
        for o in s3.list_objects(bucket):
            num_objects += 1
            total_bytes += o.ContentLength
            keys.add(o.Key)
        self.redpanda.logger.info(f"bucket usage {num_objects} objects, " +
                                  f"{total_bytes} bytes for {bucket}")
        return BucketUsage(num_objects, total_bytes, keys)

    def _bucket_delta(self, bu1: BucketUsage,
                      bu2: BucketUsage) -> Optional[BucketUsage]:
        """ Return None if both bucket usage reports are equal.
            Otherwise, return deltas. """
        obj_delta = bu2.num_objects - bu1.num_objects
        bytes_delta = bu2.total_bytes - bu1.total_bytes
        keys_delta = bu2.keys.difference(bu1.keys)
        if obj_delta or bytes_delta or len(keys_delta) > 0:
            return BucketUsage(obj_delta, bytes_delta, keys_delta)
        else:
            return None

    @cluster(num_nodes=6)
    @matrix(partition_count=[10])
    def test_produce_is_forbidden(self, partition_count: int) -> None:

        self._setup_read_replica(partition_count=partition_count)
        second_rpk = RpkTool(self.second_cluster)
        with expect_exception(
                RpkException, lambda e:
                "unable to produce record: INVALID_TOPIC_EXCEPTION: The request attempted to perform an operation on an invalid topic."
                in str(e)):
            second_rpk.produce(self.topic_name, "", "test payload")

    @cluster(num_nodes=9)
    @matrix(partition_count=[10], min_records=[10000])
    def test_simple_end_to_end(self, partition_count: int,
                               min_records: int) -> None:

        self._setup_read_replica(num_messages=min_records,
                                 partition_count=partition_count)

        # Consume from read replica topic and validate
        self.start_consumer()
        self.run_validation()  # calls self.consumer.stop()
        assert self.redpanda
        self.redpanda.stop()

        # Run consumer again, this time with source cluster stopped.
        # Now we can test that replicas do not write to s3.
        self.start_consumer()

        # Assert zero bytes written for at least 20 seconds.
        total_bytes = lambda: self._bucket_usage().total_bytes
        assert self.redpanda and self.redpanda.logger
        er = ExpectRate(total_bytes, self.redpanda.logger)
        zero_growth = RateTarget(max_total_sec=60,
                                 target_sec=20,
                                 target_min_rate=0,
                                 target_max_rate=0)
        er.expect_rate(zero_growth)

        # Get current bucket usage
        pre_usage = self._bucket_usage()
        self.logger.info(f"pre_usage {pre_usage}")

        # Let replica consumer run to completion, assert no s3 writes
        self.run_consumer_validation()

        post_usage = self._bucket_usage()
        self.logger.info(f"post_usage {post_usage}")
        delta = self._bucket_delta(pre_usage, post_usage)
        if delta:
            m = f"S3 Bucket usage changed during read replica test: {delta}"
            assert False, m
