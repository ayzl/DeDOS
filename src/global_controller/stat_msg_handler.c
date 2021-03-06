/*
START OF LICENSE STUB
    DeDOS: Declarative Dispersion-Oriented Software
    Copyright (C) 2017 University of Pennsylvania, Georgetown University

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
END OF LICENSE STUB
*/
#include "stat_msg_handler.h"
#include "timeseries.h"
#include "controller_stats.h"
#include "scheduling_decision.h"
#include "logging.h"
#include "scheduling.h"
#include "controller_dfg.h"
#include "controller_mysql.h"

#define MAX_STAT_SAMPLES 128
#define MAX_SAMPLE_SIZE 64
struct stat_sample *incoming_samples;

static int process_stat_sample(int runtime_id, struct stat_sample *sample) {
    int rtn = db_insert_sample(sample->stats, &sample->hdr, runtime_id);
    if (rtn < 0) {
        log(LOG_MYSQL,"Error inserting stats %d.%u into DB",
            sample->hdr.stat_id, sample->hdr.item_id);
    }

    struct timed_rrdb *stat;
    if (is_thread_stat(sample->hdr.stat_id)) {
        stat = get_thread_stat(sample->hdr.stat_id, runtime_id, sample->hdr.item_id);
    } else if (is_msu_stat(sample->hdr.stat_id)) {
        stat = get_msu_stat(sample->hdr.stat_id, sample->hdr.item_id);
    } else {
        log_error("Cannot find statistic %d", sample->hdr.stat_id);
        return -1;
    }

    if (stat == NULL) {
        log(LOG_PROCESS_STATS, "Error getting stat sample %d.%u",
            sample->hdr.stat_id, sample->hdr.item_id);
        return -1;
    }
    rtn = append_to_timeseries(sample->stats, sample->hdr.n_stats, stat);
    if (rtn < 0) {
        log_error("Error appending stats %d.%u to timeseries",
                  sample->hdr.stat_id, sample->hdr.item_id);
        return -1;
    }


    return 0;
}

int handle_serialized_stats_buffer(int runtime_id, void *buffer, size_t buffer_len) {
    if (incoming_samples == NULL) {
        log_error("Stat msg handler not initialized");
        return -1;
    }

    int n_samples = deserialize_stat_samples(buffer, buffer_len,
                                             incoming_samples, MAX_STAT_SAMPLES);
    if (n_samples < 0) {
        log_error("Error deserializing stat samples");
        return -1;
    }

    for (int i=0; i<n_samples; i++) {
        if (process_stat_sample(runtime_id, &incoming_samples[i]) != 0) {
            //log_error("Error processing stat sample");
        }
    }

#ifdef DYN_SCHED
    perform_cloning();
    fix_all_route_ranges(get_dfg());
#endif

    return 0;
}

int init_stats_msg_handler() {
    incoming_samples = init_stat_samples(MAX_STAT_SAMPLES, MAX_SAMPLE_SIZE);
    if (incoming_samples == NULL) {
        log_error("Error initializing stat samples");
        return -1;
    }
    return 0;
}
