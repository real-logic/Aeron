/*
 * Copyright 2014-2020 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "aeron_driver_conductor_test.h"

using testing::_;

class DriverConductorIpcTest : public DriverConductorTest, public testing::Test
{
};

// TODO: Parameterise
TEST_F(DriverConductorIpcTest, shouldBeAbleToAddSingleIpcSubscriptionThenAddSingleIpcPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    doWork();

    aeron_ipc_publication_t *publication = aeron_driver_conductor_find_ipc_publication(
        &m_conductor.m_conductor, pub_id);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication), 1u);

    size_t response_number = 0;
    int32_t session_id = 0;
    std::string log_file_name;
    auto handler =
        [&](std::int32_t msgTypeId, AtomicBuffer &buffer, util::index_t offset, util::index_t length)
        {
            if (0 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_SUBSCRIPTION_READY);

                const command::SubscriptionReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), sub_id);
            }
            else if (1 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

                const command::PublicationBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), pub_id);
                session_id = response.sessionId();

                log_file_name = response.logFileName();
            }
            else if (2 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

                const command::ImageBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.streamId(), STREAM_ID_1);
                EXPECT_EQ(response.sessionId(), session_id);

                EXPECT_EQ(response.subscriptionRegistrationId(), sub_id);

                EXPECT_EQ(log_file_name, response.logFileName());
                EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
            }

            response_number++;
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 3u);
}

// TODO: Parameterise
TEST_F(DriverConductorIpcTest, shouldBeAbleToAddSingleIpcPublicationThenAddSingleIpcSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);
    doWork();

    aeron_ipc_publication_t *publication = aeron_driver_conductor_find_ipc_publication(
        &m_conductor.m_conductor, pub_id);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication), 1u);

    size_t response_number = 0;
    int32_t session_id = 0;
    std::string log_file_name;
    auto handler =
        [&](std::int32_t msgTypeId, AtomicBuffer &buffer, util::index_t offset, util::index_t length)
        {
            if (0 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

                const command::PublicationBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), pub_id);
                session_id = response.sessionId();

                log_file_name = response.logFileName();
            }
            else if (1 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_SUBSCRIPTION_READY);

                const command::SubscriptionReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), sub_id);
            }
            else if (2 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

                command::ImageBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.streamId(), STREAM_ID_1);
                EXPECT_EQ(response.sessionId(), session_id);
                EXPECT_EQ(response.subscriptionRegistrationId(), sub_id);

                EXPECT_EQ(log_file_name, response.logFileName());
                EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
            }

            response_number++;
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 3u);
}

// TODO: Parameterise
TEST_F(DriverConductorIpcTest, shouldBeAbleToAddMultipleIpcSubscriptionWithSameStreamIdThenAddSingleIpcPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id_1 = nextCorrelationId();
    int64_t sub_id_2 = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id_1, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id_2, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    doWork();

    aeron_ipc_publication_t *publication = aeron_driver_conductor_find_ipc_publication(
        &m_conductor.m_conductor, pub_id);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication), 2u);

    size_t response_number = 0;
    int32_t session_id = 0;
    std::string log_file_name;
    auto handler =
        [&](std::int32_t msgTypeId, AtomicBuffer &buffer, util::index_t offset, util::index_t length)
        {
            if (0 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_SUBSCRIPTION_READY);

                const command::SubscriptionReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), sub_id_1);
            }
            else if (1 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_SUBSCRIPTION_READY);

                const command::SubscriptionReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), sub_id_2);
            }
            else if (2 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

                const command::PublicationBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), pub_id);
                session_id = response.sessionId();

                log_file_name = response.logFileName();
            }
            else if (3 == response_number || 4 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

                const command::ImageBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.streamId(), STREAM_ID_1);
                EXPECT_EQ(response.sessionId(), session_id);
                EXPECT_TRUE(
                    response.subscriptionRegistrationId() == sub_id_1 ||
                        response.subscriptionRegistrationId() == sub_id_2);

                EXPECT_EQ(log_file_name, response.logFileName());
                EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
            }

            response_number++;
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 5u);
}

// TODO: Parameterise
TEST_F(DriverConductorIpcTest, shouldAddSingleIpcSubscriptionThenAddMultipleExclusiveIpcPublicationsWithSameStreamId)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t pub_id_1 = nextCorrelationId();
    int64_t pub_id_2 = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_1, STREAM_ID_1, true), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_2, STREAM_ID_1, true), 0);
    doWork();

    aeron_ipc_publication_t *publication_1 = aeron_driver_conductor_find_ipc_publication(
        &m_conductor.m_conductor, pub_id_1);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication_1), 1u);
    aeron_ipc_publication_t *publication_2 = aeron_driver_conductor_find_ipc_publication(
        &m_conductor.m_conductor, pub_id_2);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication_2), 1u);

    size_t response_number = 0;
    int32_t session_id_1 = 0;
    int32_t session_id_2 = 0;
    std::string log_file_name_1;
    std::string log_file_name_2;
    auto handler =
        [&](std::int32_t msgTypeId, AtomicBuffer &buffer, util::index_t offset, util::index_t length)
        {
            if (0 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_SUBSCRIPTION_READY);

                const command::SubscriptionReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), sub_id);
            }
            else if (1 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_EXCLUSIVE_PUBLICATION_READY);

                const command::PublicationBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), pub_id_1);
                session_id_1 = response.sessionId();

                log_file_name_1 = response.logFileName();
            }
            else if (2 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

                const command::ImageBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.streamId(), STREAM_ID_1);
                EXPECT_EQ(response.subscriptionRegistrationId(), sub_id);
                EXPECT_EQ(response.sessionId(), session_id_1);
                EXPECT_EQ(response.correlationId(), pub_id_1);
                EXPECT_EQ(log_file_name_1, response.logFileName());
                EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
            }
            else if (3 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_EXCLUSIVE_PUBLICATION_READY);

                const command::PublicationBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), pub_id_2);
                session_id_2 = response.sessionId();

                log_file_name_2 = response.logFileName();
            }
            else if (4 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

                const command::ImageBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.streamId(), STREAM_ID_1);
                EXPECT_EQ(response.subscriptionRegistrationId(), sub_id);
                EXPECT_EQ(response.sessionId(), session_id_2);
                EXPECT_EQ(response.correlationId(), pub_id_2);
                EXPECT_EQ(log_file_name_2, response.logFileName());
                EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
            }

            response_number++;
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 5u);
}

// TODO: Parameterise
TEST_F(DriverConductorIpcTest, shouldNotLinkSubscriptionOnAddPublicationAfterFirstAddPublication)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t pub_id_1 = nextCorrelationId();
    int64_t pub_id_2 = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, -1), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_1, STREAM_ID_1, false), 0);
    ASSERT_EQ(addIpcPublication(client_id, pub_id_2, STREAM_ID_1, false), 0);
    doWork();

    aeron_ipc_publication_t *publication = aeron_driver_conductor_find_ipc_publication(
        &m_conductor.m_conductor, pub_id_1);
    EXPECT_EQ(aeron_ipc_publication_num_subscribers(publication), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_active_ipc_subscriptions(&m_conductor.m_conductor, STREAM_ID_1), 1u);

    size_t response_number = 0;
    int32_t session_id = 0;
    std::string log_file_name;
    auto handler =
        [&](std::int32_t msgTypeId, AtomicBuffer &buffer, util::index_t offset, util::index_t length)
        {
            if (0 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_SUBSCRIPTION_READY);

                const command::SubscriptionReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), sub_id);
            }
            else if (1 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

                const command::PublicationBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), pub_id_1);
                session_id = response.sessionId();

                log_file_name = response.logFileName();
            }
            else if (2 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_AVAILABLE_IMAGE);

                const command::ImageBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.streamId(), STREAM_ID_1);
                EXPECT_EQ(response.subscriptionRegistrationId(), sub_id);
                EXPECT_EQ(response.sessionId(), session_id);
                EXPECT_EQ(response.correlationId(), pub_id_1);
                EXPECT_EQ(log_file_name, response.logFileName());
                EXPECT_EQ(AERON_IPC_CHANNEL, response.sourceIdentity());
            }
            else if (3 == response_number)
            {
                ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_PUBLICATION_READY);

                const command::PublicationBuffersReadyFlyweight response(buffer, offset);

                EXPECT_EQ(response.correlationId(), pub_id_2);
                EXPECT_EQ(response.registrationId(), pub_id_1);
                EXPECT_EQ(response.logFileName(), log_file_name);
            }

            response_number++;
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 4u);
}

// TODO: Paramterise
TEST_F(DriverConductorIpcTest, shouldBeAbleToTimeoutMultipleIpcSubscriptions)
{
    int64_t client_id = nextCorrelationId();
    int64_t sub_id1 = nextCorrelationId();
    int64_t sub_id2 = nextCorrelationId();
    int64_t sub_id3 = nextCorrelationId();

    ASSERT_EQ(addIpcSubscription(client_id, sub_id1, STREAM_ID_1, false), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id2, STREAM_ID_2, false), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id3, STREAM_ID_3, false), 0);
    doWork();
    EXPECT_EQ(aeron_driver_conductor_num_ipc_subscriptions(&m_conductor.m_conductor), 3u);
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 3u);

    doWorkForNs(
        m_context.m_context->publication_linger_timeout_ns + (m_context.m_context->client_liveness_timeout_ns * 2));
    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_ipc_subscriptions(&m_conductor.m_conductor), 0u);
}

// TODO: Parameterise
TEST_F(DriverConductorIpcTest, shouldBeAbleToTimeoutIpcPublicationWithActiveIpcSubscription)
{
    int64_t client_id = nextCorrelationId();
    int64_t pub_id = nextCorrelationId();
    int64_t sub_id = nextCorrelationId();
    int64_t remove_correlation_id = nextCorrelationId();

    ASSERT_EQ(addIpcPublication(client_id, pub_id, STREAM_ID_1, false), 0);
    ASSERT_EQ(addIpcSubscription(client_id, sub_id, STREAM_ID_1, false), 0);
    doWork();
    ASSERT_EQ(removePublication(client_id, remove_correlation_id, pub_id), 0);
    doWork();
    EXPECT_EQ(readAllBroadcastsFromConductor(null_handler), 4u);

    int64_t timeout = m_context.m_context->publication_linger_timeout_ns * 2;

    doWorkForNs(
        timeout,
        100,
        [&]()
        {
            clientKeepalive(client_id);
        });

    EXPECT_EQ(aeron_driver_conductor_num_clients(&m_conductor.m_conductor), 1u);
    EXPECT_EQ(aeron_driver_conductor_num_ipc_publications(&m_conductor.m_conductor), 0u);
    EXPECT_EQ(aeron_driver_conductor_num_active_ipc_subscriptions(&m_conductor.m_conductor, STREAM_ID_1), 0u);

    auto handler =
        [&](std::int32_t msgTypeId, AtomicBuffer &buffer, util::index_t offset, util::index_t length)
        {
            ASSERT_EQ(msgTypeId, AERON_RESPONSE_ON_UNAVAILABLE_IMAGE);

            const command::ImageMessageFlyweight response(buffer, offset);

            EXPECT_EQ(response.correlationId(), pub_id);
            EXPECT_EQ(response.subscriptionRegistrationId(), sub_id);
            EXPECT_EQ(response.streamId(), STREAM_ID_1);
            EXPECT_EQ(response.channel(), AERON_IPC_CHANNEL);
        };

    EXPECT_EQ(readAllBroadcastsFromConductor(handler), 1u);
}
