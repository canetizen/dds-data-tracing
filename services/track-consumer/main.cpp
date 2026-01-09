#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <string>

#include "traced_dds.hpp"
#include "CombatMessages.h"

TRACED_DDS_TYPE(combat_TacticalTrack);

#define SERVICE_NAME "track-consumer"

static volatile sig_atomic_t running = 1;

void handle_signal(int sig) { running = 0; }

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[%s] Starting track consumer service...\n", SERVICE_NAME);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) {
        fprintf(stderr, "Failed to create participant!\n");
        return 1;
    }

    auto reader = TRACED_READER(combat_TacticalTrack, participant, "TacticalTrackTopic");

    printf("[%s] DDS connected, waiting for discovery...\n", SERVICE_NAME);
    sleep(3);

    printf("[%s] Consumer operational - listening for tactical tracks\n", SERVICE_NAME);

    while (running) {
        // Simple callback - no span parameter needed, tracing is automatic!
        reader.take_simple("process-tactical", [](combat_TacticalTrack& msg) {
            printf("\n[CONSUMER] ════════════════════════════════════════\n");
            printf("[CONSUMER] Received Tactical Track: %s\n", 
                   msg.tactical_track_id ? msg.tactical_track_id : "?");
            printf("[CONSUMER] From sources: %s\n",
                   msg.contributing_sensors ? msg.contributing_sensors : "?");
            printf("[CONSUMER] Source tracks: %s\n",
                   msg.contributing_track_ids ? msg.contributing_track_ids : "?");
            printf("[CONSUMER] Position: %.4f, %.4f | Alt: %.0fm\n",
                   msg.position_lat, msg.position_lon, msg.altitude_m);
            printf("[CONSUMER] Heading: %.1f° | Speed: %.1f m/s\n",
                   msg.heading_deg, msg.speed_mps);
            printf("[CONSUMER] Classification: %s | Confidence: %.2f\n",
                   msg.classification ? msg.classification : "?", msg.confidence);
            printf("[CONSUMER] ════════════════════════════════════════\n\n");
        });
        
        usleep(100000);  // 100ms poll interval
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    dds_delete(participant);

    return 0;
}
