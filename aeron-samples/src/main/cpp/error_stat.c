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

#include <errno.h>
#include <stdio.h>
#include <inttypes.h>

#ifndef _MSC_VER
#include <unistd.h>
#include <getopt.h>
#endif

#include "aeronc.h"
#include "aeron_common.h"
#include "aeron_cnc_file_descriptor.h"
#include "concurrent/aeron_thread.h"
#include "concurrent/aeron_mpsc_rb.h"
#include "concurrent/aeron_distinct_error_log.h"
#include "util/aeron_strutil.h"
#include "util/aeron_error.h"

typedef struct aeron_error_stat_setting_stct
{
    const char *base_path;
    int64_t timeout_ms;
}
aeron_error_stat_setting_t;

const char *usage()
{
    return
        "    -d basePath   Base Path to shared memory. Default: /dev/shm/aeron-mike\n"
        "    -h            Displays help information.\n"
        "    -t timeout    Number of milliseconds to wait to see if the driver metadata is available.  Default 1,000\n";
}

void print_error_and_usage(const char *message)
{
    fprintf(stderr, "%s\n%s", message, usage());
}

void aeron_error_stat_on_observation(
    int32_t observation_count,
    int64_t first_observation_timestamp,
    int64_t last_observation_timestamp,
    const char *error,
    size_t error_length,
    void *clientd)
{
    char first_timestamp[AERON_MAX_PATH];
    char last_timestamp[AERON_MAX_PATH];

    aeron_format_date(first_timestamp, sizeof(first_timestamp), first_observation_timestamp);
    aeron_format_date(last_timestamp, sizeof(last_timestamp), last_observation_timestamp);

    fprintf(
        stdout,
        "***\n%d observations from %s to %s for:\n %.*s\n",
        observation_count,
        first_timestamp,
        last_timestamp,
        (int)error_length,
        error);
}


int main(int argc, char **argv)
{
    char default_directory[AERON_MAX_PATH];
    aeron_default_path(default_directory, AERON_MAX_PATH);
    aeron_error_stat_setting_t settings = {
        .base_path = default_directory,
        .timeout_ms = 1000
    };

    int opt;

    while ((opt = getopt(argc, argv, "d:t:h")) != -1)
    {
        switch (opt)
        {
            case 'd':
                settings.base_path = optarg;
                break;

            case 't':
            {
                aeron_set_errno(0);
                char *endptr;
                settings.timeout_ms = strtoll(optarg, &endptr, 10);
                if (0 != errno || '\0' != endptr[0])
                {
                    print_error_and_usage("Invalid timeout");
                    return EXIT_FAILURE;
                }
                break;
            }

            case 'h':
                print_error_and_usage(argv[0]);
                return EXIT_SUCCESS;

            default:
                print_error_and_usage("Unknown option");
                return EXIT_FAILURE;
        }
    }

    aeron_cnc_metadata_t *cnc_metadata;
    aeron_mapped_file_t cnc_file = { 0 };
    const int64_t deadline_ms = aeron_epoch_clock() + settings.timeout_ms;

    do
    {
        aeron_cnc_load_result_t result = aeron_cnc_map_file_and_load_metadata(
            settings.base_path, &cnc_file, &cnc_metadata);

        if (AERON_CNC_LOAD_SUCCESS == result)
        {
            break;
        }
        else if (AERON_CNC_LOAD_FAILED == result)
        {
            print_error_and_usage(aeron_errmsg());
            return EXIT_FAILURE;
        }
        else
        {
            aeron_micro_sleep(16 * 1000);
        }

        if (deadline_ms <= aeron_epoch_clock())
        {
            print_error_and_usage("Timed out trying to get driver's CnC metadata");
            return EXIT_FAILURE;
        }
    }
    while (true);

    uint8_t *error_buffer = aeron_cnc_error_log_buffer(cnc_metadata);

    size_t count = aeron_error_log_read(
        error_buffer, cnc_metadata->error_log_buffer_length, aeron_error_stat_on_observation, NULL, 0);

    fprintf(stdout, "\n%" PRIu64 " distinct errors observed.\n", (uint64_t)count);

    aeron_unmap(&cnc_file);

    return 0;
}
