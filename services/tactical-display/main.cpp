#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <string>
#include <map>

#include "traced_dds.hpp"
#include "CombatMessages.h"

TRACED_DDS_TYPE(combat_MissionOrder);
TRACED_DDS_TYPE(combat_ReconReport);
TRACED_DDS_TYPE(combat_SupplyUpdate);

#define SERVICE_NAME "tactical-display"

static volatile sig_atomic_t running = 1;

struct CombatStats {
    int total_missions = 0;
    int targets_confirmed = 0;
    int targets_not_found = 0;
    int supplies_dispatched = 0;
    int alerts_generated = 0;
    std::map<std::string, int> by_zone;
    std::map<std::string, int> by_threat;
    time_t start_time;
} combat_stats;

void handle_signal(int sig) { running = 0; }

void print_tactical_display() {
    int uptime = (int)(time(NULL) - combat_stats.start_time);
    float success_rate = combat_stats.targets_confirmed + combat_stats.targets_not_found > 0
        ? (float)combat_stats.targets_confirmed / (combat_stats.targets_confirmed + combat_stats.targets_not_found) * 100
        : 100.0;

    printf("\n");
    printf("+============================================================+\n");
    printf("|           TACTICAL COMMAND DISPLAY                         |\n");
    printf("+============================================================+\n");
    printf("|  Uptime: %6d seconds                                    |\n", uptime);
    printf("+------------------------------------------------------------+\n");
    printf("|  Total Missions:    %5d                                  |\n", combat_stats.total_missions);
    printf("|  Targets Confirmed: %5d   (%.1f%%)                        |\n", combat_stats.targets_confirmed, success_rate);
    printf("|  Targets Not Found: %5d                                  |\n", combat_stats.targets_not_found);
    printf("|  Supplies Sent:     %5d                                  |\n", combat_stats.supplies_dispatched);
    printf("|  Total Alerts:      %5d                                  |\n", combat_stats.alerts_generated);
    printf("+------------------------------------------------------------+\n");
    printf("|  Operations by Zone:                                       |\n");

    for (auto& kv : combat_stats.by_zone) {
        printf("|    %-8s: %5d missions                                 |\n",
               kv.first.c_str(), kv.second);
    }

    if (!combat_stats.by_threat.empty()) {
        printf("+------------------------------------------------------------+\n");
        printf("|  Threat Level Distribution:                                |\n");
        for (auto& kv : combat_stats.by_threat) {
            printf("|    %-10s: %3d                                         |\n",
                   kv.first.c_str(), kv.second);
        }
    }

    printf("+============================================================+\n\n");
}

int main() {
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    combat_stats.start_time = time(NULL);

    printf("[%s] Starting tactical display system...\n", SERVICE_NAME);

    dds_entity_t participant = dds_create_participant(DDS_DOMAIN_DEFAULT, NULL, NULL);
    if (participant < 0) { fprintf(stderr, "Failed to create participant!\n"); return 1; }

    auto mission_reader = TRACED_READER(combat_MissionOrder, participant, "MissionOrderTopic");
    auto recon_reader = TRACED_READER(combat_ReconReport, participant, "ReconReportTopic");
    auto supply_reader = TRACED_READER(combat_SupplyUpdate, participant, "SupplyUpdateTopic");

    printf("[%s] DDS connected...\n", SERVICE_NAME);
    sleep(3);

    time_t last_display = time(NULL);

    printf("[%s] Tactical display ready, monitoring operations...\n", SERVICE_NAME);

    while (running) {
        // Process mission orders
        mission_reader.take("display-mission", [](combat_MissionOrder& order, traced::trace_api::Span& span) {
            combat_stats.total_missions++;
            std::string zone = order.target_zone ? order.target_zone : "Unknown";
            combat_stats.by_zone[zone]++;

            span.SetAttribute("mission.type", order.mission_type ? order.mission_type : "");
            span.SetAttribute("mission.zone", zone);
            span.SetAttribute("display.total_missions", combat_stats.total_missions);

            printf("[DISPLAY] NEW MISSION: %s | Zone: %s | Priority: %s\n",
                   order.mission_type, zone.c_str(),
                   order.priority ? order.priority : "?");
        });

        // Process recon reports
        recon_reader.take("display-intel", [](combat_ReconReport& report, traced::trace_api::Span& span) {
            if (report.target_confirmed) {
                combat_stats.targets_confirmed++;
            } else {
                combat_stats.targets_not_found++;
            }

            std::string threat = report.threat_level ? report.threat_level : "UNKNOWN";
            combat_stats.by_threat[threat]++;

            span.SetAttribute("recon.target_confirmed", report.target_confirmed);
            span.SetAttribute("recon.threat_level", threat);
            span.SetAttribute("recon.enemy_count", report.enemy_count);

            printf("[DISPLAY] INTEL: %s | Threat: %s | Enemies: %d\n",
                   report.target_confirmed ? "TARGET CONFIRMED" : "NOT FOUND",
                   threat.c_str(), report.enemy_count);

            if (threat == "EXTREME" || threat == "HIGH") {
                combat_stats.alerts_generated++;
                printf("\n[ALERT] High threat detected: %s level!\n\n", threat.c_str());
                span.AddEvent("high_threat_alert");
            }
        });

        // Process supply updates
        supply_reader.take("display-logistics", [](combat_SupplyUpdate& update, traced::trace_api::Span& span) {
            combat_stats.supplies_dispatched += update.quantity;

            span.SetAttribute("supply.type", update.supply_type ? update.supply_type : "");
            span.SetAttribute("supply.quantity", update.quantity);
            span.SetAttribute("depot.stock", update.current_stock);

            printf("[DISPLAY] SUPPLY: %s x%d from %s | Stock: %d\n",
                   update.supply_type, update.quantity,
                   update.depot_location ? update.depot_location : "?",
                   update.current_stock);

            if (update.low_stock_alert) {
                combat_stats.alerts_generated++;
                printf("\n[WARNING] Low stock for %s at %s!\n\n",
                       update.supply_type, update.depot_location);
                span.AddEvent("low_stock_alert");
            }
        });

        if (time(NULL) - last_display >= 25) {
            print_tactical_display();
            last_display = time(NULL);
        }

        usleep(100000);
    }

    printf("[%s] Shutting down...\n", SERVICE_NAME);
    dds_delete(participant);
    return 0;
}
