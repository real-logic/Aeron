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
#ifndef AERON_COUNTERS_READER_H
#define AERON_COUNTERS_READER_H

#include <functional>

#include "util/Exceptions.h"
#include "util/BitUtil.h"
#include "AtomicBuffer.h"

#include "aeronc.h"

namespace aeron { namespace concurrent {

/**
 * Reads the counters metadata and values buffers.
 *
 * This class is threadsafe.
 *
 * <b>Values Buffer</b>
 * <pre>
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                        Counter Value                          |
 *  |                                                               |
 *  +---------------------------------------------------------------+
 *  |                       Registration Id                         |
 *  |                                                               |
 *  +---------------------------------------------------------------+
 *  |                     112 bytes of padding                     ...
 * ...                                                              |
 *  +---------------------------------------------------------------+
 *  |                   Repeats to end of buffer                   ...
 *  |                                                               |
 * ...                                                              |
 *  +---------------------------------------------------------------+
 * </pre>
 *
 * <b>Meta Data Buffer</b>
 * <pre>
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                        Record State                           |
 *  +---------------------------------------------------------------+
 *  |                          Type Id                              |
 *  +---------------------------------------------------------------+
 *  |                  Free-for-reuse Deadline (ms)                 |
 *  |                                                               |
 *  +---------------------------------------------------------------+
 *  |                      112 bytes for key                       ...
 * ...                                                              |
 *  +-+-------------------------------------------------------------+
 *  |R|                      Label Length                           |
 *  +-+-------------------------------------------------------------+
 *  |                  380 bytes of Label in ASCII                 ...
 * ...                                                              |
 *  +---------------------------------------------------------------+
 *  |                   Repeats to end of buffer                   ...
 *  |                                                               |
 * ...                                                              |
 *  +---------------------------------------------------------------+
 * </pre>
 */

typedef std::function<void(std::int32_t, std::int32_t, const AtomicBuffer&, const std::string&)> on_counters_metadata_t;

using namespace aeron::util;

class CountersReader
{

public:
    inline CountersReader(aeron_counters_reader_t *countersReader) : m_countersReader(countersReader)
    {
    }

    template <typename F>
    void forEach(F &&onCountersMetadata) const
    {
        using handler_type = typename std::remove_reference<F>::type;
        handler_type &handler = onCountersMetadata;
        void *handler_ptr = const_cast<void *>(reinterpret_cast<const void *>(&handler));

        aeron_counters_reader_foreach_counter(m_countersReader, NULL, handler_ptr);

        std::int32_t id = 0;

        const AtomicBuffer &metadataBuffer = metaDataBuffer();
        for (util::index_t i = 0, capacity = metadataBuffer.capacity(); i < capacity; i += METADATA_LENGTH)
        {
            std::int32_t recordStatus = metadataBuffer.getInt32Volatile(i);

            if (RECORD_UNUSED == recordStatus)
            {
                break;
            }
            else if (RECORD_ALLOCATED == recordStatus)
            {
                const auto &record = metadataBuffer.overlayStruct<CounterMetaDataDefn>(i);

                const std::string label = metadataBuffer.getString(i + LABEL_LENGTH_OFFSET);
                const AtomicBuffer keyBuffer(metadataBuffer.buffer() + i + KEY_OFFSET, sizeof(CounterMetaDataDefn::key));

                onCountersMetadata(id, record.typeId, keyBuffer, label);
            }

            id++;
        }
    }

    inline std::int32_t maxCounterId() const
    {
        return aeron_counters_reader_max_counter_id(m_countersReader);
    }

    inline std::int64_t getCounterValue(std::int32_t id) const
    {
        validateCounterId(id);
        int64_t *counter_addr = aeron_counters_reader_addr(m_countersReader, id);
        return *counter_addr;
    }

    inline std::int64_t getCounterRegistrationId(std::int32_t id) const
    {
        validateCounterId(id);

        std::int64_t registrationId;
        if (aeron_counters_reader_counter_registration_id(m_countersReader, id, &registrationId) < 0)
        {
            AERON_MAP_ERRNO_TO_SOURCED_EXCEPTION_AND_THROW;
        }

        return registrationId;
    }

    inline std::int32_t getCounterState(std::int32_t id) const
    {
        std::int32_t state;
        if (aeron_counters_reader_counter_state(m_countersReader, id, &state) < 0)
        {
            throw util::IllegalArgumentException(
                "counter id " + std::to_string(id) +
                    " out of range: maxCounterId=" + std::to_string(maxCounterId()),
                SOURCEINFO);
        }

        return state;
    }

