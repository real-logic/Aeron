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
#ifndef AERON_UTIL_COMMAND_OPTION_H
#define AERON_UTIL_COMMAND_OPTION_H

#include <iostream>
#include <exception>
#include <string>
#include <vector>
#include <map>

#include "Exceptions.h"
#include "util/Export.h"
#include "util/StringUtil.h"

namespace aeron { namespace util
{

AERON_DECLARE_SOURCED_EXCEPTION (CommandOptionException, ExceptionCategory::EXCEPTION_CATEGORY_ERROR);

class CLIENT_EXPORT CommandOption
{

private:
    char m_optionChar;
    size_t m_minParams;
    size_t m_maxParams;
    std::string m_helpText;

    bool m_isPresent;

    std::vector<std::string> m_params;

    void checkIndex(size_t index) const
    {
        if (index > m_params.size())
        {
            throw CommandOptionException(
                std::string("Internal Error: index out of range for option: ") + m_optionChar, SOURCEINFO);
        }
    }

public:
    static const char UNNAMED = -1;

    CommandOption() :
        m_optionChar('-'),
        m_minParams(0),
        m_maxParams(0),
        m_helpText(""),
        m_isPresent(false)
    {
    }

    CommandOption(char optionChar, size_t minParams, size_t maxParams, std::string helpText) :
        m_optionChar(optionChar),
        m_minParams(minParams),
        m_maxParams(maxParams),
        m_helpText(std::move(helpText)),
        m_isPresent(false)
    {
    }

    char getOptionChar() const
    {
        return m_optionChar;
    }

    std::string getHelpText() const
    {
        return m_helpText;
    }

    void addParam(std::string p)
    {
        m_params.push_back(std::move(p));
    }

    void validate() const
    {
        if (!isPresent())
        {
            return;
        }

        if (m_params.size() > m_maxParams)
        {
            throw CommandOptionException(
                std::string("option -") + m_optionChar + " has too many parameters specified.", SOURCEINFO);
        }

        if (m_params.size() < m_minParams)
        {
            throw CommandOptionException(
                std::string("option -") + m_optionChar + " has too few parameters specified.", SOURCEINFO);
        }
    }

    bool isPresent() const
    {
        return m_isPresent;
    }

    void setPresent()
    {
        m_isPresent = true;
    }

    size_t getNumParams() const
    {
        return m_params.size();
    }

    std::string getParam(size_t index) const
    {
        checkIndex(index);
        return m_params[index];
    }

    std::string getParam(size_t index, std::string defaultValue) const
    {
        if (!isPresent())
        {
            return defaultValue;
        }

        return getParam(index);
    }


    int getParamAsInt(size_t index) const
    {
        checkIndex(index);
        std::string param = m_params[index];

        try
        {
            return parse<int>(param);
        }
        catch (const ParseException &)
        {
            throw CommandOptionException(
                std::string("invalid numeric value: \"") + param + "\" on option -" + m_optionChar, SOURCEINFO);
        }
    }

    long getParamAsLong(size_t index) const
    {
        checkIndex(index);
        std::string param = m_params[index];

        try
        {
            return parse<long>(param);
        }
        catch (const ParseException &)
        {
            throw CommandOptionException(
                std::string("invalid numeric value: \"") + param + "\" on option -" + m_optionChar, SOURCEINFO);
        }
    }

    int getParamAsInt(size_t index, int minValue, int maxValue, int defaultValue) const
    {
        if (!isPresent())
        {
            return defaultValue;
        }

        int value = getParamAsInt(index);
        if (value < minValue || value > maxValue)
        {
            throw CommandOptionException(
                std::string("value \"") + toString(value) + "\" out of range: [" +
                    toString(minValue) + ".." + toString(maxValue) + "] on option -" + m_optionChar,
                SOURCEINFO);
        }

        return value;
    }

    long getParamAsLong(size_t index, long minValue, long maxValue, long defaultValue) const
    {
        if (!isPresent())
        {
            return defaultValue;
        }

        long value = getParamAsLong(index);
        if (value < minValue || value > maxValue)
        {
            throw CommandOptionException(
                std::string("value \"") + toString(value) + "\" out of range: [" +
                    toString(minValue) + ".." + toString(maxValue) + "] on option -" + m_optionChar,
                SOURCEINFO);
        }

        return value;
    }
};

}}

#endif