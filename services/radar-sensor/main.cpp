#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <random>
#include <string>

#include "traced_dds.hpp"
#include "CombatMessages.h"

TRACED_DDS_TYPE(combat_SourceTrack);

#define SERVICE_NAME "radar-sensor"
#define SENSOR_ID "RADAR-1"
#define SENSOR_TYPE "RADAR"

static volatile sig_atomic_t running = 1;

void handle_signal(int sig) { running = 0; }

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[%s] Starting radar sensor...\n", SERVICE_NAME);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) {
        fprintf(stderr, "Failed to create participant!\n");
        return 1;
    }

    auto writer = TRACED_WRITER(combat_SourceTrack, participant, "SourceTrackTopic");

    printf("[%s] DDS connected, waiting for discovery...\n", SERVICE_NAME);
    sleep(3);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> lat_dis(39.0, 41.0);
    std::uniform_real_distribution<> lon_dis(32.0, 34.0);
    std::uniform_real_distribution<> alt_dis(1000.0, 15000.0);
    std::uniform_real_distribution<> hdg_dis(0.0, 360.0);
    std::uniform_real_distribution<> spd_dis(100.0, 900.0);
    std::uniform_real_distribution<> conf_dis(0.7, 0.95);
    std::uniform_int_distribution<> class_dis(0, 2);

    const char* classifications[] = {"UNKNOWN", "HOSTILE", "NEUTRAL"};

    int track_num = 1;

    printf("[%s] Radar sensor operational - publishing source tracks\n", SERVICE_NAME);

    while (running) {
        char track_id[32];
        snprintf(track_id, sizeof(track_id), "R-%d", track_num);

        combat_SourceTrack msg;
        memset(&msg, 0, sizeof(msg));

        msg.sensor_id = (char*)SENSOR_ID;
        msg.sensor_type = (char*)SENSOR_TYPE;
        msg.timestamp_ns = time(NULL) * 1000000000LL;
        msg.source_track_id = track_id;
        msg.position_lat = lat_dis(gen);
        msg.position_lon = lon_dis(gen);
        msg.altitude_m = alt_dis(gen);
        msg.heading_deg = hdg_dis(gen);
        msg.speed_mps = spd_dis(gen);
        msg.confidence = conf_dis(gen);
        msg.classification = (char*)classifications[class_dis(gen)];

        if (writer.write(msg, "radar-detect")) {
            printf("[RADAR] Track %s | Pos: %.2f, %.2f | Alt: %.0fm | Conf: %.2f\n",
                   track_id, msg.position_lat, msg.position_lon, 
                   msg.altitude_m, msg.confidence);
        }

        track_num++;
        sleep(2);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    dds_delete(participant);

    return 0;
}
