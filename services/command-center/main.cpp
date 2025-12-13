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

TRACED_DDS_TYPE(combat_MissionOrder);

#define SERVICE_NAME "command-center"

static volatile sig_atomic_t running = 1;

static const char* MISSION_TYPES[] = {"RECON", "STRIKE", "SUPPLY", "EVAC"};
static const char* PRIORITIES[] = {"LOW", "MEDIUM", "HIGH", "CRITICAL"};
static const char* ZONES[] = {"Alpha", "Bravo", "Charlie", "Delta"};

void handle_signal(int sig) { running = 0; }

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[%s] Starting command center...\n", SERVICE_NAME);

    // DDS setup
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) {
        fprintf(stderr, "Failed to create participant!\n");
        return 1;
    }

    // Traced writer - handles trace injection automatically
    auto writer = TRACED_WRITER(combat_MissionOrder, participant, "MissionOrderTopic");

    printf("[%s] DDS connected, waiting for discovery...\n", SERVICE_NAME);
    sleep(3);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> mission_dis(0, 3);
    std::uniform_int_distribution<> priority_dis(0, 3);
    std::uniform_int_distribution<> zone_dis(0, 3);
    std::uniform_real_distribution<> lat_dis(35.0, 42.0);
    std::uniform_real_distribution<> lon_dis(26.0, 45.0);
    std::uniform_int_distribution<> cmd_dis(1, 5);

    int sequence = 0;

    printf("[%s] Command center operational!\n", SERVICE_NAME);

    while (running) {
        char mission_id[64];
        snprintf(mission_id, sizeof(mission_id), "MSN-%ld-%d", time(NULL), sequence);

        const char* mission_type = MISSION_TYPES[mission_dis(gen)];
        const char* priority = PRIORITIES[priority_dis(gen)];
        const char* zone = ZONES[zone_dis(gen)];

        // Create message
        combat_MissionOrder msg;
        memset(&msg, 0, sizeof(msg));

        msg.source_service = (char*)SERVICE_NAME;
        msg.timestamp_ns = time(NULL) * 1000000000LL;
        msg.sequence_num = sequence;
        msg.mission_id = mission_id;
        msg.mission_type = (char*)mission_type;
        msg.priority = (char*)priority;
        msg.target_zone = (char*)zone;
        msg.target_lat = lat_dis(gen);
        msg.target_lon = lon_dis(gen);

        char cmd_id[16];
        snprintf(cmd_id, sizeof(cmd_id), "CMD-%d", cmd_dis(gen));
        msg.commander_id = cmd_id;

        // Write with automatic trace injection
        if (writer.write(msg, "issue-mission")) {
            printf("[ORDER] %s | Zone: %s | Priority: %s | ID: %s\n",
                   mission_type, zone, priority, mission_id);
        }

        sequence++;
        sleep(3);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    dds_delete(participant);

    return 0;
}