    inline std::int64_t getFreeForReuseDeadline(std::int32_t id) const
    {
        std::int64_t deadline;
        if (aeron_counters_reader_free_for_reuse_deadline_ms(m_countersReader, id, &deadline))
        {
            throw util::IllegalArgumentException(
                "counter id " + std::to_string(id) +
                    " out of range: maxCounterId=" + std::to_string(maxCounterId()),
                SOURCEINFO);
        }

        return deadline;
    }

    inline std::string getCounterLabel(std::int32_t id) const
    {
        char buffer[AERON_COUNTERS_MAX_LABEL_LENGTH];
        int length = aeron_counters_reader_counter_label(m_countersReader, id, buffer, sizeof(buffer));
        if (length < 0)
        {
            throw util::IllegalArgumentException(
                "counter id " + std::to_string(id) +
                    " out of range: maxCounterId=" + std::to_string(maxCounterId()),
                SOURCEINFO);
        }

        return std::string(buffer, (size_t)length);
    }

#pragma pack(push)
#pragma pack(4)
    struct CounterValueDefn
    {
        std::int64_t counterValue;
        std::int64_t registrationId;
        std::int8_t padding[(2 * util::BitUtil::CACHE_LINE_LENGTH) - (2 * sizeof(std::int64_t))];
    };

    struct CounterMetaDataDefn
    {
        std::int32_t state;
        std::int32_t typeId;
        std::int64_t freeToReuseDeadline;
        std::int8_t key[(2 * util::BitUtil::CACHE_LINE_LENGTH) - (2 * sizeof(std::int32_t)) - sizeof(std::int64_t)];
        std::int32_t labelLength;
        std::int8_t label[(6 * util::BitUtil::CACHE_LINE_LENGTH) - sizeof(std::int32_t)];
    };
#pragma pack(pop)

    static const std::int32_t NULL_COUNTER_ID = -1;

    static const std::int32_t RECORD_UNUSED = 0;
    static const std::int32_t RECORD_ALLOCATED = 1;
    static const std::int32_t RECORD_RECLAIMED = -1;

    static const std::int64_t DEFAULT_REGISTRATION_ID = INT64_C(0);
    static const std::int64_t NOT_FREE_TO_REUSE = INT64_MAX;

    static const util::index_t COUNTER_LENGTH = sizeof(CounterValueDefn);
    static const util::index_t REGISTRATION_ID_OFFSET = offsetof(CounterValueDefn, registrationId);

    static const util::index_t METADATA_LENGTH = sizeof(CounterMetaDataDefn);
    static const util::index_t TYPE_ID_OFFSET = offsetof(CounterMetaDataDefn, typeId);
    static const util::index_t FREE_FOR_REUSE_DEADLINE_OFFSET = offsetof(CounterMetaDataDefn, freeToReuseDeadline);
    static const util::index_t KEY_OFFSET = offsetof(CounterMetaDataDefn, key);
    static const util::index_t LABEL_LENGTH_OFFSET = offsetof(CounterMetaDataDefn, labelLength);

    static const std::int32_t MAX_LABEL_LENGTH = sizeof(CounterMetaDataDefn::label);
    static const std::int32_t MAX_KEY_LENGTH = sizeof(CounterMetaDataDefn::key);

protected:
    aeron_counters_reader_t *m_countersReader;

    void validateCounterId(std::int32_t counterId) const
    {
        if (counterId < 0 || counterId > maxCounterId())
        {
            throw util::IllegalArgumentException(
                "counter id " + std::to_string(counterId) +
                " out of range: maxCounterId=" + std::to_string(maxCounterId()),
                SOURCEINFO);
        }
    }

    // typedef std::function<void(std::int32_t, std::int32_t, const AtomicBuffer&, const std::string&)> on_counters_metadata_t;

    template<typename H>
    static void forEachCounter(
        int64_t value,
        int32_t id,
        const uint8_t *key,
        size_t key_length,
        const char *label,
        size_t label_length,
        void *clientd)
    {
        H &handler = *reinterpret_cast<H *>(clientd);

        handler(id, 0, static_cast<util::index_t>(0), static_cast<util::index_t>(length), headerWrapper);
    }

};

}}

#endif
