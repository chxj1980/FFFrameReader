﻿/**
 * Copyright 2019 Matthew Oliver
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "FFFRTestData.h"
#include "FfFrameReader.h"

#include <gtest/gtest.h>

using namespace FfFrameReader;

class FrameTest1 : public ::testing::TestWithParam<TestParams>
{
protected:
    FrameTest1() = default;

    void SetUp() override
    {
        const auto ret = m_manager.getStream(GetParam().m_fileName);
        ASSERT_NE(ret.index(), 0);
        const auto stream = std::get<1>(ret);
        const auto ret1 = stream->getNextFrame();
        ASSERT_NE(ret1.index(), 0);
        m_frame = std::get<1>(ret1);
    }

    ~FrameTest1() override
    {
        m_manager.releaseStream(GetParam().m_fileName);
    }

    Manager m_manager;
    std::shared_ptr<Frame> m_frame;
};

TEST_P(FrameTest1, getTimeStamp)
{
    ASSERT_EQ(m_frame->getTimeStamp(), 0);
}

TEST_P(FrameTest1, getFrameNumber)
{
    ASSERT_EQ(m_frame->getFrameNumber(), 0);
}

TEST_P(FrameTest1, getWidth)
{
    ASSERT_EQ(m_frame->getWidth(), GetParam().m_width);
}

TEST_P(FrameTest1, getHeight)
{
    ASSERT_EQ(m_frame->getHeight(), GetParam().m_height);
}

TEST_P(FrameTest1, getAspectRatio)
{
    ASSERT_DOUBLE_EQ(m_frame->getAspectRatio(), GetParam().m_aspectRatio);
}

// TODO: Get output frames and check they have decoded properly

INSTANTIATE_TEST_SUITE_P(FrameTestData, FrameTest1, ::testing::ValuesIn(g_testData));