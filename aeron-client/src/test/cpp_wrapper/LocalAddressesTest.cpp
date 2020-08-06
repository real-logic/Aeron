
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
#include <gmock/gmock.h>

#include "EmbeddedMediaDriver.h"
#include "Aeron.h"
#include "ChannelUriStringBuilder.h"
#include "TestUtil.h"

using namespace aeron;
using testing::MockFunction;
using testing::_;

class PubSubTest : public testing::Test
{
public:
    PubSubTest()
    {
        m_driver.start();
    }

    ~PubSubTest() override
    {
        m_driver.stop();
    }

protected:
    EmbeddedMediaDriver m_driver;
};

TEST_F(PubSubTest, shouldGetLocalAddresses)
{
    std::int32_t streamId = 10001;
    std::string channel = "aeron:udp?endpoint=127.0.0.1:23456|control=127.0.0.1:23457";

    Context ctx;
    ctx.useConductorAgentInvoker(true);
    std::shared_ptr<Aeron> aeron = Aeron::connect(ctx);

    AgentInvoker<ClientConductor> &invoker = aeron->conductorAgentInvoker();
    std::int64_t subId = aeron->addSubscription(channel, streamId);
    std::int64_t pubId = aeron->addPublication(channel, streamId);

    {
        POLL_FOR_NON_NULL(sub, aeron->findSubscription(subId), invoker);
        auto subAddresses = sub->localSocketAddresses();
        ASSERT_EQ(1U, subAddresses.size());
        EXPECT_NE(std::string::npos, channel.find(subAddresses[0]));

        POLL_FOR_NON_NULL(pub, aeron->findPublication(pubId), invoker);
        auto pubAddresses = pub->localSocketAddresses();
        ASSERT_EQ(1U, pubAddresses.size());
        EXPECT_NE(std::string::npos, channel.find(pubAddresses[0]));
    }

    invoker.invoke();
}