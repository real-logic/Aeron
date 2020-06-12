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

#include <functional>

#include <gtest/gtest.h>
#include <inttypes.h>

extern "C"
{
#include "aeron_driver_context.h"
#include "agent/aeron_driver_agent.c"
}

class DriverAgentTest : public testing::Test
{
public:
    DriverAgentTest()
    {
        agent_is_initialized = AERON_INIT_ONCE_VALUE;
        mask = 0;
        rb_buffer = NULL;
        logfp = NULL;
        log_reader_running = false;

        if (aeron_driver_context_init(&m_context) < 0)
        {
            throw std::runtime_error("could not init context: " + std::string(aeron_errmsg()));
        }
    }

    ~DriverAgentTest() override
    {
        aeron_driver_context_close(m_context);
        clear_env(AERON_AGENT_MASK_ENV_VAR);

        log_reader_running = false;
        int pthread_result = aeron_thread_join(log_reader_thread, NULL);
        if (0 != pthread_result && ESRCH != pthread_result)
        {
            printf("*** [WARNING] Could not stop logger thread: result=%d\n", pthread_result);
        }

        if (NULL != rb_buffer)
        {
            aeron_free(rb_buffer);
        }
    }

    static char *to_mask(uint64_t value)
    {
        static char buffer[256];

        snprintf(buffer, sizeof(buffer) - 1, "%" PRIu64, value);
        return buffer;
    }

    static int set_env(const char *name, const char *value)
    {
        #if defined(AERON_COMPILER_MSVC)
            return _putenv_s(name, value);
        #else
            return setenv(name, value, 1);
        #endif
    }

    static int clear_env(const char *name)
    {
        #if defined(AERON_COMPILER_MSVC)
            return _put_env_s(name, "");
        #else
            return unsetenv(name);
        #endif
    }

protected:
    aeron_driver_context_t *m_context = nullptr;
};

TEST_F(DriverAgentTest, shouldInitializeUntetheredStateChangeInterceptor)
{
    EXPECT_EQ(set_env(AERON_AGENT_MASK_ENV_VAR, to_mask(AERON_UNTETHERED_SUBSCRIPTION_STATE_CHANGE)), 0);

    aeron_driver_agent_context_init(m_context);

    EXPECT_EQ(m_context->untethered_subscription_state_change_func, &aeron_driver_agent_untethered_subscription_state_change_interceptor);
}

TEST_F(DriverAgentTest, shouldKeepOriginalUntetheredStateChangeFunctionIfEventNotEnabled)
{
    aeron_driver_agent_context_init(m_context);

    EXPECT_EQ(m_context->untethered_subscription_state_change_func, &aeron_untethered_subscription_state_change);
}

TEST_F(DriverAgentTest, shouldLogUntetheredSubscriptionStateChange)
{
    init_logging_ring_buffer();

    aeron_subscription_tether_state_t old_state = AERON_SUBSCRIPTION_TETHER_RESTING;
    aeron_subscription_tether_state_t new_state = AERON_SUBSCRIPTION_TETHER_ACTIVE;
    int64_t now_ns = -432482364273648;
    int32_t stream_id = 777;
    int32_t session_id = 21;
    int64_t subscription_id = 56;
    aeron_tetherable_position_t tetherable_position = {};
    tetherable_position.state = old_state;
    tetherable_position.subscription_registration_id = subscription_id;

    aeron_driver_agent_untethered_subscription_state_change_interceptor(
            &tetherable_position,
            now_ns,
            new_state,
            stream_id,
            session_id);

    EXPECT_EQ(tetherable_position.state, new_state);
    EXPECT_EQ(tetherable_position.time_of_last_update_ns, now_ns);

    auto message_handler = [](int32_t msg_type_id, const void *msg, size_t length, void *clientd)
    {
        size_t *count = (size_t *)clientd;
        (*count)++;

        EXPECT_EQ(msg_type_id, AERON_UNTETHERED_SUBSCRIPTION_STATE_CHANGE);

        aeron_driver_agent_untethered_subscription_state_change_log_header_t *data =
                (aeron_driver_agent_untethered_subscription_state_change_log_header_t *)msg;
        EXPECT_EQ(data->new_state, AERON_SUBSCRIPTION_TETHER_ACTIVE);
        EXPECT_EQ(data->old_state, AERON_SUBSCRIPTION_TETHER_RESTING);
        EXPECT_EQ(data->subscription_id, 56);
        EXPECT_EQ(data->stream_id, 777);
        EXPECT_EQ(data->session_id, 21);
    };

    size_t timesCalled = 0;
    const size_t messagesRead = aeron_mpsc_rb_read(&logging_mpsc_rb, message_handler, &timesCalled, 1);

    EXPECT_EQ(messagesRead, (size_t)1);
    EXPECT_EQ(timesCalled, (size_t)1);
}
