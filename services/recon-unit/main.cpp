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
TRACED_DDS_TYPE(combat_ReconReport);

#define SERVICE_NAME "recon-unit"

static volatile sig_atomic_t running = 1;
static const char* THREAT_LEVELS[] = {"NONE", "LOW", "MEDIUM", "HIGH", "EXTREME"};
static const char* TERRAIN_TYPES[] = {"URBAN", "FOREST", "DESERT", "MOUNTAIN"};

void handle_signal(int sig) { running = 0; }

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    printf("[%s] Starting reconnaissance unit...\n", SERVICE_NAME);

    // DDS setup
    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) { fprintf(stderr, "Failed to create participant!\n"); return 1; }

    auto reader = TRACED_READER(combat_MissionOrder, participant, "MissionOrderTopic");
    auto writer = TRACED_WRITER(combat_ReconReport, participant, "ReconReportTopic");

    printf("[%s] DDS connected, waiting for discovery...\n", SERVICE_NAME);
    sleep(3);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_real_distribution<> confirm_dis(0.0, 1.0);
    std::uniform_int_distribution<> enemy_dis(0, 50);
    std::uniform_int_distribution<> threat_dis(0, 4);
    std::uniform_int_distribution<> terrain_dis(0, 3);
    std::uniform_int_distribution<> unit_dis(1, 5);

    printf("[%s] Recon unit ready, awaiting mission orders...\n", SERVICE_NAME);

    while (running) {
        // Take messages with automatic trace extraction and child span creation
        reader.take("execute-recon", [&](combat_MissionOrder& order, traced::trace_api::Span& span) {
            printf("[RECON] Mission: %s | Zone: %s | Priority: %s\n",
                   order.mission_type, order.target_zone, order.priority);

            span.SetAttribute("mission.id", order.mission_id ? order.mission_id : "");
            span.SetAttribute("mission.type", order.mission_type ? order.mission_type : "");
            span.SetAttribute("mission.zone", order.target_zone ? order.target_zone : "");

            // Simulate reconnaissance
            usleep(500000 + (rand() % 1000000));

            bool target_confirmed = confirm_dis(gen) > 0.20;
            int enemy_count = target_confirmed ? enemy_dis(gen) : 0;
            const char* threat_level = THREAT_LEVELS[threat_dis(gen)];
            const char* terrain = TERRAIN_TYPES[terrain_dis(gen)];

            span.SetAttribute("recon.target_confirmed", target_confirmed);
            span.SetAttribute("recon.enemy_count", enemy_count);
            span.SetAttribute("recon.threat_level", threat_level);

            printf("[INTEL] %s | Enemies: %d | Threat: %s | Terrain: %s\n",
                   target_confirmed ? "TARGET CONFIRMED" : "TARGET NOT FOUND",
                   enemy_count, threat_level, terrain);

            // Create report
            combat_ReconReport report;
            memset(&report, 0, sizeof(report));

            report.source_service = (char*)SERVICE_NAME;
            report.timestamp_ns = time(NULL) * 1000000000LL;
            report.mission_id = order.mission_id;

            char report_id[64];
            snprintf(report_id, sizeof(report_id), "RPT-%ld", time(NULL));
            report.report_id = report_id;

            char unit_id[16];
            snprintf(unit_id, sizeof(unit_id), "UNIT-%d", unit_dis(gen));
            report.unit_id = unit_id;

            report.target_confirmed = target_confirmed;
            report.enemy_count = enemy_count;
            report.threat_level = (char*)threat_level;
            report.terrain_type = (char*)terrain;
            report.intel_details = (char*)"{}";

            // Forward - trace context automatically propagated by middleware
            writer.write(report, "send-report");

            if (!target_confirmed) {
                span.SetStatus(traced::trace_api::StatusCode::kError, "Target not found");
            }
        });

        usleep(100000);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    dds_delete(participant);
    return 0;
}
