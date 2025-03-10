# Copyright 2022 Redpanda Data, Inc.
#
# Use of this software is governed by the Business Source License
# included in the file licenses/BSL.md
#
# As of the Change Date specified in that file, in accordance with
# the Business Source License, use of this software will be governed
# by the Apache License, Version 2.0

import requests
from rptest.services.admin import Admin
from rptest.services.cluster import cluster
from ducktape.utils.util import wait_until
from rptest.services.franz_go_verifiable_services import FranzGoVerifiableConsumerGroupConsumer, FranzGoVerifiableProducer, await_minimum_produced_records
from rptest.tests.partition_movement import PartitionMovementMixin
from rptest.tests.prealloc_nodes import PreallocNodesTest
from rptest.clients.types import TopicSpec
from ducktape.mark import parametrize


class HighThroughputPartitionMovementTest(PreallocNodesTest,
                                          PartitionMovementMixin):
    def __init__(self, test_context, *args, **kwargs):
        super().__init__(test_context=test_context,
                         node_prealloc_count=1,
                         num_brokers=5,
                         *args,
                         **kwargs)

        self._partitions = 32
        self._message_size = 1280
        self._message_cnt = 500000
        self._consumers = 8
        self._number_of_moves = 50

    def _start_producer(self, topic_name):
        self.producer = FranzGoVerifiableProducer(
            self.test_context,
            self.redpanda,
            topic_name,
            self._message_size,
            self._message_cnt,
            custom_node=self.preallocated_nodes)
        self.producer.start(clean=False)

        wait_until(lambda: self.producer.produce_status.acked > 10,
                   timeout_sec=30,
                   backoff_sec=1)

    def _start_consumer(self, topic_name):

        self.consumer = FranzGoVerifiableConsumerGroupConsumer(
            self.test_context,
            self.redpanda,
            topic_name,
            self._message_size,
            readers=self._consumers,
            nodes=self.preallocated_nodes)
        self.consumer.start(clean=False)

    def verify(self):
        self.producer.wait()
        # wait for consumers to finish
        wait_until(
            lambda: self.consumer.consumer_status.valid_reads == self.producer.
            produce_status.acked, 30)
        self.consumer.shutdown()
        self.consumer.wait()

        assert self.consumer.consumer_status.valid_reads == self.producer.produce_status.acked

    @cluster(num_nodes=6)
    @parametrize(replication_factor=1)
    @parametrize(replication_factor=3)
    def test_moving_single_partition_under_load(self, replication_factor):
        topic = TopicSpec(partition_count=self._partitions,
                          replication_factor=replication_factor)
        self.client().create_topic(topic)

        self._start_producer(topic.name)
        self._start_consumer(topic.name)

        for _ in range(20):
            # choose a random topic-partition
            metadata = self.client().describe_topics()
            topic, partition = self._random_partition(metadata)
            self.logger.info(f"selected partition: {topic}/{partition}")
            self._do_move_and_verify(topic, partition, 360)

        self.verify()

    def _random_move_and_cancel(self, topic, partition):
        previous_assignment, _ = self._dispatch_random_partition_move(
            topic, partition, allow_no_op=False)

        self._request_move_cancel(unclean_abort=False,
                                  topic=topic,
                                  partition=partition,
                                  previous_assignment=previous_assignment)

    @cluster(num_nodes=6)
    @parametrize(replication_factor=1)
    @parametrize(replication_factor=3)
    def test_interrupting_partition_movement_under_load(
            self, replication_factor):
        topic = TopicSpec(partition_count=self._partitions,
                          replication_factor=replication_factor)
        self.client().create_topic(topic)

        self._start_producer(topic.name)
        self._start_consumer(topic.name)

        for _ in range(self._number_of_moves):
            # choose a random topic-partition
            metadata = self.client().describe_topics()
            topic, partition = self._random_partition(metadata)
            self.logger.info(f"selected partition: {topic}/{partition}")

            self._random_move_and_cancel(topic, partition)
